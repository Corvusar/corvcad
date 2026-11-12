#!/usr/bin/env python3
"""Classify measured upstream files using retained file and component notices."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent


def cc_license(text):
    match = re.search(r"creativecommons.org/licenses/(by(?:-sa|-nc(?:-sa|-nd)?|-nd)?)/(\d\.\d)", text)
    return "CC-"+match[1].upper()+"-"+match[2] if match else None


def document_notice(path):
    with zipfile.ZipFile(path) as archive:
        root = ET.fromstring(archive.read("Document.xml"))
    properties = root.find("Properties")
    values, blocks = {}, []
    if properties is not None:
        for item in properties:
            if item.attrib.get("name") in ("License", "LicenseURL", "CreatedBy"):
                string = item.find("String")
                if string is not None:
                    values[item.attrib["name"]] = string.attrib.get("value", "")
                    blocks.append(ET.tostring(item, encoding="unicode"))
    expression = cc_license(values.get("LicenseURL", ""))
    if not expression:
        expression = ("LicenseRef-All-Rights-Reserved" if "all rights reserved" in values.get("License", "").lower()
                      else "LicenseRef-No-Document-Grant")
    return expression, "\n".join(blocks)


def classify(name, text, policy):
    if name in policy.get("overrides", {}):
        record = policy["overrides"][name]
        result = {"expression": record["expression"], "basis": "reviewed file exception", "evidence": record["evidence"]}
        if "additional_notice" in record:
            result["additional_notice"] = record["additional_notice"]
        return result
    declarations = list(dict.fromkeys(re.findall(r"SPDX-License-Identifier:\s*([^\r\n*]+)", text)))
    if declarations:
        return {"expression": " AND ".join("("+x.strip()+")" for x in declarations), "basis": "file SPDX",
                "evidence": name, "declarations": declarations}
    if name.endswith(".FCMat"):
        match = re.search(r'^\s*License:\s*"([^"\r\n]+)"', text, re.MULTILINE)
        if match:
            expression = match[1]
            if expression == "All rights reserved":
                expression = "LicenseRef-All-Rights-Reserved"
            elif not re.fullmatch(r"(?:LGPL|GPL|CC-BY(?:-SA)?)-\d\.\d(?:-or-later)?", expression):
                raise ValueError(f"Unreviewed material licence: {name}: {expression}")
            return {"expression": expression, "basis": "material metadata", "evidence": name}
    cc = cc_license(text)
    if cc and name.endswith((".stp", ".step", ".svg")):
        return {"expression": cc, "basis": "asset grant", "evidence": name}
    shorthand = re.search(r"Licen[cs]e:\s*(LGPL-\d\.\d-or-later)", text[:3000])
    if shorthand:
        return {"expression": shorthand[1], "basis": "file grant", "evidence": name}
    # Match the actual grant, not later documentation mentioning another GPL.
    banner = re.search(r"(?:GNU\s+(?:Lesser|Library)\s+General\s+Public|GNU\s+General\s+Public)\s*License.{0,240}?version\s+(\d(?:\.\d)?).{0,100}?(?:any\s+later\s+version|or\s+later)",
                       re.sub(r"[\s*#]+", " ", text), re.IGNORECASE)
    if banner:
        family = "LGPL" if re.search("Lesser|Library", banner[0], re.IGNORECASE) else "GPL"
        version = banner[1] if "." in banner[1] else banner[1]+".0"
        return {"expression": family+"-"+version+"-or-later", "basis": "file grant", "evidence": name}
    for prefix, record in sorted(policy["vendored"].items(), key=lambda item: -len(item[0])):
        if name.startswith(prefix):
            return {"expression": record["expression"], "basis": "component grant", "evidence": record["evidence"]}
    if name.startswith("src/3rdParty/"):
        raise ValueError(f"No reviewed component grant for {name}")
    default = policy["project_default"]
    return {"expression": default["expression"], "basis": "upstream application default", "evidence": default["evidence"]}


def catalog(source, records, output):
    policy = json.loads((HERE/"license-policy.json").read_text())
    result, evidence, excerpts, excluded = {}, {}, {}, {}
    for name in sorted(records):
        data = (source/name).read_bytes()
        text = data.decode("utf-8", errors="replace")
        if name.lower().endswith(".fcstd"):
            expression, notice = document_notice(source/name)
            classification = {"expression": expression, "basis": "document metadata", "evidence": name}
        else:
            classification = classify(name, text, policy)
        result[name] = classification
        path = source/classification["evidence"]
        evidence_data = path.read_bytes()
        if b"\0" not in evidence_data:
            evidence_data = evidence_data.replace(b"\r\n", b"\n")
        evidence[classification["evidence"]] = hashlib.sha256(evidence_data).hexdigest()
        if not name.lower().endswith(".fcstd"):
            notice = path.read_text(encoding="utf-8", errors="replace")
        if path.suffix == ".FCMat":
            general = re.search(r"^General:\s*\n[\s\S]*?(?=^[^\s#]|\Z)", notice, re.MULTILINE)
            if not general:
                raise ValueError(f"Missing material attribution block: {name}")
            notice = general[0]
        if path.suffix.lower() in (".c", ".cpp", ".h", ".hpp", ".hxx", ".py", ".pyi", ".xml", ".stp", ".step"):
            blocks = re.findall(r"/\*[\s\S]*?\*/|<!--[\s\S]*?-->|(?:(?:^\s*//[^\n]*(?:\n|$))+)|(?:(?:^\s*\#[^\n]*(?:\n|$))+)", notice, re.MULTILINE)
            if path.suffix in (".py", ".pyi"):
                blocks += re.findall(r'"""[\s\S]*?"""|\x27{3}[\s\S]*?\x27{3}', notice)
            notice = "\n\n".join(b for b in blocks if re.search(r"SPDX|copyright|licen[cs]e|permission|special exception", b, re.IGNORECASE))
            if not notice:
                raise ValueError(f"No retainable licence excerpt: {classification['evidence']}")
        excerpts[classification["evidence"]] = notice
        if classification["expression"] in ("LicenseRef-All-Rights-Reserved", "LicenseRef-No-Document-Grant", "LicenseRef-OSIFONT-FontException"):
            excluded[name] = {"sha256": hashlib.sha256(data).hexdigest(),
                              "sha256_lf": hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest(),
                              "reason": classification["expression"]}
    origins = json.loads((HERE/"supplemental-notices/origins.json").read_text())
    for name, record in origins.items():
        if hashlib.sha256((HERE/"supplemental-notices"/name).read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError("Supplemental notice checksum mismatch")
    report = {"schema": 1, "policy_sha256": hashlib.sha256((HERE/"license-policy.json").read_bytes()).hexdigest(),
              "supplemental_notices": origins,
              "files": result, "evidence_sha256_lf": evidence, "retained_notice_excerpts": excerpts,
              "production_payload_exclusions": excluded,
              "scope": "Measured upstream inputs; file notices retained in pinned source. Not distribution approval."}
    output.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8", newline="\n")
    print(f"Classified {len(result)} files using {len(evidence)} file/component notice inputs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    catalog(args.source, json.loads(args.inventory.read_text())["source_files"], args.output)

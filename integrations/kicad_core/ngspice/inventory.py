"""Licence inventory of the ngspice files in a libngspice.a from ngspice_ios.sh. For every object
the library was packed from (<build>/libngspice.inputs; archive members through `ar t`), the
compiler's dependency record names its source and the ngspice headers it read. Each file gets its
LF-normalised SHA-256 and its licence under ngspice's COPYING rules (or its own header where that
differs). The code-model generator's inputs (cfunc.mod, ifspec.ifs, the model lists) and the
generator itself (cmpp, run at build time, not linked) are recorded too. Files whose text mentions
another licence family (GPL, LGPL, MPL, a BSD advertising clause) are flagged for review.
  inventory.py <ngspice-work-dir> <build-dir-name> <out.json>   e.g. ~/ngspice build-device audit/ngspice.json
macOS (uses ar)."""
import glob
import hashlib
import json
import os
import re
import subprocess
import sys

# COPYING ("files with licenses different to 'Modified BSD'"), most specific first; dlmain.c's
# own header says BSD-3-Clause although it sits in the public-domain xspice tree.
RULES = [
    ("src/xspice/icm/dlmain.c", "BSD-3-Clause (its header)"),
    ("src/xspice/icm/table/", "GPL-2.0-or-later"),
    ("src/xspice/", "public domain (Georgia Tech Research Corporation)"),
    ("src/frontend/numparam/", "LGPL-2.0-or-later"),
    ("src/maths/KLU/", "LGPL-2.0"),
    ("src/include/ngspice/klu", "LGPL-2.0"),
    ("src/maths/sparse/", "MIT (unnamed, per COPYING)"),
    ("src/spicelib/devices/ndev/", "public domain"),
    ("src/osdi/", "MPL-2.0"),
    ("src/tclspice.c", "LGPL-2.0"),
]
# Bison-generated parsers carry the skeleton's GPL with the exception that frees generated parsers.
BISON = re.compile(r"special exception, you may create a larger work that contains\s+part or all of the Bison parser skeleton")
MENTIONS = {
    "GPL": re.compile(r"GNU General Public License"),
    "LGPL": re.compile(r"GNU (Lesser|Library) General Public"),
    "MPL": re.compile(r"Mozilla Public License"),
    "BSD advertising clause": re.compile(r"advertising materials", re.I),
}


def licence(rel):
    return next((lic for prefix, lic in RULES if rel.startswith(prefix)), "BSD-3-Clause")


def dep_record(obj_dir, stem):
    """The dependency file for object <stem>.o compiled in obj_dir, and the directory its relative
    paths start from."""
    for d in (obj_dir, os.path.dirname(obj_dir)):  # libtool puts objects in .libs/
        for ext in (".Plo", ".Po", ".P"):
            path = os.path.join(d, ".deps", stem + ext)
            if os.path.isfile(path):
                return path, d
    raise SystemExit(f"no dependency record for {os.path.join(obj_dir, stem)}.o")


def main():
    work, build, out = sys.argv[1:]
    top = next(os.path.join(work, d) for d in os.listdir(work) if re.fullmatch(r"ngspice-\d+", d))
    icm = os.path.join(work, build, "src", "xspice", "icm")
    objects = []  # (directory, stem)
    for line in open(os.path.join(work, build, "libngspice.inputs")):
        path = os.path.join(work, line.strip())
        if path.endswith(".a"):
            members = subprocess.run(["ar", "t", path], capture_output=True, text=True, check=True).stdout.split()
            objects += [(os.path.dirname(path), m[:-2]) for m in members if m.endswith(".o")]
        else:
            objects.append((os.path.dirname(path), os.path.basename(path)[:-2]))
    roles = {}
    for obj_dir, stem in objects:
        record, base = dep_record(obj_dir, stem)
        if record.startswith(icm + os.sep):
            base = icm  # the code models are compiled from src/xspice/icm/GNUmakefile
        text = open(record, encoding="latin-1").read().replace("\\\n", " ")
        for token in text.split():
            if token.endswith(":"):
                continue  # targets
            path = os.path.normpath(token if os.path.isabs(token) else os.path.join(base, token))
            if path.startswith(top + os.sep) and os.path.isfile(path):
                roles.setdefault(os.path.relpath(path, top), "compiled")
        if stem in ("cfunc", "ifspec") and obj_dir.startswith(icm + os.sep):
            model = os.path.relpath(obj_dir, icm)
            source = os.path.join("src/xspice/icm", model, "cfunc.mod" if stem == "cfunc" else "ifspec.ifs")
            roles[source] = "code-model source (cmpp generates the compiled C)"
            for lst in ("modpath.lst", "udnpath.lst"):
                roles[os.path.join("src/xspice/icm", os.path.dirname(model), lst)] = "code-model list (cmpp)"
        if stem == "dlmain_helpers":
            roles["src/xspice/icm/dlmain.c"] = "compiled in part (its helpers; ngspice_ios.sh)"
    for path in glob.glob(os.path.join(top, "src/xspice/cmpp/*.[chly]")):
        roles.setdefault(os.path.relpath(path, top), "generator (cmpp, run at build time, not linked)")
    rows, flagged, counts = [], [], {}
    for rel in sorted(roles):
        data = open(os.path.join(top, rel), "rb").read().replace(b"\r\n", b"\n")
        text = data.decode("latin-1")
        lic = licence(rel)
        if BISON.search(text):
            lic += " (generated; Bison skeleton GPL-3.0-or-later WITH Bison-exception-2.2)"
        row = {"path": rel, "role": roles[rel], "sha256_lf": hashlib.sha256(data).hexdigest(), "license": lic}
        mentions = [k for k, r in MENTIONS.items() if r.search(text)]
        if mentions:
            row["mentions"] = mentions
            flagged.append(rel)
        rows.append(row)
        counts[lic] = counts.get(lic, 0) + 1
    copying = open(os.path.join(top, "COPYING"), "rb").read().replace(b"\r\n", b"\n")
    json.dump({"ngspice": os.path.basename(top), "build": build, "objects": len(objects),
               "copying_sha256_lf": hashlib.sha256(copying).hexdigest(),
               "license_counts": counts, "files": rows}, open(out, "w"), indent=1)
    print(f"{len(objects)} objects, {len(rows)} files: {counts}")
    print(f"{len(flagged)} mention another licence: {flagged}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Read-only verification of the measured F2 source snapshot, not audit approval."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from retain_inventory import safe_relative

HERE = Path(__file__).resolve().parent


def digest(path, lf=False):
    data = path.read_bytes()
    if lf and b"\0" not in data:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def check_file(root, name, expected, lf=False):
    path = root / safe_relative(name)
    if not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"Missing or escaping input: {name}")
    # Some vendored files are CRLF even in the upstream Git blob. Match their
    # measured raw bytes first, then allow checkout CRLF for an LF measurement.
    if digest(path) != expected and (not lf or digest(path, True) != expected):
        raise ValueError(f"Input checksum mismatch: {name}")


def verify(source, audit=None, integration=None):
    integration = HERE if integration is None else integration
    audit = integration / "audit" if audit is None else audit
    report = json.loads((audit / "desktop-inputs.json").read_text())
    profile = json.loads((integration / "slice.json").read_text())
    pin = json.loads((integration / "upstream.json").read_text())["source"]["commit"]
    if report["source_commit"] != pin or report["profile_sha256"] != digest(integration / "slice.json", True):
        raise ValueError("Inventory source/profile does not match current integration")

    def git(*args):
        return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()

    if git("rev-parse", "HEAD") != pin:
        raise ValueError("Source checkout is not at the recorded pin")
    expected_edits = {}
    for patch in profile["patches"]:
        check_file(integration, patch["path"], patch["sha256"], True)
        expected_edits.update(patch["files"])
    changed = set(git("diff", "HEAD", "--name-only").splitlines())
    if changed != expected_edits.keys() or git("ls-files", "--others", "--exclude-standard"):
        raise ValueError("Expected the complete declared patch state with no undeclared edits")
    for name, record in expected_edits.items():
        check_file(source, name, record["after_sha256_lf"], True)
    retained = profile["retained_license"]
    check_file(integration, retained["path"], retained["sha256_lf"], True)
    for name, record in report["source_files"].items():
        check_file(source, name, record["sha256"], True)
    # Notice bytes are retained exactly; unlike source text their archive hash
    # must not change through newline conversion.
    for name, expected in report["source_tree_notices"].items():
        check_file(audit / "notices", name, expected)
    for platform in report.get("platforms", {}).values():
        packages = platform["host_packages"]
        check_file(audit, packages["path"], packages["sha256"])
    return {"status": "source snapshot verified; audit review remains open",
            "source_inputs": len(report["source_files"]),
            "notices": len(report["source_tree_notices"])}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Pinned, fully patched checkout")
    args = parser.parse_args()
    print(json.dumps(verify(args.source.resolve())))

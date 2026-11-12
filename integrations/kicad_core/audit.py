#!/usr/bin/env python3
"""Capture or verify gate 1's measured source and dependency inventory (stdlib only).

Capture on the macOS proof host:
  audit.py capture --kicad ~/kicad-core/kicad --build build/kicad-probe \
      --vcpkg build/ios-deps/vcpkg --installed build/ios-deps/installed \
      --wx build/wx-ios/wxWidgets-3.3.4 --output build/gate1-audit
Verify source/patch drift on any host, after fetch.py --source-only:
  audit.py verify --kicad build/kicad-core/kicad --inventory integrations/kicad_core/audit

Only reads the shared build tree. Patches are replayed into an isolated Git index;
the checkout, user's index and existing build products are never rewritten.
"""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
LOCK = json.loads((HERE / "upstream.json").read_text(encoding="utf-8"))
POLICY = json.loads((HERE / "license-policy.json").read_text(encoding="utf-8"))
# Bundled official KiCad libraries, pinned in upstream.json (NATIVE_KICAD edition).
LIBRARY_PINS = ("symbol_libraries", "footprint_libraries")
PROFILES = {
    "schematic": ("probe_headless", ["probe/probe.cpp"]),
    "board": ("pcb_probe_headless", ["probe/pcb_probe.cpp"]),
    "session": ("session", ["session/kicad_session.cpp", "session/abi_test.c",
                             "../../engine/src/schematic_route.cpp"]),
    # What the NATIVE_KICAD app links (gate 5): the session plus the board checks.
    "native": ("native", ["session/kicad_session.cpp", "session/kicad_board.cpp", "session/board_test.c",
                          "../../engine/src/schematic_route.cpp"]),
}


def canonical(data):
    """Source-text hashes use LF so Git's Windows checkout conversion is harmless."""
    return data.replace(b"\r\n", b"\n")


def sha(data):
    return hashlib.sha256(canonical(data)).hexdigest()


def run(*args, cwd=None, env=None, data=None):
    result = subprocess.run(args, cwd=cwd, env=env, input=data, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def git(root, *args, **kw):
    return run("git", "-C", str(root), *args, **kw)


def patches():
    listed = LOCK["patches"]
    actual = sorted(p.relative_to(HERE).as_posix() for p in (HERE / "patches").glob("*.patch"))
    if len(set(listed)) != len(listed) or sorted(listed) != actual:
        raise RuntimeError("upstream.json must list every patch exactly once, in application order")
    return [HERE / p for p in listed]


class SourceTree:
    """Expected upstream blobs after the complete declared patch series."""
    def __init__(self, root):
        self.root = root.resolve()
        actual = git(self.root, "rev-parse", "HEAD").decode().strip()
        if actual != LOCK["schematic_source"]["commit"]:
            raise RuntimeError(f"Wrong KiCad source commit: {actual}")
        # An absent temporary index is required: an empty file is not a Git index.
        with tempfile.TemporaryDirectory(prefix="corvcad-audit-") as tmp:
            env = {**os.environ, "GIT_INDEX_FILE": str(Path(tmp) / "index")}
            git(self.root, "read-tree", "HEAD", env=env)
            for patch in patches():
                git(self.root, "apply", "--cached", "--whitespace=nowarn", "-",
                    env=env, data=canonical(patch.read_bytes()))
            self.changed = git(self.root, "diff", "--cached", "--name-only", "HEAD",
                               env=env).decode().splitlines()
            rows = git(self.root, "ls-files", "--stage", "-z", env=env).split(b"\0")
            self.blobs = {}
            for row in filter(None, rows):
                info, path = row.split(b"\t", 1)
                self.blobs[path.decode()] = info.split()[1].decode()
        # Batch object reads avoid thousands of Git processes on Windows.
        self.cache = {}

    def read_many(self, paths):
        missing = sorted(set(paths) - self.cache.keys())
        if not missing:
            return
        for p in missing:
            if p not in self.blobs:
                raise RuntimeError(f"Input absent from pinned source + patch series: {p}")
        output = run("git", "-C", str(self.root), "cat-file", "--batch",
                     data="".join(self.blobs[p] + "\n" for p in missing).encode())
        pos = 0
        for p in missing:
            end = output.index(b"\n", pos)
            size = int(output[pos:end].split()[-1])
            self.cache[p] = output[end + 1:end + 1 + size]
            pos = end + 2 + size

    def read(self, path):
        self.read_many([path])
        return self.cache[path]

    def check_worktree(self, paths):
        """Reject even unused edits to patched files; sentinel version files are not sources."""
        paths = set(paths) | set(self.changed)
        self.read_many(paths)
        for p in sorted(paths):
            file = self.root / p
            if not file.is_file() or sha(file.read_bytes()) != sha(self.read(p)):
                raise RuntimeError(f"KiCad worktree differs from declared patch series: {p}")
        dirty = git(self.root, "diff", "HEAD", "--name-only").decode().splitlines()
        untracked = git(self.root, "ls-files", "--others", "--exclude-standard").decode().splitlines()
        ignored = {LOCK["schematic_source"]["tag"], LOCK["schematic_source"]["tag"] + "-dirty"}
        extras = (set(dirty) | set(untracked)) - set(self.changed) - ignored
        if extras:
            raise RuntimeError("Undeclared upstream changes: " + ", ".join(sorted(extras)))


def portable_closure(line):
    """Old maps contain /Users/<name> paths; never retain those in a release inventory."""
    if "integrations/kicad_core/" in line:
        return "corvcad/integrations/kicad_core/" + line.split("integrations/kicad_core/", 1)[1]
    if line.startswith("generated/"):
        return line
    return "kicad/" + line


def license_info(key, data):
    text = data.decode("utf-8", errors="replace")
    expressions, evidence = [], []
    upstream = key.removeprefix("kicad/")
    if Path(key).name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "AUTHORS")):
        return {"expression": "LicenseRef-Notice-Text", "evidence": ["Preserve this notice verbatim"]}
    if upstream in POLICY["overrides"]:
        expr, why = POLICY["overrides"][upstream]
        return {"expression": expr, "evidence": [why]}
    if key.startswith("kicad/thirdparty/"):
        component = upstream.split("/")[1]
        if component not in POLICY["vendored"]:
            raise RuntimeError(f"Unreviewed vendored component: {component}")
        expr, *notices = POLICY["vendored"][component]
        expressions.append(expr)
        evidence += ["kicad/" + p for p in notices]
    else:
        # A file may contain more than one licensed excerpt, so SPDX additions must
        # not mask its enclosing KiCad GPL notice (e.g. json_common.h).
        comment_text = re.sub(r"(?m)^\s*(?:\*|//)\s?", "", text[:6500])
        gpl = re.search(r"either version\s+([23])\s+of the License", comment_text)
        if gpl:
            expressions.append(f"GPL-{gpl[1]}.0-or-later")
            evidence.append("file GPL notice")
        elif key.startswith("corvcad/"):
            if "/headless/" in key or re.search(r"/session/kicad_(session|board)\.cpp$", key) or (
                    "/probe/" in key and Path(key).suffix in (".cpp", ".h", ".hpp")):
                expressions.append("GPL-3.0-or-later")
                evidence.append("KiCad-derived integration; THIRD_PARTY.md")
            else:
                expressions.append("MIT")
                evidence.append("corvcad/LICENSE")
        else:
            expressions.append("BSD-3-Clause" if upstream.startswith("eeschema/sim/kibis/")
                                else "GPL-3.0-or-later")
            evidence.append("kicad/LICENSE.README")
    if key.endswith("/headless/wx_property_choices.cpp"):
        return {"expression": "wxWindows", "evidence": ["file wxWindows 3.1 notice"]}
    for value in re.findall(r"(?m)^[ \t]*(?:/\*+|\*|//|#)[ \t]*SPDX-License-Identifier:[ \t]*([^\r\n]+)", text):
        value = value.rstrip(" */")
        # Select the CC0 alternative permitted by GLAD's own dual-license expression.
        value = value.replace("(WTFPL OR CC0-1.0)", "CC0-1.0")
        if value not in expressions:
            expressions.append(value)
            evidence.append("file SPDX notice")
    # Avoid duplicate conditions already included in a component's expression.
    expressions = [x for x in expressions if not any(x != y and x in y for y in expressions)]
    return {"expression": " AND ".join("(" + x + ")" if " OR " in x else x
                                      for x in expressions), "evidence": evidence}


def source_record(key, data):
    return {"sha256_lf": sha(data), "license": license_info(key, data)}


def recipe_inputs(tree):
    # Preserve generator code/templates as well as every schema/keyword input. The
    # actual generated bytes belong to each build record, since protoc versions differ.
    cmake = (HERE / "probe/CMakeLists.txt").read_text()
    keywords = set(re.findall(r'"(?:PCB\|)?([^"|]+\.keywords)\|', cmake))
    grammars = {"common/libeval/grammar.lemon", "common/libeval_compiler/grammar.lemon",
                "common/text_eval/text_eval.lemon"}
    return [p for p in tree.blobs if
            p.startswith(("api/proto/", "cmake/BuildSteps/")) or p in keywords | grammars or
            p in ("thirdparty/lemon/lemon.c", "thirdparty/lemon/lempar.c", "cmake/config.h.cmake")]


def capture_sources(tree, build):
    profiles, selected = {}, set()
    for profile, (prefix, roots) in PROFILES.items():
        units = sorted(set(portable_closure(x) for x in
                           (build / (prefix + ".closure.txt")).read_text().splitlines() if x))
        direct = sorted("corvcad/" + (HERE / x).resolve().relative_to(REPO).as_posix() for x in roots)
        profiles[profile] = {"closure": units, "direct_objects": direct}
        selected.update(units + direct)
    # Ninja's compiler dependency graph records vendored headers that a TU-only
    # list misses. Generated and SDK inputs are classified separately below.
    actual = {}
    for key in selected:
        if key.startswith("kicad/"):
            actual[str(tree.root / key[6:])] = key
        elif key.startswith("corvcad/"):
            actual[str(REPO / key[8:])] = key
    dependencies, block = set(), []
    data = run("ninja", "-C", str(build), "-t", "deps").decode()
    matched = set()
    def collect():
        if not block or block[0] not in actual:
            return
        matched.add(actual[block[0]])
        for name in block:
            path = Path(name)
            if path.is_relative_to(tree.root):
                dependencies.add("kicad/" + path.relative_to(tree.root).as_posix())
            elif path.is_relative_to(REPO) and not path.is_relative_to(build):
                dependencies.add("corvcad/" + path.relative_to(REPO).as_posix())
    for line in data.splitlines():
        if line and not line.startswith(" "):
            collect()
            block = []
        elif line.strip():
            block.append(line.strip())
    collect()
    if set(actual.values()) - matched:
        raise RuntimeError("Missing compiler dependency evidence: " +
                           ", ".join(sorted(set(actual.values()) - matched)))
    selected.update(dependencies)
    selected.update("kicad/" + p for p in recipe_inputs(tree))
    selected.update("kicad/" + p for p in tree.blobs if p.startswith("LICENSE"))
    selected.add("kicad/AUTHORS.txt")
    selected.add("corvcad/LICENSE")
    for p in (HERE / "headless").rglob("*"):
        if p.is_file():
            selected.add("corvcad/" + p.relative_to(REPO).as_posix())
    # Include every build script and ABI header, not just code the linker loads.
    for p in HERE.rglob("*"):
        if p.is_file() and (p.suffix in (".sh", ".cmake", ".py") or p.name == "CMakeLists.txt"):
            selected.add("corvcad/" + p.relative_to(REPO).as_posix())
    selected.update("corvcad/engine/include/corvcad/" + n for n in
                    ("schematic_session_c_api.h", "board_native_c_api.h", "schematic_route.hpp", "starter_parts.hpp"))
    # Add all component license documents before computing the final inventory.
    for key in list(selected):
        if key.startswith("kicad/thirdparty/"):
            comp = key.split("/")[2]
            if comp not in POLICY["vendored"]:
                raise RuntimeError(f"Unreviewed component: {comp}")
            selected.update("kicad/" + p for p in POLICY["vendored"][comp][1:])
    upstream = [p[6:] for p in selected if p.startswith("kicad/")]
    tree.check_worktree(upstream)
    files, generated = {}, {}
    for key in sorted(selected):
        if key.startswith("generated/"):
            generated[key] = {"sha256_lf": sha((build / key).read_bytes()),
                              "note": "Observed desktop output; regenerate with the build recipe"}
        else:
            data = tree.read(key[6:]) if key.startswith("kicad/") else (REPO / key[8:]).read_bytes()
            files[key] = source_record(key, data)
    return {"schema": 1, "hash_convention": "SHA-256 of text with CRLF normalized to LF",
            "source": LOCK["schematic_source"], "patches": patch_records(tree),
            "license_policy_sha256_lf": sha((HERE / "license-policy.json").read_bytes()),
            "profiles": profiles, "files": files, "observed_generated": generated}


def patch_records(tree):
    result = []
    for p in patches():
        paths = re.findall(r"^diff --git a/(.+) b/(.+)$", p.read_text(encoding="utf-8"), re.M)
        if any(a != b for a, b in paths):
            raise RuntimeError("Patch renames require explicit audit handling")
        result.append({"path": p.relative_to(HERE).as_posix(),
                       "sha256_lf": sha(p.read_bytes()), "files": sorted(b for _, b in paths)})
    return result


def paragraphs(text):
    for block in text.split("\n\n"):
        row = {}
        for line in block.splitlines():
            if line and not line.startswith(" ") and ": " in line:
                key, value = line.split(": ", 1)
                row[key] = value
        if row:
            yield row


def write_notice(output, name, data):
    rel = "notices/" + name + "-" + sha(data)[:12] + ".txt"
    file = output / rel
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_bytes(canonical(data))
    return {"path": rel, "sha256_lf": sha(data)}


def notice_excerpt(path, data):
    """Retain the licensed comment blocks, rather than vendoring code as a notice."""
    if Path(path).suffix not in (".h", ".hpp", ".c", ".cpp", ".cc"):
        return data
    text = data.decode(errors="replace")
    comments = re.findall(r"/\*.*?\*/|(?m:^(?:[ \t]*//[^\n]*\n)+)", text, flags=re.S)
    excerpts = [c for c in comments if re.search(r"copyright|licen[cs]e|permission|SPDX|public domain", c, re.I)]
    if not excerpts:
        raise RuntimeError("No license comment found in notice source: " + path)
    return ("Source: KiCad 10.0.6 / " + path + "\n\n" + "\n\n".join(excerpts) + "\n").encode()


def verify_wx_sources(wx, archive):
    """Prove there are no hidden wx source patches behind the tarball pin."""
    entries = []
    with tarfile.open(archive, "r:bz2") as tar:
        for member in tar:
            if not member.isfile():
                continue
            path = Path(*Path(member.name).parts[1:])
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError("Invalid wxWidgets archive path")
            original = tar.extractfile(member).read()
            file = wx / path
            if not file.is_file() or sha(file.read_bytes()) != sha(original):
                raise RuntimeError("Undeclared wxWidgets source change: " + path.as_posix())
            entries.append(path.as_posix() + ":" + sha(original))
    return {"files": len(entries), "tree_sha256_lf": sha("\n".join(sorted(entries)).encode())}


def verify_notices(folder):
    notices = json.loads((folder / "notices.json").read_text())
    actual = {p.relative_to(folder).as_posix() for p in (folder / "notices").glob("*.txt")}
    if set(notices) != actual:
        raise RuntimeError("Notice inventory has missing or unrecorded files")
    for name, digest in notices.items():
        if sha((folder / name).read_bytes()) != digest:
            raise RuntimeError("License notice text drifted: " + name)
    return len(notices)


def capture_dependencies(build, vcpkg, installed, wx, output):
    if git(vcpkg, "rev-parse", "HEAD").decode().strip() != LOCK["vcpkg"]["commit"]:
        raise RuntimeError("Wrong vcpkg baseline")
    if git(vcpkg, "status", "--porcelain", "--untracked-files=no").strip():
        raise RuntimeError("vcpkg port recipes have unrecorded edits")
    records, owners = [], {}
    rows = list(paragraphs((installed / "vcpkg/status").read_text()))
    for row in rows:
        if row.get("Status") != "install ok installed" or "Feature" in row:
            continue
        name, arch = row["Package"], row["Architecture"]
        port = vcpkg / "ports" / name
        manifest = json.loads((port / "vcpkg.json").read_text())
        version = next(manifest[k] for k in ("version", "version-semver", "version-date", "version-string") if k in manifest)
        if version != row["Version"] or str(manifest.get("port-version", 0)) != row.get("Port-Version", "0"):
            raise RuntimeError(f"Installed {name} does not match the pinned port recipe")
        lists = list((installed / "vcpkg/info").glob(f"{name}_{version}_{arch}.list"))
        if len(lists) != 1:
            raise RuntimeError(f"Missing installed file inventory for {name}:{arch}")
        paths = lists[0].read_text().splitlines()
        for p in paths:
            owners[p] = name
        notices = [p for p in paths if Path(p).name in ("copyright", "LICENSE", "LICENSE.txt")]
        archives = sorted(p.split("/lib/", 1)[1] for p in paths if "/lib/" in p and p.endswith(".a"))
        if not notices and archives:
            raise RuntimeError(f"No redistribution notice for static dependency {name}:{arch}")
        expression = manifest.get("license")
        note = None
        if expression is None:
            expression, note = POLICY["vcpkg_overrides"][name]
        records.append({"name": name, "triplet": arch, "version": version,
                        "port_version": manifest.get("port-version", 0), "abi": row.get("Abi"),
                        "features": sorted(r["Feature"] for r in rows if r.get("Package") == name
                                           and r.get("Architecture") == arch and "Feature" in r),
                        "license": expression, "license_note": note,
                        "port_recipe_sha256_lf": sha((port / "portfile.cmake").read_bytes()),
                        "port_manifest_sha256_lf": sha((port / "vcpkg.json").read_bytes()),
                        "notices": [write_notice(output, "vcpkg-" + name, (installed / p).read_bytes()) for p in notices],
                        "archives": archives, "depends": row.get("Depends", "")})
    links = {}
    for path in sorted(build.glob("*_ios*.map")):
        obj = path.read_text(errors="replace").split("# Sections:", 1)[0]
        libs = sorted(set(re.findall(r"/installed/([^/]+/lib/[^(/]+\.a)\(", obj)))
        for lib in libs:
            if lib not in owners:
                raise RuntimeError("Linked archive has no vcpkg package owner: " + lib)
        links[path.stem] = {"vcpkg": [{"archive": lib, "package": owners[lib]} for lib in libs],
                           "wxwidgets": sorted(set(re.findall(r"/(libwx[^(/ ]+\.a)\(", obj))),
                           # ngspice_ios.sh's library (build-<target>/libngspice.a); its files are
                           # inventoried in audit/ngspice.json.
                           "ngspice": sorted(set(re.findall(r"/(build-[a-z]+/libngspice\.a)\(", obj)))}
    if any(row["ngspice"] for row in links.values()) and not (HERE / "audit" / "ngspice.json").is_file():
        raise RuntimeError("ngspice is linked but audit/ngspice.json (ngspice/inventory.py) is absent")
    if not {"session_ios", "pcb_probe_ios", "pcb_probe_ios-device"} <= links.keys():
        raise RuntimeError("Required simulator/device link-map evidence is absent")
    tar = wx.parent / "wx.tar.bz2"
    if not tar.is_file() or hashlib.sha256(tar.read_bytes()).hexdigest() != LOCK["wxwidgets"]["sha256"]:
        raise RuntimeError("wxWidgets release archive checksum mismatch")
    wx_tree = verify_wx_sources(wx, tar)
    wx_notices = [write_notice(output, "wxwidgets-" + p.name, p.read_bytes())
                  for p in sorted((wx / "docs").glob("*"))
                  if p.name in ("licence.txt", "lgpl.txt", "gpl.txt", "licendoc.txt")]
    bundled = {name: {"license": row[0], "notices": [write_notice(
                    output, "wx-bundled-" + name + "-" + Path(p).name, (wx / p).read_bytes()) for p in row[1:]]}
               for name, row in POLICY["wx_bundled"].items()}
    # The official symbol and footprint libraries share one licence text (CC-BY-SA 4.0 with
    # KiCad's design exception), retained once as the kicad-symbols notice.
    symbol_notice = HERE / LOCK["symbol_libraries"]["license_file"]
    for name in LIBRARY_PINS:
        if sha((HERE / LOCK[name]["license_file"]).read_bytes()) != LOCK[name]["license_sha256_lf"]:
            raise RuntimeError(f"KiCad {name} notice checksum mismatch")
    library_notice = write_notice(output, "kicad-symbols", symbol_notice.read_bytes())
    return {"schema": 1, "vcpkg": LOCK["vcpkg"],
            "wxwidgets": {**LOCK["wxwidgets"], "notices": wx_notices, "source_tree": wx_tree, "bundled": bundled},
            **{name: {**LOCK[name], "notices": [library_notice]} for name in LIBRARY_PINS},
            "packages": sorted(records, key=lambda x: (x["name"], x["triplet"])),
            "ngspice": {**LOCK["ngspice"], "inventory": "audit/ngspice.json"},
            "link_map_archive_inputs": links,
            "scope": "All installed packages are recorded; only the listed archive inputs contribute to each measured iOS link. Header-only and host build packages are retained separately by triplet. Apple SDK/runtime files are system dependencies, not redistributed package payloads."}


def verify(tree, folder):
    source = json.loads((folder / "sources.json").read_text())
    if source["source"] != LOCK["schematic_source"] or source["patches"] != patch_records(tree):
        raise RuntimeError("Source lock or patch inventory drifted; recapture and review")
    if source["license_policy_sha256_lf"] != sha((HERE / "license-policy.json").read_bytes()):
        raise RuntimeError("License policy drifted")
    tree.check_worktree(p[6:] for p in source["files"] if p.startswith("kicad/"))
    for key, expected in source["files"].items():
        data = tree.read(key[6:]) if key.startswith("kicad/") else (REPO / key[8:]).read_bytes()
        if source_record(key, data) != expected:
            raise RuntimeError("Source or license drift: " + key)
    dependencies = json.loads((folder / "dependencies.json").read_text())
    if dependencies["vcpkg"] != LOCK["vcpkg"] or any(
            dependencies["wxwidgets"][k] != v for k, v in LOCK["wxwidgets"].items()):
        raise RuntimeError("External dependency lock drifted")
    for name in LIBRARY_PINS:
        if any(dependencies[name][k] != v for k, v in LOCK[name].items()):
            raise RuntimeError(f"KiCad {name} pin or notice drifted")
        if sha((HERE / LOCK[name]["license_file"]).read_bytes()) != LOCK[name]["license_sha256_lf"]:
            raise RuntimeError(f"Local {name} notice drifted")
    for package in dependencies["packages"] + [dependencies["wxwidgets"]] + [dependencies[name] for name in LIBRARY_PINS]:
        for notice in package["notices"]:
            if sha((folder / notice["path"]).read_bytes()) != notice["sha256_lf"]:
                raise RuntimeError("Dependency license text drifted: " + notice["path"])
    count = verify_notices(folder)
    print(f"Verified {len(source['files'])} source/recipe inputs, {len(source['patches'])} patches, "
          f"{len(dependencies['packages'])} installed package records and {count} retained notices")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("--build", type=Path, required=True)
    cap.add_argument("--vcpkg", type=Path, required=True)
    cap.add_argument("--installed", type=Path, required=True)
    cap.add_argument("--wx", type=Path, required=True)
    cap.add_argument("--output", type=Path, required=True)
    check = sub.add_parser("verify")
    check.add_argument("--inventory", type=Path, default=HERE / "audit")
    for command in (cap, check):
        command.add_argument("--kicad", type=Path, required=True)
    args = parser.parse_args()
    tree = SourceTree(args.kicad)
    if args.command == "verify":
        verify(tree, args.inventory)
    else:
        if args.output.exists():
            raise RuntimeError("Choose a new output directory to preserve the previous audit")
        sources = capture_sources(tree, args.build.resolve())
        args.output.mkdir(parents=True)
        deps = capture_dependencies(args.build.resolve(), args.vcpkg.resolve(),
                                    args.installed.resolve(), args.wx.resolve(), args.output)
        for name, value in (("sources", sources), ("dependencies", deps)):
            (args.output / (name + ".json")).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        # Retain exact upstream notices independently of an installed checkout.
        for p, record in sources["files"].items():
            if p.startswith("kicad/") and (p.startswith("kicad/LICENSE") or
                    p in {e for row in POLICY["vendored"].values() for e in ["kicad/" + x for x in row[1:]]}):
                write_notice(args.output, "kicad-" + p[6:].replace("/", "__"),
                             notice_excerpt(p[6:], tree.read(p[6:])))
            elif p.startswith("kicad/") and ("file SPDX notice" in record["license"]["evidence"] or
                    p[6:] in POLICY["overrides"]):
                # Keep all licensed excerpts in mixed-license headers, including
                # the second notice on Zint's embedded font and LLVM excerpts.
                write_notice(args.output, "kicad-excerpts-" + p[6:].replace("/", "__"),
                             notice_excerpt(p[6:], tree.read(p[6:])))
        (args.output / "notices.json").write_text(json.dumps({
            p.relative_to(args.output).as_posix(): sha(p.read_bytes())
            for p in sorted((args.output / "notices").glob("*.txt"))}, indent=2) + "\n")
        verify(tree, args.output)


if __name__ == "__main__":
    main()

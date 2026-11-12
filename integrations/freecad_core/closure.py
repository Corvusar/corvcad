#!/usr/bin/env python3
"""Measure generator inputs and owned desktop dependencies after a source build.

No source/install tree is changed. A capture becomes a lock only after review;
system SDKs and a release's complete corresponding source remain separate.
"""
import argparse
import hashlib
import importlib.util
import importlib.metadata
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import zipfile

from inventory import gui_dependency
from retain_inventory import safe_relative


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, cwd=None):
    return subprocess.check_output([str(a) for a in args], cwd=cwd, text=True)


def graph_files(dot):
    # Rule nodes have shape=ellipse; the remaining labelled nodes name files or
    # phony targets. Keep raw DOT as well so relationships remain reviewable.
    pattern = r'^"[^"\n]+" \[label=("(?:\\.|[^"\\])*")(.*?)\]'
    return [json.loads(label) for label, attrs in re.findall(pattern, dot, re.MULTILINE)
            if "shape=ellipse" not in attrs]


def links(text, system):
    if system == "Linux":
        if "not found" in text:
            raise ValueError("Unresolved runtime library")
        return re.findall(r"(?:=>\s+|^\s*)(/[^\s]+)\s+\(", text, re.MULTILINE)
    return [line.strip().split(" (", 1)[0] for line in text.splitlines()[1:] if line.startswith("\t")]


def debian_owners(paths, lists):
    wanted = {p.as_posix() for p in paths}
    result = {}
    for listing in lists.glob("*.list"):
        owner = listing.name[:-5]
        for name in listing.read_text(errors="replace").splitlines():
            # Ubuntu's /usr merge leaves package database names under /lib.
            candidates = [name, "/usr"+name] if name.startswith(("/lib/", "/lib64/", "/bin/", "/sbin/")) else [name]
            for candidate in candidates:
                if candidate in wanted:
                    result.setdefault(candidate, set()).add(owner)
    return result


def brew_owner(path):
    parts = path.parts
    if "Cellar" not in parts:
        return None
    start = parts.index("Cellar")
    if len(parts) <= start+2:
        return None
    return parts[start+1], parts[start+2], Path(*parts[:start+3])


def archive_notices(archive, save):
    """Read regular notice members only; never extract paths or follow links."""
    retained = []
    pattern = r"(?i)^(licen[cs]e|copying|copyright|notice)([._-]|$)"
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as bundle:
            for item in bundle.infolist():
                if not item.is_dir() and re.match(pattern, Path(item.filename).name):
                    retained.append(save(safe_relative(item.filename), bundle.read(item)))
    else:
        with tarfile.open(archive, "r:*") as bundle:
            for item in bundle:
                if item.isfile() and re.match(pattern, Path(item.name).name):
                    retained.append(save(safe_relative(item.name), bundle.extractfile(item).read()))
    if not retained:
        raise ValueError(f"No upstream notices in {archive}")
    return retained


def capture(source, build, output):
    source, build, output = source.resolve(), build.resolve(), output.resolve()
    system = platform.system()
    if system not in ("Linux", "Darwin"):
        raise ValueError("Capture requires the actual Linux/macOS source-build host")
    output.mkdir(parents=True, exist_ok=True)
    inventory = json.loads((output/"inventory.json").read_text())
    if inventory["status"] != "objects-and-runtime-recorded":
        raise ValueError("Dependency capture requires a complete build")

    dot = run(["ninja", "-t", "graph", "all"], build)
    commands = run(["ninja", "-t", "commands", "all"], build)
    (output/"generator-graph.dot").write_text(dot)
    (output/"build-commands.txt").write_text(commands)
    candidates = {Path(p) if Path(p).is_absolute() else build/p for p in graph_files(dot)}
    reply = build/".cmake/api/v1/reply"
    index = json.loads(sorted(reply.glob("index-*.json"))[-1].read_text())
    cmake = next(o for o in index["objects"] if o["kind"] == "cmakeFiles")
    configure = json.loads((reply/cmake["jsonFile"]).read_text())
    candidates.update(Path(i["path"]) if Path(i["path"]).is_absolute() else source/i["path"]
                      for i in configure["inputs"])
    # The pinned wrapper generator imports these Python model/template modules
    # dynamically; CMake declares only its entry point and one template. Retain
    # the entire small generator package, including its namespace initializers.
    candidates.update(p for p in (source/"src/Tools/bindings").rglob("*")
                      if p.suffix in (".py", ".pyi", ".xml", ".xsd"))
    # These generated parser sources are checked in at the pin. Their grammar
    # inputs are provenance even when Bison/Flex regeneration is not requested.
    candidates.update(source/name for name in ("src/App/Expression.y", "src/App/Expression.l"))
    tests = json.loads((output/"upstream-tests.json").read_text())
    for name in tests["loaded_python_files"]:
        path = Path(name)
        if path.suffix == ".pyc":
            original = Path(importlib.util.source_from_cache(str(path)))
            if original.is_file():
                path = original
        if not path.is_relative_to(Path(__file__).resolve().parent):
            candidates.add(path)
    for record in inventory["compiler_discovered_files"]:
        candidates.add(source/record["path"] if record["origin"] == "source" else Path(record["path"]))
    for name in ("cmake", "ninja", "ccache", "swig", "bison", "flex"):
        executable = shutil.which(name)
        if executable:
            candidates.add(Path(executable))
    for line in (build/"CMakeCache.txt").read_text().splitlines():
        if not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            if re.search(r"(?:EXECUTABLE|COMPILER|RCC|LRELEASE|MOC)(?:_[A-Z]+)?:", key) and Path(value).is_file():
                candidates.add(Path(value))
    # Literal executable/script/library arguments may not be Ninja file deps.
    for name in re.findall(r'(?<![\w/])(/[^\s;"\)]+)', commands):
        if Path(name).is_file():
            candidates.add(Path(name))

    brew = json.loads(run(["brew", "info", "--json=v2", "--installed"])) if system == "Darwin" else None
    formulae = {f["name"]: f for f in brew["formulae"]} if brew else {}
    library_dirs = [build/"lib", build/"bin", Path("/opt/homebrew/lib"), Path("/opt/homebrew/Frameworks")]
    library_dirs.extend((build/a["path"]).parent for a in inventory["target_artifacts"])
    for f in formulae.values():
        for installed in f["installed"]:
            root = Path("/opt/homebrew/Cellar")/f["name"]/installed["version"]
            library_dirs.extend([root/"lib", root/"Frameworks"])
    pending = [Path(item["command"][-1]) for item in inventory["runtime_link_inspections"]]
    pending.extend(p for p in candidates if p.is_file() and p.suffix in (".so", ".dylib"))
    visited, runtime, system_runtime = set(), [], set()
    while pending:
        binary = pending.pop().resolve()
        if binary in visited:
            continue
        visited.add(binary)
        text = run(["ldd", binary] if system == "Linux" else ["otool", "-L", binary])
        if gui_dependency(text):
            raise ValueError(f"GUI dependency in runtime closure of {binary}")
        runtime.append({"binary": str(binary), "links": text})
        local_dirs = list(library_dirs)
        if system == "Darwin":
            loads = run(["otool", "-l", binary])
            for rpath in re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.+?) \(offset", loads):
                local_dirs.append(Path(rpath.replace("@loader_path", str(binary.parent)).replace("@executable_path", str(build/"bin"))))
        for name in links(text, system):
            # otool includes a dylib's own install name as its first entry.
            if system == "Darwin" and name == "@rpath/"+binary.name:
                continue
            if system == "Darwin" and name.startswith(("/usr/lib/", "/System/Library/")):
                system_runtime.add(name)
                continue
            if name.startswith("@rpath/"):
                matches = [p/name[len("@rpath/"):] for p in local_dirs if (p/name[len("@rpath/"):]).is_file()]
                resolved = matches[0] if matches else None
            else:
                resolved = Path(name.replace("@loader_path", str(binary.parent)).replace("@executable_path", str(build/"bin")))
            if resolved is None or not resolved.is_file():
                raise ValueError(f"Unresolved runtime dependency {name} of {binary}")
            resolved = resolved.resolve()
            if not resolved.is_relative_to(build):
                candidates.add(resolved)
                pending.append(resolved)

    files = {p.resolve() for p in candidates if p.is_file()}
    source_files = {p.relative_to(source).as_posix(): {"sha256": sha(p)} for p in sorted(files)
                    if p.is_relative_to(source) and not p.is_relative_to(source/".git")}
    external = sorted(p for p in files if not p.is_relative_to(source) and not p.is_relative_to(build))
    packages, records, unknown = {}, [], []

    def retain(path, name):
        target = output/"dependency-notices"/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(path.read_bytes())
        return {"path": name, "sha256": sha(path)}

    def retain_data(name, data):
        target = output/"dependency-notices"/safe_relative(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest()}

    # Python post-install wheels can live outside Homebrew's Cellar. Match the
    # actual distribution RECORD, never assign an arbitrary site-packages path
    # to the interpreter's licence.
    python_owners = {}
    debian = debian_owners(external, Path("/var/lib/dpkg/info")) if system == "Linux" else {}
    external_set = {p for p in external if (str(p) not in debian if system == "Linux" else
                    brew_owner(p) is None and not str(p).startswith(("/Applications/Xcode", "/Library/Developer/", "/usr/", "/System/")))}
    for distribution in importlib.metadata.distributions():
        members = list(distribution.files or [])
        matches = {Path(distribution.locate_file(item)).resolve() for item in members} & external_set
        if not matches:
            continue
        name = re.sub(r"[^A-Za-z0-9_.-]", "_", distribution.metadata["Name"])
        key = "python-"+name+"@"+distribution.version
        notices = [item for item in members if re.match(r"(?i)^(licen[cs]e|copying|copyright|notice)([._-]|$)", Path(item).name)]
        if not notices:
            raise ValueError(f"No licence notice for loaded Python distribution {key}")
        packages[key] = {"version": distribution.version, "package_type": "Python distribution",
                         "declared_licenses": distribution.metadata.get("License-Expression") or distribution.metadata.get("License"),
                         "notices": [retain(Path(distribution.locate_file(item)), "python-"+name+"/"+str(item)) for item in notices]}
        for path in matches:
            python_owners.setdefault(path, []).append(key)

    if system == "Linux":
        owned = debian
        metadata = {}
        for line in run(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\n"]).splitlines():
            name, version, src, src_version = line.split("\t")
            metadata[name] = {"version": version, "source_package": src, "source_version": src_version}
        # Debian notices often refer to these texts instead of duplicating
        # entire GPL/LGPL/Apache grants in each package copyright file.
        packages["base-files"] = {**metadata["base-files"],
                                  "role": "Standard licence texts, not linked package code",
                                  "declared_licenses": ["LicenseRef-Debian-Common-License-Texts"],
                                  "notices": [retain(p, "common-licenses/"+p.name)
                                              for p in sorted(Path("/usr/share/common-licenses").iterdir()) if p.is_file()]}
        for path in external:
            owners = sorted(owned.get(str(path), [])) or python_owners.get(path, [])
            if not owners:
                unknown.append(str(path))
            records.append({"path": str(path), "sha256": sha(path), "owners": owners})
            for owner in owners:
                if owner in packages:
                    continue
                record = dict(metadata[owner])
                copyright_file = Path("/usr/share/doc")/owner.split(":")[0]/"copyright"
                if not copyright_file.is_file():
                    raise ValueError(f"No retained copyright for {owner}")
                record["notices"] = [retain(copyright_file, owner.replace(":", "_")+"/copyright")]
                text = copyright_file.read_text(errors="replace")
                record["declared_licenses"] = sorted(set(re.findall(r"^License:\s*(.+)", text, re.MULTILINE)))
                packages[owner] = record
    else:
        for path in external:
            owner = brew_owner(path)
            if owner:
                name, version, root = owner
                key = name+"@"+version
                if key not in packages:
                    formula = formulae[name]
                    notices = [p for p in root.rglob("*") if p.is_file() and
                               (re.match(r"(?i)^(licen[cs]e|copying|copyright|notice)([._-]|$)", p.name) or
                                any(part.lower() in ("licenses", "licences") for part in p.relative_to(root).parts[:-1]))]
                    source_archive = None
                    retained_notices = [retain(p, name+"/"+p.relative_to(root).as_posix()) for p in sorted(notices)]
                    if not notices:
                        # Some bottles (notably Boost) omit their standalone
                        # notice. Fetch the matching checksum-pinned upstream
                        # source without building or installing anything.
                        expected_version = formula["versions"]["stable"]
                        if formula.get("revision"):
                            expected_version += "_"+str(formula["revision"])
                        if version != expected_version:
                            raise ValueError(f"Installed {key} differs from current source recipe; retain its matching source notice explicitly")
                        subprocess.run(["brew", "fetch", "--build-from-source", "--retry", name], check=True)
                        archive = Path(run(["brew", "--cache", "--build-from-source", name]).strip())
                        stable = formula["urls"]["stable"]
                        if sha(archive) != stable["checksum"]:
                            raise ValueError(f"Source archive hash mismatch: {name}")
                        source_archive = {"url": stable["url"], "sha256": stable["checksum"]}
                        retained_notices = archive_notices(archive, lambda filename, data: retain_data(name+"/upstream/"+filename, data))
                    packages[key] = {"version": version, "declared_licenses": formula.get("license"),
                                     "formula_source": formula.get("urls", {}).get("stable"),
                                     "source_archive_for_notices": source_archive,
                                     "notices": retained_notices,
                                     "installed_recipe": retain(root/".brew"/(name+".rb"), name+"/formula.rb"),
                                     "receipt": retain(root/"INSTALL_RECEIPT.json", name+"/INSTALL_RECEIPT.json")}
                owners = [key]
            elif str(path).startswith(("/Applications/Xcode", "/Library/Developer/", "/usr/bin/", "/usr/lib/", "/System/")):
                owners = ["apple-system-sdk"]
            else:
                owners = python_owners.get(path, [])
                if not owners:
                    unknown.append(str(path))
            records.append({"path": str(path), "sha256": sha(path), "owners": owners})
        packages["apple-system-sdk"] = {"version": run(["xcodebuild", "-version"]).strip(),
                                        "sdk": run(["xcrun", "--show-sdk-version"]).strip(),
                                        "license": "LicenseRef-Apple-System-SDK", "notices": [],
                                        "scope": "System prerequisite; not redistributed by this integration"}

    result = {"schema": 1, "system": system, "source_commit": inventory["source_git"]["stdout"].strip(),
              "profile_sha256": inventory["profile_sha256"], "status": "captured" if not unknown else "unowned-inputs",
              "source_files": source_files, "external_files": records, "packages": packages,
              "unowned": unknown, "runtime_closure": runtime, "system_runtime": sorted(system_runtime),
              "generator_graph_sha256": sha(output/"generator-graph.dot"),
              "build_commands_sha256": sha(output/"build-commands.txt"),
              "scope": "Desktop build inputs and runtime link closure; complete release sources and iOS linkage are separate"}
    integration = Path(__file__).resolve().parent
    repository = integration.parents[1]
    tooling = list(integration.glob("*.py")) + list(integration.glob("*.json")) + list((integration/"fixtures").glob("*.json"))
    tooling += list((repository/".github/workflows").glob("freecad-*.yml"))
    tooling += [integration/".gitattributes"]
    result["integration_inputs"] = {p.relative_to(repository).as_posix(): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
                                    for p in sorted(tooling)}
    (output/"closure.json").write_text(json.dumps(result, indent=2)+"\n")
    print(f"Closure: {len(source_files)} source/build inputs, {len(records)} external files, {len(packages)} packages; {len(unknown)} unowned")
    if unknown:
        raise ValueError(f"Unowned desktop inputs: {unknown[:10]}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    capture(args.source, args.build, args.output)

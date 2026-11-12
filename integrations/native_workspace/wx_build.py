#!/usr/bin/env python3
"""Build pinned wx iOS with shared Expat and optional shared PCRE2. MIT.

An out-of-tree build preserves accepted wx products and upstream source.  Every
selected archive is inspected for Apple target metadata; compiler dependency
files must prove XML uses the union Expat header instead of the bundled copy.
The optional regex override similarly requires the union PCRE2-32 library and
compiler evidence for its header, while leaving the default build unchanged.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys

import kicad_build as native

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "freecad_core/ios"))
import build_occt

# wx prefixes NanoSVG's public functions but upstream leaves these two private
# implementation globals visible. KiCad embeds a different NanoSVG fork.
NANOSVG_PRIVATE_PREFIXES = {"nsvg__parseXML": "corvcad_wx_nsvg__parseXML",
                          "nsvg__colors": "corvcad_wx_nsvg__colors"}


def configure_command(source, sdk_path, sdk, architecture, prefix, system_regex=False):
    triple = native.target(sdk, architecture)
    host = "aarch64-apple-darwin" if architecture == "arm64" else "x86_64-apple-darwin"
    command = [str(source / "configure"), "--with-osx_iphone", "--disable-shared", "--disable-sys-libs",
               "--with-expat=sys", "--with-macosx-sdk=" + str(sdk_path), "--with-macosx-version-min=17.0",
               "--enable-macosx_arch=" + architecture, "--host=" + host]
    # Retain the reviewed standalone cross-mode selection on an arm64 host too.
    if architecture == "arm64":
        command.append("--build=x86_64-apple-darwin")
    if system_regex:
        command.append("--with-regex=sys")
    return command


def build_environment(sdk, sdk_path, architecture, prefix, system_regex=False):
    env = dict(os.environ)
    for key in ("CC", "CXX", "CPP", "CXXCPP", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBS",
                "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "SDKROOT",
                "MACOSX_DEPLOYMENT_TARGET", "IPHONEOS_DEPLOYMENT_TARGET", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR",
                "LIBPCRE_CFLAGS", "LIBPCRE_LIBS"):
        env.pop(key, None)
    triple = native.target(sdk, architecture)
    common = ["-target", triple, "-isysroot", str(sdk_path)]
    env.update({"CC": shlex.join(["xcrun", "--sdk", sdk, "clang", *common]),
                "CXX": shlex.join(["xcrun", "--sdk", sdk, "clang++", *common]),
                "CPPFLAGS": shlex.join(["-I" + str(prefix / "include"),
                                        *["-D" + name + "=" + renamed
                                          for name, renamed in NANOSVG_PRIVATE_PREFIXES.items()]]),
                "LDFLAGS": shlex.join(["-L" + str(prefix / "lib")]),
                "PKG_CONFIG": "/usr/bin/false", "IPHONEOS_DEPLOYMENT_TARGET": "17.0"})
    if system_regex:
        # wx's configure accepts these overrides without invoking pkg-config.
        # Both iOS targets use 32-bit wchar_t; do not discover host PCRE2.
        env["LIBPCRE_CFLAGS"] = shlex.join(["-I" + str(prefix / "include")])
        env["LIBPCRE_LIBS"] = shlex.join([str(prefix / "lib/libpcre2-32.a")])
    return env


def selected_archives(flags, build, prefix=None, system_regex=False):
    result = []
    if "-lexpat" not in flags or any("wxexpat" in x for x in flags):
        raise ValueError("wx must use the union Expat library and omit bundled wxexpat")
    regex_archive = (prefix / "lib/libpcre2-32.a").resolve() if system_regex and prefix else None
    if system_regex and (regex_archive is None or str(regex_archive) not in flags
                         or any("wxregex" in x for x in flags)):
        raise ValueError("wx must select the union PCRE2-32 archive and omit bundled wxregex")
    for arg in flags:
        if arg.startswith("-lwx"):
            path = build / "lib" / ("lib" + arg[2:] + ".a")
        elif arg.endswith(".a"):
            path = Path(arg)
        else:
            continue
        path = path.resolve()
        if system_regex and path == regex_archive:
            # This is already verified and inventoried by the shared dependency
            # receipt. It is a dependency, not a wx build product.
            continue
        if not path.is_relative_to(build.resolve() / "lib"):
            raise ValueError("wx-config selected an archive outside this build: " + str(path))
        if path not in result:
            result.append(path)
    if not result:
        raise ValueError("wx-config did not select static archives")
    return result


def capture_dependencies(build, source, prefix, sdk_path, resource, system_regex=False):
    roots = [("wx-build", build), ("wx-source", source), ("vcpkg", prefix),
             ("apple-sdk", sdk_path), ("clang-resource", resource)]
    dependencies = {}
    files = sorted((build / ".deps").glob("*.d"))
    if not files:
        raise ValueError("wx compiler dependency files are missing")
    for depfile in files:
        for name in native.parse_depfile(depfile.read_text()):
            path = Path(name)
            if not path.is_absolute():
                path = build / path
            path = path.resolve()
            key = native.dependency_key(path, roots)
            if key.startswith("wx-source/src/expat/"):
                raise ValueError("Bundled Expat input reached wx build: " + key)
            if system_regex and key.startswith(("wx-source/3rdparty/pcre/", "wx-build/3rdparty/pcre/")):
                raise ValueError("Bundled PCRE2 input reached wx build: " + key)
            if key not in dependencies:
                dependencies[key] = {"sha256": native.sha(path), "sha256_lf": native.sha(path, text=True)}
    if "vcpkg/include/expat.h" not in dependencies:
        raise ValueError("wx XML has no compiler evidence for the union Expat header")
    if system_regex and "vcpkg/include/pcre2.h" not in dependencies:
        raise ValueError("wx regex has no compiler evidence for the union PCRE2 header")
    return dict(sorted(dependencies.items()))


def verify_configuration(config, system_regex=False):
    if "wxUSE_EXPAT='sys'" not in config:
        raise ValueError("wx configure did not select system Expat")
    if system_regex and "wxUSE_REGEX='sys'" not in config:
        raise ValueError("wx configure did not select system PCRE2")


def logged(command, cwd, env, path):
    with path.open("wb") as stream:
        stream.write((shlex.join([str(x) for x in command]) + "\n").encode())
        stream.flush()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("wx build step failed; see " + str(path))


def verify_source(source, archive, lock):
    if native.sha(archive) != lock["sha256"]:
        raise ValueError("wx source archive checksum differs from pinned release")
    spec = importlib.util.spec_from_file_location("corvcad_wx_source_audit", native.KICAD / "audit.py")
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    return audit.verify_wx_sources(source, archive)


def build(args):
    source, archive, output = args.source.resolve(), args.archive.resolve(), args.output.resolve()
    if output.exists():
        raise ValueError("Choose a new output directory; existing wx products and evidence are preserved")
    receipt = json.loads(args.dependency_receipt.read_text())
    prefix = Path(receipt["paths"]["prefix"]).resolve()
    native.validate_dependency_receipt(receipt, args.sdk, args.architecture, prefix)
    protoc = Path(receipt["paths"]["installed"]) / receipt["target"]["host_triplet"] / "tools/protobuf/protoc"
    native.verify_dependency_inputs(receipt, prefix, protoc)
    system_regex = args.system_regex
    if system_regex:
        for relative in ("lib/libpcre2-32.a", "include/pcre2.h"):
            if not (prefix / relative).is_file():
                raise ValueError("Shared PCRE2 input is missing: " + relative)
    lock = json.loads((native.KICAD / "upstream.json").read_text())["wxwidgets"]
    source_record = verify_source(source, archive, lock)
    sdk_path = Path(native.run(["xcrun", "--sdk", args.sdk, "--show-sdk-path"])).resolve()
    compiler = native.run(["xcrun", "--sdk", args.sdk, "clang++", "--version"])
    if compiler != receipt["toolchain"]["compiler"]:
        raise ValueError("wx and union dependencies use different compiler versions")
    sdk_version = native.run(["xcrun", "--sdk", args.sdk, "--show-sdk-version"])
    if sdk_version != receipt["toolchain"]["sdk_version"]:
        raise ValueError("wx and union dependencies use different SDK versions")
    resource = Path(native.run(["xcrun", "--sdk", args.sdk, "clang++", "-print-resource-dir"])).resolve()
    work = output / "build"
    work.mkdir(parents=True)
    env = build_environment(args.sdk, sdk_path, args.architecture, prefix, system_regex)
    command = configure_command(source, sdk_path, args.sdk, args.architecture, prefix, system_regex)
    logged(command, work, env, output / "configure.log")
    config = (work / "config.log").read_text(errors="replace")
    verify_configuration(config, system_regex)
    logged([args.make, "-j" + str(args.jobs), "V=1"], work, env, output / "make.log")
    flags = shlex.split(native.run([work / "wx-config", "--libs", "base,xml,core"]))
    selected = selected_archives(flags, work, prefix, system_regex)
    observed = {}
    for library in selected:
        architecture = native.run(["xcrun", "lipo", "-archs", library])
        if architecture != args.architecture:
            raise ValueError("Wrong wx archive architecture: " + str(library))
        load = native.run(["xcrun", "otool", "-l", library])
        count = build_occt.validate_platform(load, args.sdk, "17.0")
        observed[str(library)] = {"sha256": native.sha(library), "bytes": library.stat().st_size,
                                  "verified_build_versions": count}
    dependencies = capture_dependencies(work, source, prefix, sdk_path, resource, system_regex)
    # Confirm the source tree was not modified by configure/make.
    if verify_source(source, archive, lock) != source_record:
        raise ValueError("wx source changed during build")
    result = {"schema": 1, "status": "wx archives built with shared Expat and target verified",
              "target": receipt["target"], "source": {**lock, "verified_tree": source_record},
              "dependency_receipt": {"path": str(args.dependency_receipt.resolve()),
                                     "sha256": native.sha(args.dependency_receipt)},
              "build": str(work), "configure_command": command,
              "environment": {key: env[key] for key in ("CC", "CXX", "CPPFLAGS", "LDFLAGS", "PKG_CONFIG",
                                                        "LIBPCRE_CFLAGS", "LIBPCRE_LIBS") if key in env},
              "link_flags": flags, "cxx_flags": shlex.split(native.run([work / "wx-config", "--cxxflags"])),
              "archives": observed, "source_inventory": dependencies,
              "nanosvg_private_prefixes": NANOSVG_PRIVATE_PREFIXES,
              "regex_provider": ({"kind": "shared PCRE2-32", "archive": str(prefix / "lib/libpcre2-32.a"),
                                  "archive_sha256": native.sha(prefix / "lib/libpcre2-32.a"),
                                  "header": "vcpkg/include/pcre2.h",
                                  "header_sha256": dependencies["vcpkg/include/pcre2.h"]["sha256"]}
                                 if system_regex else {"kind": "wx bundled PCRE2-32"}),
              "compiler": compiler, "sdk_version": sdk_version,
              "recipe_sha256_lf": native.sha(Path(__file__), text=True),
              "physical_device_validation": False}
    (output / "wx-build.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "archive", "dependency-receipt", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--architecture", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--make", default="make")
    parser.add_argument("--system-regex", action="store_true",
                        help="Use the shared prefix PCRE2-32 archive instead of wx's bundled PCRE2")
    args = parser.parse_args()
    try:
        if args.jobs < 1:
            raise ValueError("jobs must be positive")
        result = build(args)
        print(json.dumps({"status": result["status"], "archives": len(result["archives"]), "build": result["build"]}))
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()

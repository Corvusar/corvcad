#!/usr/bin/env python3
"""Build a static CPython archive with built-in stdlib extensions for iOS.

Uses upstream's iOS configure support (framework configuration is mandatory),
but builds the supported libpython archive target, not a dynamic framework.
MIT; CPython and its vendored components retain their upstream notices.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import urllib.request
import target_config

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / "inputs.json").read_text())
# CPython 3.13.7 uses link-only AC_CHECK_FUNCS probes for these functions.
# Xcode 26.6's SDK exports the symbols but its iOS headers do not declare them.
# Select upstream's dup2/fcntl and pipe fallbacks for both iOS targets only.
IOS_CONFIGURE_CACHE = {"ac_cv_func_dup3": "no", "ac_cv_func_pipe2": "no"}


def run(command, cwd, env=None):
    print("Running:", *map(str, command), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True)


def extract(archive, destination):
    """Reject traversal and links rather than trusting an archive's paths."""
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise ValueError("Archive path escapes destination")
            if not (member.isfile() or member.isdir()):
                raise ValueError("Unexpected archive member type")
        package.extractall(destination, filter="data")


def source(root):
    pin = LOCK["python"]
    archive = root / Path(pin["url"]).name
    if not archive.exists():
        partial = archive.with_suffix(".download")
        with urllib.request.urlopen(pin["url"], timeout=60) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output)
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin["sha256"]:
        raise ValueError("CPython archive checksum mismatch")
    directory = root / ("Python-" + pin["version"])
    if not directory.exists():
        extract(archive, root)
    # Do not accept edits in an existing unpacked source tree on a cache hit.
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            if member.isfile():
                path = root / member.name
                if not path.is_file() or path.read_bytes() != package.extractfile(member).read():
                    raise ValueError(f"Modified CPython source: {member.name}")
    return directory


def stage_sysconfig(target, stdlib):
    """Use target-generated build settings, never the host interpreter's data."""
    relative = Path((target / "pybuilddir.txt").read_text().strip())
    directory = (target / relative).resolve()
    if relative.is_absolute() or not directory.is_relative_to(target.resolve()):
        raise ValueError("CPython sysconfig build path escapes target")
    files = list(directory.glob("_sysconfigdata_*.py"))
    if len(files) != 1:
        raise ValueError("Expected exactly one target sysconfig data module")
    shutil.copy2(files[0], stdlib / files[0].name)


def verify_ios_config(target):
    """Reject stale/overridden feature probes before compiling posixmodule."""
    config = (target / "pyconfig.h").read_bytes()
    if re.search(rb"^\s*#\s*define\s+(HAVE_DUP3|HAVE_PIPE2)\b", config, re.MULTILINE):
        raise ValueError("iOS pyconfig enables undeclared dup3/pipe2 APIs")
    return hashlib.sha256(config).hexdigest()


def host_environment():
    # This interpreter generates sources/bytecode; it needs no TLS or OpenSSL.
    # CPython 3.13.7 does not support Homebrew OpenSSL 4. Keep optional host
    # crypto modules out of this build without changing the target interpreter.
    env = dict(os.environ)
    for key in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "PKG_CONFIG_PATH", "SDKROOT", "CC", "CXX"):
        env.pop(key, None)
    env.update(PKG_CONFIG="/usr/bin/false", ac_cv_working_openssl_ssl="no",
               ac_cv_working_openssl_hashlib="no")
    return env


def build_host(src, host, jobs):
    host.mkdir(exist_ok=True)
    executable = host / "python.exe"
    if executable.exists():
        # A failed full build may already have linked python.exe. Check the
        # extensions needed by generators before accepting that partial cache.
        ready = subprocess.run([executable, "-I", "-c", "import _struct, binascii, math"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ready.returncode == 0:
            return executable
    env = host_environment()
    run([src / "configure", "--without-ensurepip", "--disable-test-modules"], host, env)
    run(["make", f"-j{jobs}"], host, env)
    return executable


def target_dependencies(deps, system_expat=False):
    """Pin explicit static target libraries and their configure inputs."""
    if system_expat and deps is None:
        raise ValueError("--system-expat requires --deps with target Expat")
    env, archives, headers = {}, {}, {}
    libraries = [("ffi", "LIBFFI", ("ffi.h",), "LIBS"),
                 ("z", "ZLIB", ("zlib.h",), "LIBS"),
                 ("bz2", "BZIP2", ("bzlib.h",), "LIBS")]
    if system_expat:
        # CPython 3.13's configure consumes LIBEXPAT_LDFLAGS, not *_LIBS.
        libraries.append(("expat", "LIBEXPAT", ("expat.h", "expat_external.h"), "LDFLAGS"))
    if deps is not None:
        for library, variable, includes, link_variable in libraries:
            archive = deps / f"lib/lib{library}.a"
            if not archive.is_file() or any(not (deps / "include" / name).is_file() for name in includes):
                raise ValueError("Target dependency archive/headers are required: " + library)
            env[variable + "_CFLAGS"] = f"-I{deps / 'include'}"
            env[variable + "_" + link_variable] = str(archive)
            archives[archive.name] = hashlib.sha256(archive.read_bytes()).hexdigest()
            for name in includes:
                headers[name] = hashlib.sha256((deps / "include" / name).read_bytes()).hexdigest()
    return env, archives, headers


def claim_configuration(target, system_expat, archives, headers):
    """Keep incompatible CPython configure caches in separate target roots.

    Accepted older reports without this marker describe the bundled Expat
    default. They may be adopted when their recorded dependency hashes match.
    """
    marker = target / "corvcad-python-config.json"
    expected = {"schema": 1, "system_expat": system_expat,
                "dependency_archives": archives, "dependency_headers": headers}
    if marker.exists():
        if json.loads(marker.read_text()) != expected:
            raise ValueError("CPython configuration differs; use a separate root")
        return
    existing = [path for path in target.iterdir() if path.name != "corvcad-target.json"]
    if existing:
        report = target / "python-build.json"
        if not report.is_file():
            raise ValueError("Unidentified CPython configuration; use a separate root")
        prior = json.loads(report.read_text())
        if prior.get("system_expat", False) != system_expat or prior.get("dependency_archives", {}) != archives:
            raise ValueError("CPython configuration differs from cached report; use a separate root")
    marker.write_text(json.dumps(expected, indent=2) + "\n")


def build(root, sdk, jobs, deps=None, architecture="arm64", system_expat=False):
    selected = target_config.target(sdk, architecture)
    dependency_env, dependency_archives, dependency_headers = target_dependencies(deps, system_expat)
    root.mkdir(parents=True, exist_ok=True)
    src = source(root)
    host = root / "host"
    host_python = build_host(src, host, jobs)
    if subprocess.check_output([host_python, "--version"], text=True).strip() != "Python " + LOCK["python"]["version"]:
        raise ValueError("Host and target CPython versions differ")
    target = root / sdk
    target_config.claim(target, selected, "python-build.json")
    claim_configuration(target, system_expat, dependency_archives, dependency_headers)
    triple = selected.compiler_target
    env = {**os.environ, "PATH": str(src / "iOS/Resources/bin") + ":/usr/bin:/bin:/usr/sbin:/sbin",
           "MODULE_BUILDTYPE": "static", "IPHONEOS_DEPLOYMENT_TARGET": LOCK["minimum_ios"]}
    # Avoid accidentally discovering Homebrew macOS libraries in a cross build.
    for key in ("CFLAGS", "CPPFLAGS", "LDFLAGS", "PKG_CONFIG_PATH", "SDKROOT", "CC", "CXX"):
        env.pop(key, None)
    env["PKG_CONFIG"] = "/usr/bin/false"
    env.update(IOS_CONFIGURE_CACHE)
    env.update(dependency_env)
    run([src / "configure", f"--host={triple}", f"--build={platform.machine()}-apple-darwin",
         f"--with-build-python={host_python}", f"--enable-framework={target / 'framework'}",
         "--without-ensurepip", "--disable-test-modules",
         *(["--with-system-expat"] if system_expat else [])], target, env)
    config_hash = verify_ios_config(target)
    run(["make", f"-j{jobs}", "libpython3.13.a", "pybuilddir.txt"], target, env)
    output = target / "stage"
    output.mkdir(exist_ok=True)
    includes = output / "include/python3.13"
    shutil.copytree(src / "Include", includes, dirs_exist_ok=True)
    shutil.copy2(target / "pyconfig.h", includes / "pyconfig.h")
    stdlib = output / "lib/python3.13"
    shutil.copytree(src / "Lib", stdlib, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("test", "tests", "tkinter", "idlelib", "__pycache__"))
    stage_sysconfig(target, stdlib)
    # Some stdlib modules contain nested convenience archives. Flatten these as
    # well; merely putting a .a inside another archive is not a usable library.
    archives = [target / "libpython3.13.a", *sorted((target / "Modules").rglob("*.a"))]
    run(["/usr/bin/libtool", "-static", "-o", output / "lib/libpython3.13.a", *archives], target)
    shutil.copy2(src / "LICENSE", output / "PYTHON-LICENSE.txt")
    run([host_python, "-m", "compileall", "-q", "--invalidation-mode", "unchecked-hash", "-d", "python3.13", stdlib], target)
    report = {"schema": 1, "sdk": sdk, "architecture": architecture,
              "host_triplet": target_config.host_triplet(), "target": triple, "source": LOCK["python"],
              "configure_cache": IOS_CONFIGURE_CACHE, "pyconfig_sha256": config_hash,
              "system_expat": system_expat,
              "archive_bytes": (output / "lib/libpython3.13.a").stat().st_size,
              "archive_sha256": hashlib.sha256((output / "lib/libpython3.13.a").read_bytes()).hexdigest(),
              "libffi_sha256": hashlib.sha256((deps / "lib/libffi.a").read_bytes()).hexdigest() if deps else None,
              "dependency_archives": dependency_archives,
              "stdlib_bytes": sum(p.stat().st_size for p in stdlib.rglob("*") if p.is_file()),
              "static_archives": [str(p.relative_to(target)) for p in archives],
              "xcode": subprocess.check_output(["xcodebuild", "-version"], text=True).strip(),
              "sdk_version": subprocess.check_output(["xcrun", "--sdk", sdk, "--show-sdk-version"], text=True).strip()}
    (target / "python-build.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("build/freecad-ios/python"))
    parser.add_argument("--sdk", choices=("iphonesimulator", "iphoneos"), required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--deps", type=Path)
    parser.add_argument("--system-expat", action="store_true",
                        help="Use the static Expat in --deps instead of CPython's bundled copy")
    parser.add_argument("--architecture", choices=target_config.ARCHITECTURES, default="arm64")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("Building iOS requires macOS and Xcode")
    if args.system_expat and args.deps is None:
        parser.error("--system-expat requires --deps")
    build(args.root.resolve(), args.sdk, args.jobs, args.deps.resolve() if args.deps else None,
          args.architecture, args.system_expat)

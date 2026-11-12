"""Regression checks for target compilation and artifact boundary failures."""
import json
import hashlib
from pathlib import Path
import tempfile
import unittest

import cross


class TargetCompilation(unittest.TestCase):
    def test_python_dependency_provider_is_bound_to_all_archive_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            (prefix / "lib").mkdir()
            records = {}
            for name in ("libffi.a", "libz.a", "libbz2.a", "libexpat.a"):
                (prefix / "lib" / name).write_bytes(name.encode())
                records[name] = hashlib.sha256(name.encode()).hexdigest()
            old = {"dependency_archives": {k: v for k, v in records.items() if k != "libexpat.a"}}
            self.assertIsNone(cross.verify_python_dependencies(old, prefix))
            shared = {"system_expat": True, "dependency_archives": records}
            self.assertEqual(cross.verify_python_dependencies(shared, prefix), prefix / "lib/libexpat.a")
            with self.assertRaisesRegex(ValueError, "inventory differs"):
                cross.verify_python_dependencies({**old, "system_expat": True}, prefix)
            (prefix / "lib/libexpat.a").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "libexpat"):
                cross.verify_python_dependencies(shared, prefix)

    def check(self, command, sdk="iphonesimulator", architecture="arm64", host="arm64-osx"):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "compile_commands.json").write_text(json.dumps([
                {"file": "/src/module.cpp", "command": command}]))
            return cross.inspect_commands(build, sdk, Path("/deps/installed") / host, architecture)

    def test_target_sdk_and_headers_accepted(self):
        result = self.check("clang++ -arch arm64 -isysroot /Xcode/iPhoneSimulator18.5.sdk -I/deps/installed/arm64-ios-simulator/include -c /src/module.cpp")
        self.assertEqual(result["compilation_commands_checked"], 1)

    def test_macos_sdk_rejected(self):
        with self.assertRaisesRegex(ValueError, "Wrong target SDK"):
            self.check("clang++ -arch arm64 -isysroot /Xcode/MacOSX.sdk -c /src/module.cpp")

    def test_device_sdk_does_not_count_as_simulator(self):
        with self.assertRaisesRegex(ValueError, "Wrong target SDK"):
            self.check("clang++ -arch arm64 -isysroot /Xcode/iPhoneOS18.5.sdk -c /src/module.cpp")

    def test_native_dependency_headers_rejected(self):
        for include in ("/opt/homebrew/include", "/opt/homebrew/opt/qtbase/include", "/deps/installed/arm64-osx/include"):
            with self.subTest(include=include), self.assertRaisesRegex(ValueError, "Host dependency"):
                self.check("clang++ -arch arm64 -isysroot /Xcode/iPhoneSimulator18.5.sdk -I" + include + " -c /src/module.cpp")

    def test_missing_target_flags_rejected(self):
        with self.assertRaisesRegex(ValueError, "Missing explicit"):
            self.check("clang++ -c /src/module.cpp")

    def test_intel_simulator_commands_and_host_header_boundary(self):
        command = "clang++ -arch x86_64 -isysroot /Xcode/iPhoneSimulator18.5.sdk -c /src/module.cpp"
        result = self.check(command, architecture="x86_64", host="x64-osx")
        self.assertEqual(result["architecture"], "x86_64")
        for include in ("/deps/installed/x64-osx/include", "/usr/local/opt/qtbase/include"):
            with self.subTest(include=include), self.assertRaisesRegex(ValueError, "Host dependency"):
                self.check(command + " -I" + include, architecture="x86_64", host="x64-osx")

    def test_universal_or_wrong_architecture_does_not_count_as_intel(self):
        for architecture_flags in ("-arch arm64", "-arch x86_64 -arch arm64"):
            with self.subTest(flags=architecture_flags), self.assertRaisesRegex(ValueError, "Missing explicit x86_64"):
                self.check("clang++ " + architecture_flags + " -isysroot /Xcode/iPhoneSimulator18.5.sdk -c /src/module.cpp", architecture="x86_64", host="x64-osx")


if __name__ == "__main__":
    unittest.main()

"""Actual-header compatibility and raw-input boundary regressions. MIT."""
from pathlib import Path
import tempfile
import unittest

import kicad_build
import verify_inputs


class InputBoundaries(unittest.TestCase):
    def test_changed_wx_configuration_requires_recompile_even_when_archives_link(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old, new = root / "old", root / "new"
            records = {}
            for path in (old, new):
                (path / "lib/wx/include").mkdir(parents=True)
                (path / "lib/wx/include/setup.h").write_bytes(b"#define wxUSE_REGEX 1\n")
            header = old / "lib/wx/include/setup.h"
            records["wx-build/lib/wx/include/setup.h"] = {
                "sha256": kicad_build.sha(header), "sha256_lf": kicad_build.sha(header, text=True)}
            kicad = {"source_inventory": records, "build_claim": {"paths": {"wx_build": str(old)}}}
            proof = verify_inputs.wx_header_compatibility(kicad, {"build": str(new)})
            self.assertEqual(proof["compiled_build"], str(old))
            (new / "lib/wx/include/setup.h").write_bytes(b"#define wxUSE_REGEX 0\n")
            with self.assertRaisesRegex(ValueError, "Compiler input changed"):
                verify_inputs.wx_header_compatibility(kicad, {"build": str(new)})

    def test_no_wx_compiler_evidence_cannot_accept_replacement_toolkit(self):
        with self.assertRaisesRegex(ValueError, "no actual wx"):
            verify_inputs.wx_header_compatibility({"source_inventory": {}}, {"build": "/unused"})

    def test_unclassified_or_escaping_record_cannot_select_host_headers(self):
        for name in ("wx-build/../host.h", "wx-build/./host.h", "unknown/host.h", "C:/host.h"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Unclassified"):
                verify_inputs.verify_files({name: {}}, {"wx-build": Path("/unused")})

    def test_raw_archive_hash_change_is_detected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "libffi.a"
            path.write_bytes(b"accepted raw input")
            records = {str(path): kicad_build.sha(path)}
            verify_inputs.verify_hashes(records)
            path.write_bytes(b"different provider")
            with self.assertRaisesRegex(ValueError, "archive changed"):
                verify_inputs.verify_hashes(records)

    def test_only_reviewed_wx_system_flags_survive_raw_archive_selection(self):
        flags = ["-L/tmp/wx/lib", "-framework", "UIKit", "/tmp/wx/lib/libwx.a",
                 "/tmp/union/lib/libpcre2-32.a", "-lexpat", "-lpthread", "-liconv"]
        self.assertEqual(verify_inputs.wx_system_flags(flags, Path("/tmp/union"), Path("/tmp/wx")),
                         ["-framework", "UIKit", "-lpthread", "-liconv"])
        for unexpected in ("-L/usr/local/lib", "-lforeign", "-Wl,-undefined,dynamic_lookup"):
            with self.subTest(flag=unexpected), self.assertRaises(ValueError):
                verify_inputs.wx_system_flags([unexpected], Path("/tmp/union"), Path("/tmp/wx"))


if __name__ == "__main__":
    unittest.main()

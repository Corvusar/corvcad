"""Regression tests for the source/patch assurance used by gate 1."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import tarfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("kicad_audit", Path(__file__).with_name("audit.py"))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class AuditTests(unittest.TestCase):
    def test_nested_spdx_does_not_erase_gpl(self):
        data = b"/* either version 2\n * of the License, or any later version */\n" \
               b"// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n"
        result = audit.license_info("kicad/include/json_common.h", data)
        self.assertEqual(result["expression"], "GPL-2.0-or-later AND Apache-2.0 WITH LLVM-exception")

    def test_embedded_zint_font_terms_are_retained(self):
        result = audit.license_info("kicad/thirdparty/zint/backend/fonts/normal_woff2.h",
                                    b"/* SPDX-License-Identifier: BSD-3-Clause */\n"
                                    b"/* SPDX-License-Identifier: Apache-2.0 */")
        self.assertEqual(result["expression"], "BSD-3-Clause AND Apache-2.0")

    def test_unreviewed_vendored_input_fails(self):
        with self.assertRaisesRegex(RuntimeError, "Unreviewed"):
            audit.license_info("kicad/thirdparty/new-library/new.h", b"")

    def test_independent_build_scripts_keep_mit_notice(self):
        self.assertEqual(audit.license_info("corvcad/integrations/kicad_core/probe/measure.sh", b"")
                         ["expression"], "MIT")
        self.assertEqual(audit.license_info("corvcad/integrations/kicad_core/test.py",
                         b'value = "SPDX-License-Identifier: example"\n')["expression"], "MIT")

    def test_closure_has_no_build_host_paths(self):
        self.assertEqual(audit.portable_closure("Users/a/repo/integrations/kicad_core/headless/a.cpp"),
                         "corvcad/integrations/kicad_core/headless/a.cpp")
        self.assertEqual(audit.portable_closure("eeschema/schematic.cpp"), "kicad/eeschema/schematic.cpp")

    def test_lf_hashes_match_windows_checkout(self):
        self.assertEqual(audit.sha(b"a\r\nb\r\n"), audit.sha(b"a\nb\n"))

    def test_notice_tampering_or_missing_text_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            notice = audit.write_notice(root, "example", b"retain this notice\n")
            (root / "notices.json").write_text(json.dumps({notice["path"]: notice["sha256_lf"]}))
            self.assertEqual(audit.verify_notices(root), 1)
            (root / notice["path"]).write_bytes(b"truncated notice\n")
            with self.assertRaisesRegex(RuntimeError, "text drifted"):
                audit.verify_notices(root)
            (root / notice["path"]).unlink()
            with self.assertRaisesRegex(RuntimeError, "missing or unrecorded"):
                audit.verify_notices(root)

    def test_notice_keeps_all_terms_without_copying_implementation(self):
        source = b'/* Copyright A; permission notice */\nint implementation;\n' \
                 b'/* SPDX-License-Identifier: Apache-2.0 */\n'
        text = audit.notice_excerpt("example.h", source)
        self.assertIn(b"Copyright A; permission notice", text)
        self.assertIn(b"SPDX-License-Identifier: Apache-2.0", text)
        self.assertNotIn(b"int implementation", text)

    def test_hidden_wx_source_patch_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            wx = root / "wxWidgets-test"
            wx.mkdir()
            (wx / "source.cpp").write_bytes(b"original\n")
            archive = root / "wx.tar.bz2"
            with tarfile.open(archive, "w:bz2") as tar:
                member = tarfile.TarInfo("wxWidgets-test/source.cpp")
                member.size = len(b"original\n")
                tar.addfile(member, io.BytesIO(b"original\n"))
            self.assertEqual(audit.verify_wx_sources(wx, archive)["files"], 1)
            (wx / "source.cpp").write_bytes(b"unrecorded patch\n")
            with self.assertRaisesRegex(RuntimeError, "Undeclared wxWidgets"):
                audit.verify_wx_sources(wx, archive)

    def test_duplicate_or_unlisted_patch_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "patches").mkdir()
            (root / "patches/one.patch").write_text("")
            with patch.object(audit, "HERE", root), patch.dict(audit.LOCK, {"patches": []}):
                with self.assertRaisesRegex(RuntimeError, "every patch"):
                    audit.patches()
            with patch.object(audit, "HERE", root), patch.dict(audit.LOCK, {"patches": ["patches/one.patch"] * 2}):
                with self.assertRaisesRegex(RuntimeError, "every patch"):
                    audit.patches()

    def test_full_series_replay_preserves_user_index_and_detects_drift(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def git(*args):
                return subprocess.check_output(["git", "-C", str(root), *args], stderr=subprocess.STDOUT)
            git("init", "-q")
            git("config", "user.name", "Audit test")
            git("config", "user.email", "audit@example.invalid")
            git("config", "core.autocrlf", "false")
            (root / "model.cpp").write_bytes(b"before\n")
            (root / "other.cpp").write_bytes(b"untouched\n")
            git("add", ".")
            git("commit", "-qm", "fixture")
            commit = git("rev-parse", "HEAD").decode().strip()
            # Two overlapping patches: the reverse check of the first patch is
            # invalid after the second, but whole-series replay is unambiguous.
            first = root / ".git/first.patch"
            second = root / ".git/second.patch"
            (root / "model.cpp").write_bytes(b"middle\n")
            first.write_bytes(git("diff"))
            git("add", "model.cpp")
            (root / "model.cpp").write_bytes(b"after\n")
            second.write_bytes(git("diff"))
            index_before = (root / ".git/index").read_bytes()
            with patch.dict(audit.LOCK, {"schematic_source": {"commit": commit, "tag": "fixture"}}), \
                 patch.object(audit, "patches", return_value=[first, second]):
                tree = audit.SourceTree(root)
                self.assertEqual(tree.read("model.cpp"), b"after\n")
                tree.check_worktree(["model.cpp"])
                self.assertEqual((root / ".git/index").read_bytes(), index_before)
                (root / "model.cpp").write_bytes(b"undeclared change\n")
                with self.assertRaisesRegex(RuntimeError, "differs from declared"):
                    tree.check_worktree(["model.cpp"])
                (root / "model.cpp").write_bytes(b"after\n")
                (root / "other.cpp").write_bytes(b"outside patches\n")
                with self.assertRaisesRegex(RuntimeError, "Undeclared upstream"):
                    tree.check_worktree(["model.cpp"])


if __name__ == "__main__":
    unittest.main()

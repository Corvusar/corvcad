"""Licence evidence precedence and unknown component rejection."""
import unittest
import hashlib
from pathlib import Path
import tempfile
from licenses import classify
from verify_payload import verify


class LicenseTests(unittest.TestCase):
    def setUp(self):
        self.policy = {"vendored": {"src/3rdParty/example/": {"expression": "MIT", "evidence": "COPYING"}},
                       "project_default": {"expression": "LGPL-2.0-or-later", "evidence": "default"}}

    def test_spdx_takes_precedence_over_component_default(self):
        result = classify("src/3rdParty/example/x", "// SPDX-License-Identifier: BSD-2-Clause\n", self.policy)
        self.assertEqual(result["expression"], "(BSD-2-Clause)")

    def test_old_lgpl_grant_keeps_version_two(self):
        banner = "GNU Library General Public\n * License as published by the FSF; either\n * version 2 of the License, or (at your option) any later version."
        self.assertEqual(classify("src/App/x", banner, self.policy)["expression"], "LGPL-2.0-or-later")

    def test_unknown_vendored_component_fails(self):
        with self.assertRaises(ValueError):
            classify("src/3rdParty/unknown/x", "no grant", self.policy)

    def test_multiple_spdx_parts_are_preserved(self):
        result = classify("src/App/x", "SPDX-License-Identifier: MIT\nSPDX-License-Identifier: BSL-1.0", self.policy)
        self.assertEqual(result["declarations"], ["MIT", "BSL-1.0"])

    def test_material_metadata_overrides_project_default(self):
        for value, expected in (("CC-BY-3.0", "CC-BY-3.0"), ("All rights reserved", "LicenseRef-All-Rights-Reserved")):
            result = classify("src/Mod/Material/pattern.FCMat", 'General:\n  License: "'+value+'"\n', self.policy)
            self.assertEqual(result["expression"], expected)

    def test_restricted_payload_cannot_bypass_guard_by_renaming(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/"innocent-name.dat").write_bytes(b"restricted")
            catalogs = [{"production_payload_exclusions": {"pattern.FCMat": {
                "sha256": hashlib.sha256(b"restricted").hexdigest(), "reason": "reserved"}}}]
            with self.assertRaisesRegex(ValueError, "Excluded"):
                verify(root, catalogs)


if __name__ == "__main__":
    unittest.main()

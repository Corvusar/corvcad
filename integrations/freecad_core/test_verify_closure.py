"""Input drift must fail; install timestamps must not masquerade as input drift."""
import copy
import unittest
from verify_closure import compare, signature


class LockTests(unittest.TestCase):
    def setUp(self):
        self.capture = {"status": "captured", "unowned": [], "source_commit": "pin", "profile_sha256": "profile",
                        "source_files": {"schema.xml": {"sha256": "input"}},
                        "integration_inputs": {"integrations/freecad_core/slice.py": "tool-bytes"},
                        "external_files": [{"path": "/lib/tool", "sha256": "bytes", "owners": ["tool"]}],
                        "packages": {"tool": {"version": "1", "notices": [], "receipt": {"sha256": "time"}}},
                        "system_runtime": []}

    def test_changed_recipe_version_header_and_generator_fail(self):
        expected = signature(self.capture)
        for field in ("source_files", "external_files", "packages", "integration_inputs"):
            changed = copy.deepcopy(self.capture)
            if field == "source_files":
                changed[field]["schema.xml"]["sha256"] = "new"
            elif field == "external_files":
                changed[field][0]["sha256"] = "new"
            elif field == "integration_inputs":
                changed[field]["integrations/freecad_core/slice.py"] = "new"
            else:
                changed[field]["tool"]["version"] = "2"
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "drift"):
                compare(expected, changed)

    def test_install_time_does_not_change_content_lock(self):
        expected = signature(self.capture)
        self.capture["packages"]["tool"]["receipt"]["sha256"] = "new-install-time"
        compare(expected, self.capture)

    def test_missing_owned_input_and_unknown_package_fail(self):
        expected = signature(self.capture)
        self.capture["unowned"] = ["/unknown/header"]
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            compare(expected, self.capture)


if __name__ == "__main__":
    unittest.main()

"""Archive variant selection must follow measured consumers, never link order. MIT."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import select_archives as selection


def nm(strong=(), weak=(), undefined=()):
    return ("member.o:\n" + "".join("0000000000000010 (__TEXT,__text) external " + symbol + "\n" for symbol in strong)
            + "".join("0000000000000020 (__TEXT,__text) weak external automatically hidden " + symbol + "\n" for symbol in weak)
            + "".join("                 (undefined) external " + symbol + "\n" for symbol in undefined))


class SymbolParsing(unittest.TestCase):
    def test_strong_weak_and_undefined_are_separate_and_private_externals_remain_providers(self):
        text = nm(["_strong"], ["_weak"], ["_import"])
        text += "0000000000000030 (__DATA,__data) private external _private\n"
        text += "                 (undefined) weak external _optional\n"
        text += "                 (undefined) external _malloc (from libSystem)\n"
        text += "0000000000000040 (__TEXT,__text) non-external _local\n"
        text += "0000000000000050 (absolute) external _absolute\n"
        text += "0000000000000060 (__TEXT,__text) weak private external [cold func] _cold\n"
        text += "0000000000000004 (common) (alignment 2^2) external _common\n"
        text += "0000000000000000 (__TEXT,__text) weak private external [no dead strip] _kept_weak\n"
        self.assertEqual(selection.symbols(text), {
            "strong": {"_strong", "_private", "_absolute", "_common"}, "weak": {"_weak", "_cold", "_kept_weak"},
            "undefined": {"_import", "_optional", "_malloc"}})

    def test_unrecognized_external_rows_fail_closed(self):
        for line in ("new-format external _symbol", "00 (__TEXT,__text) mysterious external _symbol",
                     "00 (undefined) external _symbol", "00 (indirect) external _symbol"):
            with self.subTest(line=line), self.assertRaisesRegex(ValueError, "Unrecognized|Unexpected"):
                selection.symbols("member.o:\n" + line)

    def test_external_without_archive_member_fails(self):
        with self.assertRaisesRegex(ValueError, "member identity"):
            selection.symbols("00 (__TEXT,__text) external _symbol")


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.outputs = {}

    def archive(self, name, output):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(("raw archive " + name).encode())
        self.outputs[str(path.resolve())] = output
        return path

    def select(self, paths):
        def output(command, **kwargs):
            self.assertEqual(command[:4], ["xcrun", "nm", "-g", "-m"])
            return self.outputs[command[4]]
        with patch.object(selection.subprocess, "check_output", side_effect=output):
            return selection.select(paths)

    def test_full_runtime_replaces_shared_lite_exports_in_either_input_order(self):
        core = self.archive("libcore.a", nm(undefined=["_protobuf_shared"]))
        full = self.archive("libprotobuf.a", nm(strong=["_protobuf_shared", "_full_only"]))
        lite = self.archive("libprotobuf-lite.a", nm(strong=["_protobuf_shared", "_lite_unused"]))
        for inputs in ([core, full, lite], [lite, full, core]):
            selected, report = self.select(inputs)
            self.assertEqual(set(selected), {core.resolve(), full.resolve()})
            excluded = report["excluded_archives"][str(lite.resolve())]
            self.assertEqual(excluded["sha256"], selection.digest(lite))
            self.assertEqual(excluded["retained_consumer_checks"]["_protobuf_shared"]["retained_providers"], [str(full.resolve())])
            self.assertEqual(excluded["unique_exports_without_retained_consumer"], 1)
            self.assertFalse(report["missing_retained_providers"])

    def test_new_consumer_of_unique_variant_export_rejects_omission(self):
        core = self.archive("libcore.a", nm(undefined=["_subset_required"]))
        subset = self.archive("libharfbuzz-subset.a", nm(strong=["_subset_required"]))
        with self.assertRaises(selection.OmissionError) as caught:
            self.select([subset, core])
        report = caught.exception.report
        self.assertEqual(report["status"], "rejected")
        self.assertEqual(report["missing_retained_providers"]["_subset_required"], {
            "omitted_providers": [str(subset.resolve())], "retained_consumers": [str(core.resolve())]})

    def test_another_omitted_provider_cannot_satisfy_a_retained_consumer(self):
        core = self.archive("libcore.a", nm(undefined=["_shared_allocator"]))
        boost = self.archive("libboost_container.a", nm(strong=["_shared_allocator"]))
        subset = self.archive("libharfbuzz-subset.a", nm(weak=["_shared_allocator"]))
        with self.assertRaises(selection.OmissionError) as caught:
            self.select([core, boost, subset])
        self.assertEqual(set(caught.exception.report["missing_retained_providers"]["_shared_allocator"]["omitted_providers"]),
                         {str(boost.resolve()), str(subset.resolve())})

    def test_weak_retained_export_is_a_provider_and_dropped_consumer_adds_no_requirement(self):
        core = self.archive("libcore.a", nm(undefined=["_shared"]))
        full = self.archive("libprotobuf.a", nm(weak=["_shared"]))
        lite = self.archive("libprotobuf-lite.a", nm(strong=["_shared"], undefined=["_lite_private_helper"]))
        _, report = self.select([core, full, lite])
        self.assertEqual(report["selected_archives"][str(full.resolve())]["weak_exports"], 1)
        self.assertFalse(report["missing_retained_providers"])

    def test_identical_inputs_and_symlink_alias_are_coalesced_before_symbol_measurement(self):
        core = self.archive("libcore.a", nm(strong=["_core"]))
        alias = self.root / "alias.a"
        try:
            alias.symlink_to(core)
        except OSError:
            self.skipTest("Host cannot create symlinks")
        selected, report = self.select([core, alias])
        self.assertEqual(selected, [core.resolve()])
        self.assertEqual(len(report["selected_archives"]), 1)
        self.assertIn(str(core.resolve()), report["identical_inputs_coalesced"][str(core.resolve())])

    def test_different_same_named_archive_inputs_are_rejected_before_nm(self):
        first = self.archive("first/libcore.a", nm())
        second = self.archive("second/libcore.a", nm())
        with self.assertRaisesRegex(ValueError, "Different builds"):
            self.select([first, second])

    def test_no_pcre_variant_is_omitted_and_no_unrecognized_names_are_dropped(self):
        pcre = self.archive("libpcre2-32.a", nm(strong=["_pcre2_compile_32"]))
        unknown = self.archive("libsome_optional_component.a", nm(strong=["_component"]))
        selected, report = self.select([pcre, unknown])
        self.assertEqual(selected, [pcre.resolve(), unknown.resolve()])
        self.assertFalse(report["excluded_archives"])

    def test_archive_change_during_nm_fails(self):
        core = self.archive("libcore.a", nm())
        def change(*args, **kwargs):
            core.write_bytes(b"changed during measurement")
            return nm()
        with patch.object(selection.subprocess, "check_output", side_effect=change):
            with self.assertRaisesRegex(ValueError, "Archive changed"):
                selection.select([core])


if __name__ == "__main__":
    unittest.main()

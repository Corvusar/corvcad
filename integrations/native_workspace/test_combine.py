"""Regressions for archive-order and Mach-O weak-symbol traps. MIT."""
from pathlib import Path
import tempfile
import unittest
import combine


class ArchiveInputs(unittest.TestCase):
    def test_identical_archives_are_selected_once_but_different_builds_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first, second = root / "first/libz.a", root / "second/libz.a"
            first.parent.mkdir()
            second.parent.mkdir()
            first.write_bytes(b"same target archive")
            second.write_bytes(first.read_bytes())
            selected, aliases = combine.coalesce([first, first, second])
            self.assertEqual(len(selected), 1)
            self.assertIn(str(second.resolve()), aliases[str(first.resolve())])
            second.write_bytes(b"different triplet output")
            with self.assertRaisesRegex(ValueError, "Different builds"):
                combine.coalesce([first, second])

    def test_old_merged_dependencies_cannot_hide_original_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            for name in combine.BUNDLED:
                path = Path(directory) / name
                path.touch()
                with self.assertRaisesRegex(ValueError, "bundled input"):
                    combine.coalesce([path])


class SymbolOverlap(unittest.TestCase):
    def test_apple_weak_definition_is_not_misread_as_a_strong_text_symbol(self):
        output = """
wxexpat_xmlparse.o:
0000000000000010 (__TEXT,__text) external _XML_ParserCreate
0000000000000020 (__TEXT,__text) weak external automatically hidden __ZN3fmt3v12detailEv
                 (undefined) external _malloc
0000000000000030 (__DATA,__data) non-external _local
"""
        self.assertEqual(combine.strong_definitions(output), {"_XML_ParserCreate": {"wxexpat_xmlparse.o"}})

    def test_different_archive_names_do_not_conceal_expat_collision(self):
        output = "xmlparse.o:\n0000000000000010 (__TEXT,__text) external _XML_ParserCreate\n"
        records = {name: combine.strong_definitions(output) for name in ("libwxexpat.a", "libexpat.a")}
        conflict = combine.collisions(records)
        self.assertEqual(len(conflict["_XML_ParserCreate"]), 2)

    def test_duplicate_members_inside_one_archive_are_also_rejected(self):
        output = "a.o:\n00 (__TEXT,__text) external _same\nb.o:\n01 (__TEXT,__text) external _same\n"
        self.assertEqual(len(combine.collisions({"merged.a": combine.strong_definitions(output)})["_same"]), 2)

    def test_same_named_members_keep_separate_strong_definitions(self):
        output = "a.o:\n00 (__TEXT,__text) external _same\na.o:\n01 (__TEXT,__text) external _same\n"
        conflicts = combine.collisions({"merged.a": combine.strong_definitions(output)})
        self.assertEqual(conflicts["_same"], [
            {"archive": "merged.a", "member": "a.o"},
            {"archive": "merged.a", "member": "a.o [occurrence 2]"},
        ])

    def test_occurrence_label_cannot_hide_a_literal_member_name(self):
        output = ("a.o:\n00 (__TEXT,__text) external _same\n"
                  "a.o:\n01 (__TEXT,__text) external _same\n"
                  "a.o [occurrence 2]:\n02 (__TEXT,__text) external _same\n")
        conflicts = combine.collisions({"merged.a": combine.strong_definitions(output)})
        self.assertEqual(len(conflicts["_same"]), 3)

    def test_prefixes_separate_wx_png_and_zlib_from_common_target_libraries(self):
        output = "a.o:\n00 (__TEXT,__text) external _wx_zlib_crc32\n01 (__TEXT,__text) external _wx_png_create_read_struct\n"
        target = "b.o:\n00 (__TEXT,__text) external _crc32\n01 (__TEXT,__text) external _png_create_read_struct\n"
        self.assertFalse(combine.collisions({"wx.a": combine.strong_definitions(output),
                                             "common.a": combine.strong_definitions(target)}))

    def test_unrecognized_external_record_does_not_silently_pass(self):
        with self.assertRaisesRegex(ValueError, "Unrecognized"):
            combine.strong_definitions("member.o:\nnew-format external _symbol\n")


if __name__ == "__main__":
    unittest.main()

"""Shared-Expat wx build policy and compiler-evidence regression tests."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import wx_build as wx


class WxBuildTests(unittest.TestCase):
    def test_private_svg_globals_are_prefixed_without_changing_public_api_or_optimizations(self):
        env = wx.build_environment("iphonesimulator", Path("/sdk"), "x86_64", Path("/union"))
        flags = wx.shlex.split(env["CPPFLAGS"])
        self.assertEqual(flags, ["-I" + str(Path("/union/include")),
                                "-Dnsvg__parseXML=corvcad_wx_nsvg__parseXML",
                                "-Dnsvg__colors=corvcad_wx_nsvg__colors"])
        self.assertNotIn("CXXFLAGS", env)
        self.assertEqual(set(wx.NANOSVG_PRIVATE_PREFIXES), {"nsvg__parseXML", "nsvg__colors"})

    def test_explicit_target_and_only_expat_system_override(self):
        args = wx.configure_command(Path("/source"), Path("/sdk"), "iphonesimulator", "x86_64", Path("/prefix"))
        self.assertIn("--disable-sys-libs", args)
        self.assertIn("--with-expat=sys", args)
        self.assertIn("--enable-macosx_arch=x86_64", args)
        self.assertNotIn("--build=x86_64-apple-darwin", args)
        self.assertNotIn("--with-regex=sys", args)
        device = wx.configure_command(Path("/source"), Path("/sdk"), "iphoneos", "arm64", Path("/prefix"))
        self.assertIn("--build=x86_64-apple-darwin", device)
        with self.assertRaises(ValueError):
            wx.configure_command(Path("/source"), Path("/sdk"), "iphoneos", "x86_64", Path("/prefix"))

    def test_system_regex_configuration_is_explicit_and_cannot_fall_back(self):
        prefix = Path(tempfile.gettempdir()).resolve() / "shared prefix"
        args = wx.configure_command(Path("/source"), Path("/sdk"), "iphonesimulator", "x86_64",
                                    prefix, system_regex=True)
        self.assertIn("--with-regex=sys", args)
        with patch.dict(os.environ, {"LIBPCRE_CFLAGS": "-I/host/include", "LIBPCRE_LIBS": "/host/pcre.a"}):
            env = wx.build_environment("iphonesimulator", Path("/sdk"), "x86_64", prefix, True)
            default = wx.build_environment("iphonesimulator", Path("/sdk"), "x86_64", prefix)
        self.assertEqual(wx.shlex.split(env["LIBPCRE_CFLAGS"]), ["-I" + str(prefix / "include")])
        self.assertEqual(wx.shlex.split(env["LIBPCRE_LIBS"]), [str(prefix / "lib/libpcre2-32.a")])
        self.assertNotIn("LIBPCRE_CFLAGS", default)
        self.assertNotIn("LIBPCRE_LIBS", default)
        wx.verify_configuration("wxUSE_EXPAT='sys'\nwxUSE_REGEX='sys'\n", True)
        with self.assertRaisesRegex(ValueError, "system PCRE2"):
            wx.verify_configuration("wxUSE_EXPAT='sys'\nwxUSE_REGEX='builtin'\n", True)

    def test_inherited_host_flags_cannot_override_target(self):
        with patch.dict(os.environ, {"CC": "host-clang", "CPATH": "/host/include", "LIBRARY_PATH": "/host/lib",
                                     "CXXFLAGS": "-mmacosx-version-min=26", "PKG_CONFIG_PATH": "/host/pkg"}):
            env = wx.build_environment("iphonesimulator", Path("/sdk"), "x86_64", Path("/union"))
        self.assertIn("x86_64-apple-ios17.0-simulator", env["CXX"])
        self.assertEqual(env["PKG_CONFIG"], "/usr/bin/false")
        for key in ("CPATH", "CXXFLAGS", "LIBRARY_PATH", "PKG_CONFIG_PATH"):
            self.assertNotIn(key, env)

    def test_link_selection_rejects_bundled_expat_and_foreign_archives(self):
        root = Path(tempfile.gettempdir()).resolve() / "wx-fixture"
        flags = [str(root / "lib/libwxcore.a"), "-lwxpng", "-lexpat"]
        selected = wx.selected_archives(flags, root)
        self.assertEqual(selected, [root / "lib/libwxcore.a", root / "lib/libwxpng.a"])
        for bad in (flags[:-1], flags + ["-lwxexpat"], flags + [str(root.parent / "foreign.a")]):
            with self.assertRaises(ValueError):
                wx.selected_archives(bad, root)

    def test_compiler_evidence_requires_union_expat(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            source, build, prefix = root / "source", root / "build", root / "prefix"
            (build / ".deps").mkdir(parents=True)
            (source / "src/xml").mkdir(parents=True)
            (source / "src/xml/xml.cpp").write_text("#include <expat.h>\n")
            (prefix / "include").mkdir(parents=True)
            (prefix / "include/expat.h").write_text("/* union expat */\n")
            depfile = build / ".deps/xml.o.d"
            # GCC-style depfiles are macOS paths even when the policy tests run on Windows.
            with patch.object(wx.native, "parse_depfile", return_value=[str(source / "src/xml/xml.cpp"),
                                                                        str(prefix / "include/expat.h")]):
                depfile.write_text("xml.o: source.cpp\n")
                observed = wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang")
            self.assertIn("vcpkg/include/expat.h", observed)
            with patch.object(wx.native, "parse_depfile", return_value=[str(source / "src/xml/xml.cpp")]):
                with self.assertRaisesRegex(ValueError, "no compiler evidence"):
                    wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang")
            (source / "src/expat").mkdir()
            (source / "src/expat/expat.h").write_text("/* old bundled */\n")
            with patch.object(wx.native, "parse_depfile", return_value=[str(source / "src/expat/expat.h")]):
                with self.assertRaisesRegex(ValueError, "Bundled Expat"):
                    wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang")

    def test_shared_regex_archive_is_a_dependency_not_a_wx_product(self):
        root = Path(tempfile.gettempdir()).resolve() / "wx-fixture"
        prefix = root.parent / "union"
        shared = str(prefix / "lib/libpcre2-32.a")
        flags = [str(root / "lib/libwxcore.a"), shared, "-lexpat"]
        self.assertEqual(wx.selected_archives(flags, root, prefix, True), [root / "lib/libwxcore.a"])
        for bad in (flags[:-2] + ["-lexpat"], flags + ["-lwxregexu"],
                    flags + [str(root.parent / "other/libpcre2-32.a")]):
            with self.assertRaises(ValueError):
                wx.selected_archives(bad, root, prefix, True)
        with self.assertRaisesRegex(ValueError, "outside this build"):
            wx.selected_archives(flags, root)

    def test_system_regex_requires_actual_union_header_and_rejects_bundled_inputs(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            source, build, prefix = root / "source", root / "build", root / "prefix"
            (build / ".deps").mkdir(parents=True)
            (build / ".deps/regex.o.d").write_text("regex.o: source.cpp\n")
            (prefix / "include").mkdir(parents=True)
            expat, pcre = prefix / "include/expat.h", prefix / "include/pcre2.h"
            expat.write_text("/* union expat */\n")
            pcre.write_text("/* union pcre2 */\n")
            with patch.object(wx.native, "parse_depfile", return_value=[str(expat), str(pcre)]):
                observed = wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang", True)
            self.assertEqual(observed["vcpkg/include/pcre2.h"]["sha256"], wx.native.sha(pcre))
            with patch.object(wx.native, "parse_depfile", return_value=[str(expat)]):
                with self.assertRaisesRegex(ValueError, "union PCRE2 header"):
                    wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang", True)
            for bundled in (source / "3rdparty/pcre/src/pcre2.h", build / "3rdparty/pcre/src/config.h"):
                bundled.parent.mkdir(parents=True, exist_ok=True)
                bundled.write_text("/* bundled pcre2 */\n")
                with patch.object(wx.native, "parse_depfile", return_value=[str(expat), str(pcre), str(bundled)]):
                    with self.assertRaisesRegex(ValueError, "Bundled PCRE2"):
                        wx.capture_dependencies(build, source, prefix, root / "sdk", root / "clang", True)


if __name__ == "__main__":
    unittest.main()

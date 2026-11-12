#include "corvcad/c_api.h"
#include "corvcad/library.hpp"
#include "corvcad/project.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool okay, const char *message) {
  if (!okay) throw std::runtime_error(message);
}

std::string read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

template <class F> bool refused(F f) {
  try { f(); } catch (const std::exception &) { return true; }
  return false;
}

// A project with a generated symbol library (Chip, an 8-pin box symbol with an SOIC footprint;
// Conn, a 4-pin header) and footprint library (SOIC-8 and a 1x4 header). With
// CORVCAD_TEST_LIBRARY_OUTPUT set, the project is copied there for native-check (KiCad places
// the symbols and adds the footprints through Update PCB).
void build(const fs::path &parent) {
  corvcad::create_project(parent.string(), "Library", 40, 30);
  const std::string folder = (parent / "Library").string();
  corvcad::create_project_library(folder, false, "CorvLib");
  corvcad::create_project_library(folder, true, "CorvLib");
  corvcad::save_library_symbol(folder, "CorvLib", corvcad::parse_symbol_spec(
      "name\tChip\nreference\tU\nfootprint\tCorvLib:Chip_SOIC\ndescription\tGenerated 8-pin part\n"
      "pin\t1\tVCC\tpower_in\tleft\npin\t2\tIN\tinput\tleft\npin\t3\tEN\tinput\tleft\npin\t4\tGND\tpower_in\tleft\n"
      "pin\t5\tOUT\toutput\tright\npin\t6\tNC\tno_connect\tright\npin\t7\tSDA\tbidirectional\tright\n"
      "pin\t8\tSCL\tinput\tright\n"), false);
  corvcad::save_library_symbol(folder, "CorvLib", corvcad::parse_symbol_spec(
      "name\tConn\nreference\tJ\nfootprint\tCorvLib:Header_1x04\n"
      "pin\t1\tP1\tpassive\tleft\npin\t2\tP2\tpassive\tleft\npin\t3\tP3\tpassive\tleft\npin\t4\tP4\tpassive\tleft\n"),
      false);
  corvcad::save_library_footprint(folder, "CorvLib", corvcad::parse_footprint_spec(
      "name\tChip_SOIC\ndescription\tSOIC-8, 1.27 mm pitch\ntype\tsmd\npads\t8\nrows\t2\npitch\t1.27\n"
      "span\t5.4\npad\t1.55\t0.6\n"), false);
  corvcad::save_library_footprint(folder, "CorvLib", corvcad::parse_footprint_spec(
      "name\tHeader_1x04\ntype\ttht\npads\t4\nrows\t1\npitch\t2.54\npad\t1.7\t1.7\ndrill\t1\n"), false);
}

int main() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path parent = fs::temp_directory_path() / ("corvcad-library-test-" + std::to_string(nonce));
  fs::create_directories(parent);
  try {
    build(parent);
    const fs::path folder = parent / "Library";
    const std::string project = folder.string();
    const std::string listing = corvcad::project_libraries_json(project);
    require(listing.find("\"name\":\"CorvCAD\",\"uri\":\"${KIPRJMOD}/CorvCAD.kicad_sym\",\"editable\":false") !=
                std::string::npos, "the starter library is listed read-only");
    require(listing.find("{\"name\":\"Chip\",\"reference\":\"U\",\"value\":\"\"") == std::string::npos &&
                listing.find("\"name\":\"Chip\",\"reference\":\"U\",\"value\":\"Chip\",\"footprint\":\"CorvLib:Chip_SOIC\"") !=
                    std::string::npos, "generated symbol listed with its fields");
    require(listing.find("[\"5\",\"OUT\",\"output\",\"right\"]") != std::string::npos, "pins listed with their side");
    require(listing.find("\"name\":\"Chip_SOIC\",\"description\":\"SOIC-8, 1.27 mm pitch\",\"smd\":true,\"pads\":[[-2.7,-1.905,") !=
                std::string::npos, "footprint pads listed from the board reader");
    // Pad 8 sits opposite pad 1 (numbered up the right side, as SOIC).
    require(listing.find("[2.7,-1.905,1.55,0.6,0,\"roundrect\",\"8\",\"smd\"]") != std::string::npos, "SOIC numbering");
    require(listing.find("\"name\":\"Header_1x04\",\"description\":\"\",\"smd\":false,\"pads\":[[-3.81,0,1.7,1.7,0,\"rect\",\"1\"") !=
                std::string::npos, "header pads");
    // The symbol drawing: a body and 8 pins, ends on the 2.54 mm grid.
    const auto chip = listing.find("\"name\":\"Chip\"");
    require(listing.find("\"shape\":{\"graphics\":[[1,2,", chip) != std::string::npos &&
                listing.find("[-7.62,-5.08,-5.08,-5.08,\"1\",\"VCC\"]", chip) != std::string::npos,
            "symbol drawn through the schematic reader");

    // Editing: replace needs the flag, the starter library is read-only, names are checked.
    const auto chip_spec = corvcad::parse_symbol_spec("name\tChip\npin\t1\tA\tpassive\tleft\n");
    require(refused([&] { corvcad::save_library_symbol(project, "CorvLib", chip_spec, false); }), "no silent overwrite");
    corvcad::save_library_symbol(project, "CorvLib", chip_spec, true);
    require(corvcad::project_libraries_json(project).find("[\"1\",\"A\",\"passive\",\"left\"]") != std::string::npos,
            "symbol replaced");
    require(refused([&] { corvcad::save_library_symbol(project, "CorvCAD", chip_spec, true); }), "starter read-only");
    require(refused([&] { corvcad::create_project_library(project, false, "../Escape"); }), "library name checked");
    require(refused([&] { corvcad::library_symbol_text(corvcad::parse_symbol_spec("name\tX\npin\t1\tA\tpassive\tleft\npin\t1\tB\tpassive\tleft\n")); }),
            "duplicate pin numbers refused");
    require(refused([&] { corvcad::library_footprint_text(corvcad::parse_footprint_spec("name\tX\npads\t3\nrows\t2\n")); }),
            "two rows need an even count");
    require(refused([&] { corvcad::library_footprint_text(corvcad::parse_footprint_spec("name\tX\npads\t4\npitch\t1\npad\t1.7\t1.7\n")); }),
            "overlapping pads refused");
    corvcad::delete_library_item(project, false, "CorvLib", "Chip");
    corvcad::delete_library_item(project, true, "CorvLib", "Header_1x04");
    const std::string after = corvcad::project_libraries_json(project);
    // (Conn's Footprint field still names CorvLib:Header_1x04; only the footprint is gone.)
    require(after.find("\"name\":\"Chip\"") == std::string::npos && after.find("\"name\":\"Header_1x04\"") == std::string::npos &&
                after.find("\"name\":\"Conn\"") != std::string::npos, "items deleted, others kept");

    // Import: a copied library and its row; the same name again is refused. Remove deletes both.
    const fs::path outside = parent / "Imported.kicad_sym";
    fs::copy_file(folder / "CorvLib.kicad_sym", outside);
    char error[256]{};
    require(cc_import_project_library(project.c_str(), outside.string().c_str(), error, sizeof error) == 1 &&
                fs::exists(folder / "Imported.kicad_sym") &&
                read(folder / "sym-lib-table").find("(lib (name \"Imported\") (type \"KiCad\") (uri \"${KIPRJMOD}/Imported.kicad_sym\")") !=
                    std::string::npos, "C API import adds the copy and its row");
    require(cc_import_project_library(project.c_str(), outside.string().c_str(), error, sizeof error) == 0 && *error,
            "importing the same library twice is refused");
    require(cc_remove_project_library(project.c_str(), 0, "Imported", error, sizeof error) == 1 &&
                !fs::exists(folder / "Imported.kicad_sym") &&
                read(folder / "sym-lib-table").find("Imported") == std::string::npos, "remove deletes row and copy");
    require(cc_remove_project_library(project.c_str(), 0, "CorvCAD", error, sizeof error) == 0, "starter library stays");
    char *generated = cc_library_generate(1, "name\tP\ntype\tsmd\npads\t2\npitch\t2\npad\t1\t1\n", error, sizeof error);
    char *form = generated ? cc_library_item_json(1, generated, nullptr, error, sizeof error) : nullptr;
    require(form && std::string(form).find("{\"i\":0,\"number\":\"1\",\"type\":\"smd\",\"shape\":\"roundrect\",\"x\":-1,\"y\":0") !=
                std::string::npos, "C API footprint template and editor form");
    cc_free_text(generated);
    cc_free_text(form);

    // Symbol edits on Conn's text: unmodelled items survive, pins and shapes change one at a time.
    std::string conn = corvcad::library_item_text(project, false, "CorvLib", "Conn");
    const auto body = conn.find("(symbol \"Conn_1_1\"") + 18;
    conn.insert(body, " (text \"HELLO\" (at 0 0 0) (effects (font (size 1.27 1.27))))");
    conn = corvcad::library_edit(false, conn, "pin\tmove\t0\t-10.16\t5.08\t0");
    conn = corvcad::library_edit(false, conn, "pin\tset\t1\t2\tCLK\tinput");
    conn = corvcad::library_edit(false, conn, "shape\tadd\t1\tcircle\t1\t0\t0\t1.27");
    conn = corvcad::library_edit(false, conn, "shape\tmove\t0\t1.27\t0");
    conn = corvcad::library_edit(false, conn, "units\t2");
    conn = corvcad::library_edit(false, conn, "pin\tadd\t2\t5\tGND\tpower_in\t0\t-7.62\t90\t2.54");
    conn = corvcad::library_edit(false, conn, "field\tFootprint\tCorvLib:Chip_SOIC");
    conn = corvcad::library_edit(false, conn, "name\tConnector");
    require(refused([&] { corvcad::library_edit(false, conn, "pin\tadd\t1\t5\tX\tpassive\t0\t0\t0\t2.54"); }),
            "a pin number used in another unit is refused");
    const std::string conn_form = corvcad::library_item_json(false, conn);
    require(conn.find("HELLO") != std::string::npos && conn.find("(symbol \"Connector_2_1\"") != std::string::npos &&
                conn_form.find("\"units\":2") != std::string::npos &&
                conn_form.find("{\"i\":0,\"unit\":1,\"number\":\"1\",\"name\":\"P1\",\"type\":\"passive\",\"x\":-10.16,\"y\":5.08") !=
                    std::string::npos &&
                conn_form.find("\"number\":\"2\",\"name\":\"CLK\",\"type\":\"input\"") != std::string::npos &&
                conn_form.find("\"unit\":2,\"number\":\"5\",\"name\":\"GND\",\"type\":\"power_in\",\"x\":0,\"y\":-7.62,\"angle\":90") !=
                    std::string::npos &&
                conn_form.find("\"Footprint\":\"CorvLib:Chip_SOIC\"") != std::string::npos,
            "symbol edits: pin move and set, circle, units, pin in unit 2, field, rename; text kept");
    corvcad::save_library_item_text(project, false, "CorvLib", "Conn", conn, false);
    const std::string renamed = corvcad::project_libraries_json(project);
    require(renamed.find("\"name\":\"Connector\"") != std::string::npos && renamed.find("\"name\":\"Conn\"") == std::string::npos,
            "saved under its new name, replacing the old");

    // A derived symbol: its own fields over the base's drawing; pins belong to the base.
    std::string derived = "(symbol \"Connector2\" (extends \"Connector\") (property \"Reference\" \"J\" (at 0 0 0)"
                          " (effects (font (size 1.27 1.27)))) (property \"Value\" \"Connector2\" (at 0 0 0)"
                          " (effects (font (size 1.27 1.27)))))";
    corvcad::save_library_item_text(project, false, "CorvLib", "", derived, false);
    derived = corvcad::library_edit(false, derived, "field\tDescription\tDerived part");
    require(refused([&] { corvcad::library_edit(false, derived, "pin\tdelete\t0"); }), "derived symbols keep the base's pins");
    const std::string derived_form = corvcad::library_item_json(false, derived, conn);
    require(derived_form.find("\"derived\":true,\"base\":\"Connector\",\"units\":2") != std::string::npos &&
                derived_form.find("\"Description\":\"Derived part\"") != std::string::npos &&
                derived_form.find("\"name\":\"CLK\"") != std::string::npos, "derived form draws the base");
    require(refused([&] { corvcad::save_library_item_text(project, false, "CorvLib", "Connector", conn, true); }) == false,
            "the base saves in place");
    require(refused([&] { corvcad::delete_library_item(project, false, "CorvLib", "Connector"); }),
            "a base with a derived symbol stays");

    // Footprint edits: pad move and set keep the pad's other attributes; shapes add and move.
    std::string soic = corvcad::library_item_text(project, true, "CorvLib", "Chip_SOIC");
    const auto pad = soic.find("(pad \"1\"");
    soic.insert(soic.find(')', soic.find("(roundrect_rratio", pad)) + 1, " (solder_mask_margin 0.05)");
    soic = corvcad::library_edit(true, soic, "pad\tmove\t0\t-3\t-1.905");
    soic = corvcad::library_edit(true, soic, "pad\tset\t0\t1\tsmd\trect\t2\t0.6");
    soic = corvcad::library_edit(true, soic, "pad\tadd\tEP\tsmd\trect\t0\t0\t2\t3");
    soic = corvcad::library_edit(true, soic, "shape\tadd\tline\tF.SilkS\t-1\t-3\t1\t-3\t0.12");
    soic = corvcad::library_edit(true, soic, "shape\tmove\t3\t0\t-0.5");
    require(refused([&] { corvcad::library_edit(true, soic, "pad\tset\t0\t1\tthru_hole\tcircle\t1\t1\t2"); }),
            "a drill larger than the pad is refused");
    const std::string soic_form = corvcad::library_item_json(true, soic);
    require(soic.find("(pad \"1\" smd rect (at -3 -1.905) (size 2 0.6) (layers \"F.Cu\" \"F.Mask\" \"F.Paste\") (solder_mask_margin 0.05))") !=
                    std::string::npos &&
                soic_form.find("\"number\":\"EP\",\"type\":\"smd\",\"shape\":\"rect\",\"x\":0,\"y\":0") != std::string::npos &&
                soic_form.find("{\"i\":3,\"kind\":\"fp_line\",\"layer\":\"F.SilkS\",\"width\":0.12,\"closed\":false,\"text\":\"\",\"points\":[-1,-3.5,1,-3.5]}") !=
                    std::string::npos,
            "footprint edits: pad move, set (margin kept), add, line add and move");
    corvcad::save_library_item_text(project, true, "CorvLib", "Chip_SOIC", soic, false);

    // One footprint file into a library.
    const fs::path single = parent / "Single.kicad_mod";
    fs::copy_file(folder / "CorvLib.pretty" / "Chip_SOIC.kicad_mod", single);
    require(cc_import_library_footprint(project.c_str(), "CorvLib", single.string().c_str(), error, sizeof error) == 1 &&
                corvcad::project_libraries_json(project).find("\"name\":\"Chip_SOIC\"") != std::string::npos &&
                fs::exists(folder / "CorvLib.pretty" / "Single.kicad_mod"), "single footprint import");
    require(cc_import_library_footprint(project.c_str(), "CorvLib", single.string().c_str(), error, sizeof error) == 0,
            "the same footprint twice is refused");

    if (const char *output = std::getenv("CORVCAD_TEST_LIBRARY_OUTPUT")) {
      const fs::path again = parent / "again";
      fs::create_directories(again);
      build(again);
      fs::remove_all(output);
      fs::copy(again / "Library", output, fs::copy_options::recursive);
    }
  } catch (...) {
    fs::remove_all(parent);
    throw;
  }
  fs::remove_all(parent);
}

#include "corvcad/project.hpp"
#include "corvcad/authoring.hpp"
#include "corvcad/schematic_session.hpp"
#include "corvcad/schematic_session_c_api.h"
#include "corvcad/c_api.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
void require(bool okay, const char *message) {
  if (!okay) throw std::runtime_error(message);
}
std::string read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void write(const fs::path &path, const std::string &contents) {
  std::ofstream stream(path, std::ios::binary);
  stream << contents;
  require(static_cast<bool>(stream), "test write");
}

int main() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() /
      ("corvcad-session-test-" + std::to_string(nonce));
  fs::create_directory(root);
  try {
    corvcad::create_project(corvcad::utf8_string(root), "Demo", 40, 30);
    const fs::path folder = root / "Demo";
    const fs::path main = folder / "Demo.kicad_sch";
    const fs::path child = folder / "Child.kicad_sch";
    const std::string child_template =
        "(kicad_sch (version 20250114) (generator \"eeschema\") (uuid \"child-root\")"
        " (paper \"A4\") (lib_symbols) (embedded_fonts no)"
        " (title_block (title \"keep me\")))\n";
    const std::string child_original =
        corvcad::add_schematic_two_pin(child_template, "Demo", 20.32, 20.32);
    write(child, child_original);
    auto parent = read(main);
    parent.insert(parent.rfind(')'),
        " (sheet (at 20 20) (size 20 15) (uuid \"child-instance\")"
        " (property \"Sheetname\" \"Sub\" (at 20 20 0))"
        " (property \"Sheetfile\" \"Child.kicad_sch\" (at 20 22 0)))"
        " (sheet (at 50 20) (size 20 15) (uuid \"child-instance-2\")"
        " (property \"Sheetname\" \"Sub Again\" (at 50 20 0))"
        " (property \"Sheetfile\" \"Child.kicad_sch\" (at 50 22 0)))\n");
    write(main, parent);

    auto session = corvcad::SchematicSession::open(corvcad::utf8_string(main));
    require(session.sheets().size() == 3, "multi-sheet discovery");
    const std::string path = session.sheets().at(1).path;
    require(path.find("/child-instance") != std::string::npos, "stable sheet instance path");
    require(session.sheet(path).uuid == "child-root", "child model");
    require(session.sheet(path).symbols.size() == 1, "child symbol");
    const std::string symbol_uuid = session.sheet(path).symbols[0].uuid;
    session.begin();
    bool rejected = false;
    try { session.rename_symbol(path, symbol_uuid, "R9"); }
    catch (const corvcad::SchematicError &error) {
      rejected = error.code() == corvcad::SchematicErrorCode::Unsupported;
    }
    require(rejected, "shared-sheet reference rename refused");
    session.abort();
    session.begin();
    session.add_wire(path, 0, 0, 2.54, 0);
    session.move_symbol(path, symbol_uuid, 25.4, 20.32);
    session.set_property(path, symbol_uuid, "Value", "22k");
    session.commit();
    require(session.sheet(path).wires.size() == 1, "transaction committed");
    require(session.sheet(path).symbols[0].position.x == 25.4 &&
            session.sheet(path).symbols[0].value == "22k", "grouped symbol edits");
    require(read(child) == child_original, "commit does not write until save");
    require(session.undo() && session.sheet(path).wires.empty() &&
            session.sheet(path).symbols[0].position.x == 20.32, "grouped undo");
    require(session.redo() && session.sheet(path).wires.size() == 1, "redo");
    require(session.sheets().at(1).name == "Sub" && session.sheets().at(2).name == "Sub Again",
            "child sheet instances carry their sheet names");
    require(session.can_undo() && !session.can_redo(), "undo availability");
    const std::string root_path = session.sheets().front().path;
    session.begin();
    rejected = false;
    try { session.add_two_pin(path, 60.96, 60.96); }
    catch (const corvcad::SchematicError &error) {
      rejected = error.code() == corvcad::SchematicErrorCode::Unsupported;
    }
    require(rejected, "part add refused in a shared sheet");
    session.add_two_pin(root_path, 50.8, 50.8);
    session.add_routed_wire(root_path, 45.72, 50.8, 45.72, 60.96);
    session.add_label(root_path, "IN", 45.72, 60.96);
    session.commit();
    require(session.sheet(root_path).symbols.size() == 1 &&
            session.sheet(root_path).symbols[0].reference == "R2",
            "new part reference is unique across sheets");
    require(session.source(root_path).find("(path \"" + root_path + "\"") != std::string::npos,
            "new part records its sheet instance path");
    const std::string label_uuid = session.sheet(root_path).labels[0].uuid;
    session.begin();
    session.rename_label(root_path, label_uuid, "VIN");
    session.move_label(root_path, label_uuid, 45.72, 63.5);
    session.commit();
    require(session.sheet(root_path).labels[0].text == "VIN" &&
            session.sheet(root_path).labels[0].position.y == 63.5, "label edits");
    require(session.route(root_path, 45.72, 40.64, 60.96, 40.64).size() >= 2, "route preview");
    require(session.undo() && session.undo() && session.sheet(root_path).symbols.empty(),
            "authoring undo");
    require(session.redo() && session.redo() && !session.can_redo(), "authoring redo");
    session.begin();
    rejected = false;
    try { session.delete_item(path, "missing"); } catch (...) { rejected = true; }
    require(rejected, "bad edit rejected");
    session.abort();
    require(session.sheet(path).wires.size() == 1, "abort preserves committed state");

    const fs::path copy = root / "Copy";
    session.save_copy(corvcad::utf8_string(copy));
    require(read(child) == child_original, "save copy preserves original");
    require(read(copy / "Child.kicad_sch").find("(wire") != std::string::npos,
            "save copy includes edited child");
    require(read(copy / "Child.kicad_sch").find("(title_block (title \"keep me\"))") !=
            std::string::npos, "known unmodelled expression preserved");
    session.save();
    require(read(child).find("(wire") != std::string::npos, "original save");
    const std::string child_saved = read(child);
    session.begin();
    session.add_label(path, "TEST", 5.08, 0);
    session.add_label(session.sheets().front().path, "ROOT", 5.08, 0);
    session.commit();
    write(main, read(main) + "\n; external edit\n");
    rejected = false;
    try { session.save(); } catch (...) { rejected = true; }
    require(rejected && read(main).find("external edit") != std::string::npos &&
            read(child) == child_saved, "concurrent disk edit leaves all originals intact");

    cc_sch_result result{};
    auto *handle = cc_sch_session_open(corvcad::utf8_string(main).c_str(), &result);
    require(handle && result.code == 0, "C ABI open");
    require(cc_sch_session_validate_structure(handle, &result), "C ABI structure validation");
    require(cc_sch_api_version() == 11 && !(cc_sch_session_capabilities(handle) & CC_SCH_PLOT) &&
            !(cc_sch_session_capabilities(handle) & CC_SCH_SIMULATE) &&
            !cc_sch_session_simulate(handle, ".op", 0, &result) && result.code == CC_SCH_UNSUPPORTED &&
            !cc_sch_session_spice_netlist(handle, ".op", &result) && result.code == CC_SCH_UNSUPPORTED &&
            !cc_sch_session_plot(handle, 0, "x.pdf", &result) && result.code == CC_SCH_UNSUPPORTED && !(cc_sch_session_capabilities(handle) & CC_SCH_LIBRARY) &&
            !(cc_sch_session_capabilities(handle) & CC_SCH_KICAD_TOOLS) &&
            !cc_sch_session_netlist(handle, &result) && result.code == CC_SCH_UNSUPPORTED &&
            !cc_sch_session_erc_json(handle, &result) && result.code == CC_SCH_UNSUPPORTED &&
            !cc_sch_session_bom_csv(handle, &result) && result.code == CC_SCH_UNSUPPORTED,
            "compatible backend refuses KiCad checks");
    {
      auto *tools = cc_sch_transaction_begin(handle, &result);
      require(tools && !cc_sch_transaction_annotate(tools, 0, &result) && result.code == CC_SCH_UNSUPPORTED &&
              !cc_sch_transaction_add_sheet(tools, "/", "Child", 0, 0, 20, 20, &result) &&
              result.code == CC_SCH_UNSUPPORTED, "compatible backend refuses KiCad's editing tools");
      cc_sch_transaction_free(tools);
    }
    require(
            (cc_sch_session_capabilities(handle) & CC_SCH_HIERARCHY) &&
            !(cc_sch_session_capabilities(handle) & CC_SCH_NATIVE_KICAD),
            "C ABI reports actual backend capability");
    auto *json = cc_sch_session_snapshot_json(handle, &result);
    require(json && std::string(json).find("child-instance") != std::string::npos,
            "C ABI snapshot exposes sheet identity");
    require(std::string(json).find("\"sharedFile\":true") != std::string::npos,
            "C ABI marks shared sheet files");
    cc_free_text(json);
    auto *transaction = cc_sch_transaction_begin(handle, &result);
    require(transaction, "C ABI transaction begin");
    if (!cc_sch_transaction_add_label(transaction, path.c_str(),
                                     "NET", 7.62, 0, &result))
      throw std::runtime_error(std::string("C ABI transaction edit: ") +
                               (result.message ? result.message : "unknown"));
    require(cc_sch_transaction_commit(transaction, &result), "C ABI transaction commit");
    cc_sch_transaction_free(transaction);
    require(cc_sch_session_undo(handle, &result), "C ABI undo");
    require(cc_sch_session_can_redo(handle) && !cc_sch_session_can_undo(handle),
            "C ABI undo availability");
    char *sheet_text = cc_sch_session_sheet_source(handle, path.c_str(), &result);
    require(sheet_text && std::string(sheet_text).find("(kicad_sch") != std::string::npos,
            "C ABI sheet source");
    cc_free_text(sheet_text);
    double xy[16]{};
    require(cc_sch_session_route(handle, path.c_str(), 5.08, 5.08, 15.24, 5.08, xy, 8,
                                 &result) >= 2, "C ABI route");
    transaction = cc_sch_transaction_begin(handle, &result);
    require(transaction && !cc_sch_transaction_add_two_pin(transaction, path.c_str(), 60.96,
                                                           60.96, &result) &&
            result.code == CC_SCH_UNSUPPORTED, "C ABI refuses part add in shared sheet");
    cc_sch_transaction_free(transaction);
    cc_sch_session_free(handle);
    cc_sch_result_clear(&result);

    auto future = read(main);
    const auto old = future.find("20250114");
    require(old != std::string::npos, "test version found");
    future.replace(old, 8, "20270101");
    write(main, future);
    auto new_version = corvcad::SchematicSession::open(corvcad::utf8_string(main));
    rejected = false;
    try { new_version.save_copy(corvcad::utf8_string(root / "FutureCopy")); }
    catch (...) { rejected = true; }
    require(rejected && !fs::exists(root / "FutureCopy"), "future format refused before write");
    cc_sch_result future_result{};
    auto *future_handle = cc_sch_session_open(corvcad::utf8_string(main).c_str(), &future_result);
    require(future_handle, "future project can open read-only");
    require(!cc_sch_session_validate_structure(future_handle, &future_result) &&
            future_result.code == CC_SCH_UNSUPPORTED,
            "C ABI distinguishes future format from ERC");
    require(!cc_sch_session_save_copy(future_handle, corvcad::utf8_string(root / "FutureCopy").c_str(),
                                      &future_result) && future_result.code == CC_SCH_UNSUPPORTED,
            "C ABI reports unsupported format explicitly");
    cc_sch_result_clear(&future_result);
    cc_sch_session_free(future_handle);

    future.replace(future.find("20270101"), 8, "20250114");
    future.insert(future.rfind(')'), " (future_note \"unsupported\")");
    write(main, future);
    auto unknown = corvcad::SchematicSession::open(corvcad::utf8_string(main));
    require(unknown.sheet(unknown.sheets().front().path).unknown_top_level.size() == 1,
            "unknown top-level construct reported");
    cc_sch_result unknown_result{};
    auto *unknown_handle = cc_sch_session_open(corvcad::utf8_string(main).c_str(), &unknown_result);
    require(unknown_handle, "unknown project can open read-only");
    auto *unknown_json = cc_sch_session_snapshot_json(unknown_handle, &unknown_result);
    require(unknown_json && std::string(unknown_json).find(
            "\"unsupportedConstructs\":[\"future_note\"]") != std::string::npos,
            "unknown construct visible to UI");
    cc_free_text(unknown_json);
    require(!cc_sch_session_validate_structure(unknown_handle, &unknown_result) &&
            unknown_result.code == CC_SCH_UNSUPPORTED,
            "unknown construct rejected before C ABI save");
    cc_sch_result_clear(&unknown_result);
    cc_sch_session_free(unknown_handle);
    rejected = false;
    try { unknown.save_copy(corvcad::utf8_string(root / "UnknownCopy")); }
    catch (const corvcad::SchematicError &error) {
      rejected = error.code() == corvcad::SchematicErrorCode::Unsupported;
    }
    require(rejected && !fs::exists(root / "UnknownCopy"),
            "unknown construct refused before write");
  } catch (...) {
    fs::remove_all(root);
    throw;
  }
  fs::remove_all(root);
}

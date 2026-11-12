#include "corvcad/authoring.hpp"
#include "corvcad/c_api.h"
#include "corvcad/eda.hpp"
#include "corvcad/engine.hpp"
#include "corvcad/project.hpp"
#include "corvcad/schematic_route.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool okay, const char *message) {
  if (!okay) throw std::runtime_error(message);
}

std::string read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "project file opens");
  return {std::istreambuf_iterator<char>(file), {}};
}

// Update PCB from schematic on the touch-authored round-trip project. The expected pad nets
// are what KiCad's audit (integrations/kicad_native) accepts as consistent with its netlist.
void test_update_board() {
  const fs::path folder = CORVCAD_ROUNDTRIP_DIR;
  const std::string board = read(folder / "RoundTrip.kicad_pcb");
  const std::string schematic = read(folder / "RoundTrip.kicad_sch");
  const auto pad_net = [](const std::string &text, const std::string &reference, const std::string &pad) {
    const auto parsed = corvcad::parse_kicad_board(text);
    for (const auto &item : parsed.pads)
      if (item.reference == reference && item.number == pad)
        for (const auto &net : parsed.nets)
          if (net.code == item.net) return net.name;
    return std::string("-");
  };
  const auto update = corvcad::update_board_from_schematic(board, schematic);
  require(update.added.empty() && update.skipped.empty(), "every round-trip part is on the board");
  require(pad_net(update.board, "R1", "1") == "/NET1" && pad_net(update.board, "R1", "2") == "Net-(R1-Pad2)" &&
              pad_net(update.board, "R2", "1") == "Net-(R1-Pad2)" && pad_net(update.board, "R2", "2") == "/NET1" &&
              pad_net(update.board, "R3", "1") == "Net-(R1-Pad2)" && pad_net(update.board, "R3", "2") == "/NET1",
          "pad nets follow the schematic");
  require(update.board.find("(net \"/NET1\") (uuid") != std::string::npos, "KiCad 10 named-net form before uuid");
  require(corvcad::update_board_from_schematic(update.board, schematic).board == update.board,
          "a second update changes nothing");
  // Only pad nets were added: removing them gives back the original board byte for byte.
  std::string stripped = update.board;
  for (size_t at; (at = stripped.find(" (net \"")) != std::string::npos;)
    stripped.erase(at, stripped.find("\")", at) + 2 - at);
  require(stripped == board, "unrelated board text unchanged");

  // A relabelled net moves with the schematic; a part missing from the board is added.
  std::string relabelled = schematic;
  for (size_t at; (at = relabelled.find("\"NET1\"")) != std::string::npos;) relabelled.replace(at, 6, "\"VOUT\"");
  const auto renamed = corvcad::update_board_from_schematic(update.board, relabelled);
  require(pad_net(renamed.board, "R1", "1") == "/VOUT", "renamed net updated on pads");
  const auto without_r2 = corvcad::delete_footprint(board, "R2");
  const auto restored = corvcad::update_board_from_schematic(without_r2, schematic);
  require(restored.added == std::vector<std::string>{"R2"} && pad_net(restored.board, "R2", "2") == "/NET1",
          "missing footprint added with its nets");
  require(pad_net(restored.board, "H1", "") == "-", "mounting holes untouched");

  // Copper: a track between R1 pad 2 and R2 pad 1, then a via; deleting restores the text.
  const auto routed = corvcad::add_board_track(update.board, 20.8, 20, 39.2, 20, "F.Cu", "Net-(R1-Pad2)");
  require(routed.find("(segment (start 20.8 20) (end 39.2 20) (width 0.25) (layer \"F.Cu\") "
                      "(net \"Net-(R1-Pad2)\") (uuid") != std::string::npos, "track in KiCad 10 form");
  const auto routed_board = corvcad::parse_kicad_board(routed);
  require(routed_board.tracks.size() == 1 && !routed_board.tracks[0].uuid.empty() &&
              pad_net(routed, "R2", "1") == "Net-(R1-Pad2)", "track parses with its uuid");
  const auto with_via = corvcad::add_board_via(routed, 30, 25, "/NET1");
  const auto via_board = corvcad::parse_kicad_board(with_via);
  require(via_board.vias.size() == 1 && via_board.vias[0].size == 0.6 && via_board.vias[0].drill == 0.3,
          "via with KiCad's default size");
  require(corvcad::delete_board_copper(with_via, via_board.vias[0].uuid) == routed, "via deleted cleanly");
  {
    char error[256]{};
    char *wide = cc_add_board_track(update.board.c_str(), 30, 25, 40, 25, "F.Cu", "/NET1", 0.4, error, sizeof(error));
    char *sized = cc_add_board_via(update.board.c_str(), 30, 25, "/NET1", 0.8, 0.4, error, sizeof(error));
    require(wide && sized, "C API track and via with netclass sizes");
    const auto wide_board = corvcad::parse_kicad_board(wide), sized_board = corvcad::parse_kicad_board(sized);
    require(wide_board.tracks.back().width == 0.4 && sized_board.vias.back().size == 0.8 &&
                sized_board.vias.back().drill == 0.4, "netclass track width and via size written");
    cc_free_text(wide);
    cc_free_text(sized);
  }
  bool stacked = false;
  try { corvcad::add_board_via(with_via, 30.1, 25, "/NET1"); } catch (...) { stacked = true; }
  require(stacked, "a second via on an existing drill is refused");
  require(corvcad::parse_kicad_board(corvcad::add_board_via(with_via, 31, 25, "/NET1")).vias.size() == 2,
          "a via beside it is fine");
  require(corvcad::delete_board_copper(routed, routed_board.tracks[0].uuid) == update.board, "track deleted cleanly");
  bool refused = false;
  try { corvcad::add_board_track(update.board, 1, 1, 2, 1, "In1.Cu", ""); } catch (...) { refused = true; }
  require(refused, "inner layers refused");
  refused = false;
  try { corvcad::add_board_track(update.board, 1, 1, 200, 1, "F.Cu", ""); } catch (...) { refused = true; }
  require(refused, "track outside the outline refused");

  // Drawing: outline, zone, silkscreen line and text; each deletes back to the text before it.
  {
    const auto outline = corvcad::set_board_outline(update.board, {{0, 0}, {70, 0}, {70, 30}, {35, 45}, {0, 30}});
    const auto shaped = corvcad::parse_kicad_board(outline);
    require(shaped.outline.size() == 5 && shaped.max_y == 45 && outline.find("(gr_rect") == std::string::npos &&
                outline.find("(gr_poly (pts (xy 0 0) (xy 70 0)") != std::string::npos, "outline replaced by one polygon");
    bool crossed = false;
    try { corvcad::set_board_outline(update.board, {{0, 0}, {10, 10}, {10, 0}, {0, 10}}); } catch (...) { crossed = true; }
    require(crossed, "a self-crossing outline is refused");
    const auto zoned = corvcad::add_board_zone(outline, {{1, 1}, {60, 1}, {60, 28}, {1, 28}}, "B.Cu", "/NET1");
    const auto zone_board = corvcad::parse_kicad_board(zoned);
    require(zone_board.zones.size() == 1 && !zone_board.zones[0].filled && zone_board.zones[0].layer == "B.Cu" &&
                zone_board.zones[0].points.size() == 4 && !zone_board.zones[0].uuid.empty() &&
                zoned.find("(zone (net \"/NET1\") (layer \"B.Cu\")") != std::string::npos, "unfilled zone in KiCad 10 form");
    require(corvcad::delete_board_copper(zoned, zone_board.zones[0].uuid) == outline, "zone deleted cleanly");
    const auto lined = corvcad::add_board_line(zoned, 5, 40, 20, 40, "F.SilkS");
    const auto texted = corvcad::add_board_text(lined, "CorvCAD \"v1\"", 30, 38, "B.SilkS", 1.5);
    const auto drawn = corvcad::parse_kicad_board(texted);
    require(drawn.graphics.size() == 1 && drawn.graphics[0].width == 0.15 && drawn.graphics[0].layer == "F.SilkS" &&
                drawn.texts.size() == 1 && drawn.texts[0].text == "CorvCAD \"v1\"" && drawn.texts[0].size == 1.5 &&
                texted.find("(justify mirror)") != std::string::npos, "line and mirrored back text parse");
    require(corvcad::delete_board_copper(texted, drawn.texts[0].uuid) == lined &&
                corvcad::delete_board_copper(lined, drawn.graphics[0].uuid) == zoned, "line and text deleted cleanly");

    // Editing drawn items: only their coordinates, string and size change.
    const auto moved_line = corvcad::move_board_drawing(texted, drawn.graphics[0].uuid, 2.5, -1);
    const auto line_board = corvcad::parse_kicad_board(moved_line);
    std::string expected_line = texted;
    expected_line.replace(expected_line.find("(start 5 40) (end 20 40)"), 24, "(start 7.5 39) (end 22.5 39)");
    require(line_board.graphics[0].start.x == 7.5 && moved_line == expected_line, "line moved by its offset only");
    const auto moved_text = corvcad::move_board_drawing(texted, drawn.texts[0].uuid, -10, 0);
    require(corvcad::parse_kicad_board(moved_text).texts[0].position.x == 20 &&
                moved_text.find("(at 20 38 0)") != std::string::npos, "text moved, rotation kept");
    const auto renamed = corvcad::set_board_text(texted, drawn.texts[0].uuid, "Rev B", 2);
    const auto renamed_board = corvcad::parse_kicad_board(renamed);
    require(renamed_board.texts[0].text == "Rev B" && renamed_board.texts[0].size == 2 &&
                renamed.find("(thickness 0.3)") != std::string::npos && renamed.find("(justify mirror)") != std::string::npos,
            "text string and size set, mirror kept");
    refused = false;
    try { corvcad::set_board_text(texted, drawn.graphics[0].uuid, "x", 1); } catch (...) { refused = true; }
    require(refused, "only text takes a string");
    // A zone with a (stale) fill: moving it drops the fill and shifts the outline.
    std::string filled = zoned;
    // "... (xy 1 28))))": the last xy, pts, polygon, then the zone closes.
    const auto polygon_end = filled.find(")))", filled.find("(polygon (pts")) + 3;
    filled.insert(polygon_end, " (filled_polygon (layer \"B.Cu\") (pts (xy 2 2) (xy 3 2) (xy 3 3)))");
    require(corvcad::parse_kicad_board(filled).zones.size() == 2, "fixture zone has a fill");
    const auto moved_zone = corvcad::move_board_drawing(filled, zone_board.zones[0].uuid, 1, 1);
    const auto moved_zone_board = corvcad::parse_kicad_board(moved_zone);
    require(moved_zone_board.zones.size() == 1 && !moved_zone_board.zones[0].filled &&
                moved_zone_board.zones[0].points[0].x == 2 && moved_zone_board.zones[0].points[2].y == 29 &&
                moved_zone.find("filled_polygon") == std::string::npos, "zone moved, stale fill dropped");
    {
      // A dimension as KiCad 10 writes it: its extension lines and crossbar (offset -2 along
      // (-dy, dx), so above a left-to-right measurement) and text, under the dimension's UUID.
      std::string dimensioned = texted;
      dimensioned.insert(dimensioned.rfind(')'),
          "  (dimension (type aligned) (layer \"Cmts.User\") (uuid \"dim-1\") (pts (xy 10 50) (xy 30 50))"
          " (height -2) (gr_text \"20 mm\" (at 20 46.5 0) (layer \"Cmts.User\") (effects (font (size 1 1) (thickness 0.15))))"
          " (format (units 3) (units_format 1) (precision 4)) (style (thickness 0.12) (arrow_length 1.27)"
          " (text_position_mode 0) (extension_height 0.58642) (extension_offset 0.5) (keep_text_aligned yes))))\n");
      const auto dim_board = corvcad::parse_kicad_board(dimensioned);
      const size_t lines = dim_board.graphics.size() - drawn.graphics.size();
      const auto &crossbar = dim_board.graphics.back();
      require(lines == 3 && crossbar.uuid == "dim-1" && crossbar.start.y == 48 && crossbar.end.x == 30 &&
              crossbar.width == 0.12 && dim_board.texts.back().text == "20 mm", "a dimension reads as lines and text");
      const auto moved_dim = corvcad::parse_kicad_board(corvcad::move_board_drawing(dimensioned, "dim-1", 1, 2));
      require(moved_dim.graphics.back().start.x == 11 && moved_dim.texts.back().position.y == 48.5, "a dimension moves");
      // A zone's properties as KiCad writes them, and the defaults when they are absent.
      std::string zoned_settings = texted;
      zoned_settings.insert(zoned_settings.rfind(')'),
          "  (zone (net 0) (net_name \"\") (layer \"F.Cu\") (uuid \"zone-s\") (hatch edge 0.5) (priority 3)"
          " (connect_pads yes (clearance 0.4)) (min_thickness 0.3) (filled_areas_thickness no)"
          " (fill (mode hatch) (thermal_gap 0.6) (thermal_bridge_width 0.7))"
          " (polygon (pts (xy 1 1) (xy 9 1) (xy 9 9))))\n");
      const auto settings = corvcad::parse_kicad_board(zoned_settings).zones.back().settings;
      require(settings.priority == 3 && settings.connection == 0 && settings.clearance == 0.4 &&
              settings.min_thickness == 0.3 && settings.hatched && settings.thermal_gap == 0.6 &&
              settings.spoke_width == 0.7, "zone properties are read");
      const corvcad::ZoneSettings defaults;
      require(defaults.connection == 1 && !defaults.hatched && defaults.priority == 0 && !defaults.teardrop,
              "zone defaults are KiCad's");
      std::string teardropped = texted;
      teardropped.insert(teardropped.rfind(')'), "  (zone (net 0) (net_name \"\") (layer \"F.Cu\") (uuid \"td\") (attr (teardrop (type padvia)))"
                                                " (polygon (pts (xy 1 1) (xy 2 1) (xy 2 2))))\n");
      require(corvcad::parse_kicad_board(teardropped).zones.back().settings.teardrop, "teardrop zones are marked");
      const auto undimensioned = corvcad::delete_board_copper(dimensioned, "dim-1");
      require(undimensioned.find("(dimension") == std::string::npos &&
              corvcad::parse_kicad_board(undimensioned).graphics.size() == drawn.graphics.size(), "a dimension deletes");
    }
    char edit_error[256]{};
    char *c_moved = cc_move_board_drawing(texted.c_str(), "no-such-uuid", 1, 1, edit_error, sizeof(edit_error));
    require(!c_moved && std::string(edit_error) == "Item not found", "C API move reports a missing item");
    refused = false;
    try { corvcad::add_board_line(zoned, 1, 1, 5, 1, "F.Cu"); } catch (...) { refused = true; }
    require(refused, "drawings stay off copper");
    const double square[] = {0, 0, 50, 0, 50, 50, 0, 50};
    char draw_error[256]{};
    char *c_outline = cc_set_board_outline(update.board.c_str(), square, 4, draw_error, sizeof(draw_error));
    require(c_outline && corvcad::parse_kicad_board(c_outline).max_x == 50, "C API outline");
    cc_free_text(c_outline);
  }

  char report[256]{}, error[256]{};
  char *text = cc_update_board_from_schematic(without_r2.c_str(), schematic.c_str(), report, sizeof(report),
                                              error, sizeof(error));
  require(text && std::string(report) == "Added R2.", "C API update and report");
  cc_free_text(text);
}

int main() {
  {
    // Device:LED's light arrows extend past its cathode tip. Test all rotations:
    // the tip must escape outwards, while crossing the symbol or that pin still fails.
    for (int turn = 0; turn < 4; ++turn) {
      auto rotate = [turn](corvcad::Point p) {
        for (int i = 0; i < turn; ++i) p = {-p.y, p.x};
        return p;
      };
      auto a = rotate({-4.6, -2.3}), b = rotate({3.81, 1.4});
      const auto tip = rotate({-3.81, 0}), out = rotate({-1, 0}), end = rotate({-10, 0});
      corvcad::RouteObstacles obstacle;
      obstacle.points.push_back(tip);
      obstacle.boxes.push_back({std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)});
      corvcad::open_pin_escape(obstacle, tip, out);
      require(corvcad::plan_orthogonal_route(obstacle, tip.x, tip.y, end.x, end.y).size() == 2,
              "LED cathode can leave its drawing bounds along its pin axis");
      bool refused = false;
      const auto inside = rotate({0, 0});
      try { corvcad::plan_orthogonal_route(obstacle, tip.x, tip.y, inside.x, inside.y); }
      catch (const std::runtime_error &) { refused = true; }
      require(refused, "pin escape does not open a route into the symbol body");
      const auto before = rotate({-4, 0}), after = rotate({-3.805, 0});
      refused = false;
      try { corvcad::plan_orthogonal_route(obstacle, before.x, before.y, after.x, after.y); }
      catch (const std::runtime_error &) { refused = true; }
      require(refused, "pin remains an obstacle for unrelated wires");
    }
  }
  test_update_board();
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path parent = fs::temp_directory_path() / ("corvcad-authoring-test-" + std::to_string(nonce));
  require(fs::create_directory(parent), "unique temporary directory");
  try {
    corvcad::create_project(parent.string(), "TouchTest", 80, 60);
    const fs::path folder = parent / "TouchTest";
    require(fs::exists(folder / "sym-lib-table") && fs::exists(folder / "fp-lib-table") &&
            fs::exists(folder / "CorvCAD.kicad_sym") &&
            fs::exists(folder / "CorvCAD.pretty" / "TwoPad.kicad_mod") &&
            fs::exists(folder / "CorvCAD.pretty" / "MountingHole3.2.kicad_mod"),
            "portable part libraries are in the project");
    const fs::path schematic_path = folder / "TouchTest.kicad_sch";
    const fs::path board_path = folder / "TouchTest.kicad_pcb";
    {
      // fixtures/drawn/Drawn.kicad_pcb is this drawing sequence on a starter board with fixed
      // UUIDs; native-check compares KiCad's DRC, zone fill and Gerbers of it with kicad-cli's.
      auto drawn = corvcad::set_board_outline(read(board_path), {{0, 0}, {70, 0}, {70, 30}, {35, 45}, {0, 30}});
      drawn = corvcad::add_board_via(drawn, 30, 20, "GND");
      drawn = corvcad::add_board_zone(drawn, {{1, 1}, {69, 1}, {69, 29}, {1, 29}}, "B.Cu", "GND");
      drawn = corvcad::add_board_zone(drawn, {{5, 5}, {25, 5}, {25, 15}, {5, 15}}, "F.Cu", "");
      drawn = corvcad::add_board_line(drawn, 25, 32, 45, 32, "F.SilkS");
      drawn = corvcad::add_board_text(drawn, "CorvCAD", 35, 38, "B.SilkS", 1.5);
      drawn = corvcad::add_board_text(drawn, "TOP", 55, 10, "F.Fab");
      const std::regex id("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
      const fs::path fixture = fs::path(CORVCAD_ROUNDTRIP_DIR).parent_path() / "drawn" / "Drawn.kicad_pcb";
      require(std::regex_replace(drawn, id, "U") == std::regex_replace(read(fixture), id, "U"),
              "drawing calls write the native-check fixture");
    }
    const auto initial_schematic = read(schematic_path);
    auto schematic = corvcad::add_schematic_two_pin(initial_schematic, "TouchTest", 76.2, 76.2);
    schematic = corvcad::add_schematic_wire(schematic, 71.12, 76.2, 60.96, 76.2);
    schematic = corvcad::add_schematic_label(schematic, "IN", 60.96, 76.2);
    const auto parsed_schematic = corvcad::parse_kicad_schematic(schematic);
    require(parsed_schematic.symbols.size() == 1 && parsed_schematic.symbols[0].reference == "R1",
            "tap adds a two-pin symbol");
    require(parsed_schematic.wires.size() == 1 && parsed_schematic.labels.size() == 1,
            "tap adds a wire and label");
    require(schematic.find("(symbol \"CorvCAD:TwoPin\"") != std::string::npos,
            "symbol definition is embedded");
    require(schematic.find("(embedded_fonts no)") != std::string::npos,
            "existing schematic fields survive");
    bool overlap = false;
    try { corvcad::add_schematic_two_pin(schematic, "TouchTest", 76.2, 76.2); }
    catch (...) { overlap = true; }
    require(overlap, "touch placement rejects overlapping symbols");
    const auto route = corvcad::add_schematic_orthogonal_wire(schematic, 81.28, 76.2, 91.44, 86.36);
    const auto routed = corvcad::parse_kicad_schematic(route);
    require(routed.wires.size() == 3 && routed.wires[1].end.y == 76.2 &&
            routed.wires[2].end.y == 86.36, "diagonal touch route becomes two right-angle segments");
    {
      // Notes as KiCad 10 writes them: a text and a three-point polyline; neither is a wire or label.
      const auto noted = corvcad::parse_kicad_schematic(
          "(kicad_sch (version 20250114) (generator \"eeschema\") (uuid \"a\") (paper \"A4\")\n"
          "  (text \"Check R1\" (exclude_from_sim no) (at 50.8 40.64 0) (effects (font (size 1.27 1.27)))"
          " (uuid \"t1\"))\n"
          "  (polyline (pts (xy 10 10) (xy 20 10) (xy 20 15)) (stroke (width 0) (type default))"
          " (uuid \"p1\")))\n");
      require(noted.texts.size() == 1 && noted.texts[0].text == "Check R1" && noted.texts[0].position.x == 50.8 &&
              noted.texts[0].uuid == "t1", "sheet text notes are read");
      require(noted.note_lines.size() == 2 && noted.note_lines[1].end.y == 15 && noted.note_lines[1].uuid == "p1" &&
              noted.wires.empty() && noted.labels.empty(), "a polyline becomes note segments sharing its UUID");
    }
    const std::string symbol_uuid = parsed_schematic.symbols[0].uuid;
    const std::string label_uuid = parsed_schematic.labels[0].uuid;
    const std::string wire_uuid = parsed_schematic.wires[0].uuid;
    const auto renamed_symbol = corvcad::rename_schematic_symbol(schematic, symbol_uuid, "R9");
    require(corvcad::parse_kicad_schematic(renamed_symbol).symbols[0].reference == "R9" &&
            renamed_symbol.find("(reference \"R9\")") != std::string::npos,
            "symbol and project instance reference rename together");
    const auto renamed_label = corvcad::rename_schematic_label(schematic, label_uuid, "OUTPUT");
    require(corvcad::parse_kicad_schematic(renamed_label).labels[0].text == "OUTPUT",
            "label rename preserves schematic");
    const auto moved_label = corvcad::move_schematic_label(schematic, label_uuid, 63.5, 78.74);
    require(corvcad::parse_kicad_schematic(moved_label).labels[0].position.x == 63.5,
            "label can be moved by UUID");
    const auto without_wire = corvcad::delete_schematic_item(schematic, wire_uuid);
    require(corvcad::parse_kicad_schematic(without_wire).wires.empty() &&
            without_wire.find("(paper \"A4\")") != std::string::npos,
            "wire deletion preserves unrelated text");
    require(corvcad::parse_kicad_schematic(
            corvcad::delete_schematic_item(schematic, symbol_uuid)).symbols.empty(),
            "symbol deletion removes only selected symbol");
    // Rotate turns R1 counter-clockwise: angle 90, pins now vertical, the Reference field
    // (5.08 above the origin) now 5.08 to its left.
    const auto turned = corvcad::rotate_schematic_symbol(schematic, symbol_uuid);
    const auto turned_symbol = corvcad::parse_kicad_schematic(turned).symbols[0];
    require(turned_symbol.rotation == 90 && turned_symbol.pins.size() == 2 &&
            turned_symbol.pins[0].x == 76.2 && turned_symbol.pins[1].x == 76.2 &&
            std::abs(turned_symbol.pins[0].y - turned_symbol.pins[1].y) > 10,
            "rotated two-pin part has vertical pins");
    require(turned.find("(property \"Reference\" \"R1\" (at 71.12 76.2 90)") != std::string::npos,
            "rotated field turns about the symbol origin");
    require(corvcad::parse_kicad_schematic(corvcad::rotate_schematic_symbol(
                corvcad::rotate_schematic_symbol(corvcad::rotate_schematic_symbol(turned, symbol_uuid),
                                                 symbol_uuid), symbol_uuid)).symbols[0].rotation == 0,
            "four turns return to angle 0");
    const auto second = corvcad::add_schematic_two_pin(schematic, "TouchTest", 100, 76.2);
    require(corvcad::parse_kicad_schematic(second).symbols[1].reference == "R2",
            "next symbol gets a distinct reference");
    // R1 spans pins 71.12..81.28 on y 76.2; a straight wire from its far pin to R2
    // would cross R1's body and near pin, which KiCad would join to the wire.
    const auto detour = corvcad::route_schematic_wire(second, 71.12, 76.2, 94.92, 76.2);
    require(detour.size() == 4 && std::abs(detour[1].y - 76.2) > 2 &&
            detour[1].x == 71.12 && detour[2].x == 94.92,
            "route detours around a part instead of crossing its pins");
    const auto detoured = corvcad::parse_kicad_schematic(
        corvcad::add_schematic_orthogonal_wire(second, 71.12, 76.2, 94.92, 76.2));
    require(detoured.wires.size() == 4, "detour is written as three wire segments");
    double xy[16]{};
    char error_buffer[256]{};
    require(cc_route_schematic_wire(second.c_str(), 71.12, 76.2, 94.92, 76.2, xy, 8,
                                    error_buffer, sizeof(error_buffer)) == 4 && xy[6] == 94.92,
            "C ABI reports the planned route");

    // Imported parts: pins and body come from the sheet's lib_symbols, for the placed unit,
    // with KiCad's placement transform (Y flipped, then rotation, then mirror).
    const std::string imported =
        "(kicad_sch (version 20250114) (generator \"test\") (uuid \"s\") (paper \"A4\")\n"
        " (lib_symbols (symbol \"Test:V\"\n"
        "  (symbol \"V_0_1\" (rectangle (start -1.27 -2.54) (end 1.27 2.54)))\n"
        "  (symbol \"V_1_1\" (pin passive line (at 0 5.08 270) (length 2.54))\n"
        "                    (pin passive line (at 0 -5.08 90) (length 2.54)))\n"
        "  (symbol \"V_2_1\" (pin passive line (at 10 10 0) (length 2.54)))))\n"
        " (symbol (lib_id \"Test:V\") (at 76.2 101.6 0) (unit 1) (uuid \"u1\"))\n"
        " (symbol (lib_id \"Test:V\") (at 50.8 50.8 0) (mirror y) (unit 2) (uuid \"u2\"))\n"
        " (no_connect (at 86.36 86.36) (uuid \"n1\"))\n"
        " (global_label \"VBUS\" (shape input) (at 30.48 30.48 0) (uuid \"g1\")))\n";
    const auto parts = corvcad::parse_kicad_schematic(imported);
    require(parts.symbols[0].pins.size() == 2 && parts.symbols[0].pins[0].x == 76.2 &&
            parts.symbols[0].pins[0].y == 96.52 && parts.symbols[0].pins[1].y == 106.68,
            "imported pins are placed with Y flipped");
    require(parts.symbols[1].pins.size() == 1 && parts.symbols[1].pins[0].x == 40.8 &&
            parts.symbols[1].pins[0].y == 40.8, "only the placed unit's pins, mirrored");
    require(parts.symbols[0].graphics.size() == 1 && parts.symbols[0].graphics[0].closed &&
            parts.symbols[0].graphics[0].points.size() == 4 && parts.symbols[0].graphics[0].points[0].x == 74.93 &&
            parts.symbols[0].graphics[0].points[0].y == 104.14 && parts.symbols[1].graphics.size() == 1,
            "the shared body rectangle is drawn for each unit, placed");
    require(parts.symbols[0].pin_ends.size() == 2 && parts.symbols[0].pin_ends[0].x == 76.2 &&
            parts.symbols[0].pin_ends[0].y == 99.06 && parts.symbols[0].pin_visible[0],
            "pin lines end at the body");
    std::string any_style = imported;
    any_style.replace(any_style.find("(unit 1)"), 8, "(unit 1) (body_style 0)");
    require(corvcad::parse_kicad_schematic(any_style).symbols[0].pins.size() == 2,
            "body style 0 matches every style, as KiCad reads it");
    // Field anchors from the sheet; pin name and number settings from the definition.
    std::string fields = imported;
    fields.replace(fields.find("(unit 1) (uuid \"u1\")"), 20,
                   "(unit 1) (uuid \"u1\") (property \"Reference\" \"R1\" (at 78.74 100.33 90))"
                   " (property \"Value\" \"10k\" (at 78.74 102.87 0) (effects (font (size 1.27 1.27)) hide))");
    fields.replace(fields.find("(symbol \"V_0_1\""), 0, "(pin_names (offset 0) (hide yes)) (pin_numbers hide) ");
    const auto texts = corvcad::parse_kicad_schematic(fields).symbols[0];
    require(texts.reference_field.visible && texts.reference_field.vertical &&
            texts.reference_field.position.x == 78.74 && !texts.value_field.visible && !texts.value_field.vertical &&
            texts.pin_name_offset == 0 && !texts.pin_names_visible && !texts.pin_numbers_visible &&
            parts.symbols[0].pin_names_visible && parts.symbols[0].pin_name_offset == 0.508 &&
            !parts.symbols[0].reference_field.visible, "symbol field anchors and pin text settings");
    require(corvcad::route_schematic_wire(imported, 66.04, 101.6, 86.36, 101.6).size() == 4,
            "route detours around an imported part's body");
    require(corvcad::route_schematic_wire(imported, 76.2, 96.52, 76.2, 86.36).size() == 2,
            "route may start on an imported part's pin end");
    require(corvcad::route_schematic_wire(imported, 81.28, 86.36, 91.44, 86.36).size() == 4,
            "route does not pass over a no-connect flag");
    cc_schematic *handle = cc_schematic_parse(imported.c_str(), error_buffer, sizeof(error_buffer));
    require(handle && cc_sch_no_connect_count(handle) == 1 && cc_sch_no_connect_at(handle, 0).x == 86.36 &&
            std::string(cc_sch_label_at(handle, 0).kind) == "global_label",
            "C ABI reports no-connect flags and label kinds");
    double gx = 0, gy = 0;
    cc_sch_symbol_graphic_point(handle, 0, 0, 2, &gx, &gy);
    require(cc_sch_symbol_graphic_count(handle, 0) == 1 && cc_sch_symbol_graphic_at(handle, 0, 0).point_count == 4 &&
            cc_sch_symbol_graphic_at(handle, 0, 0).closed == 1 && gx == 77.47 && gy == 99.06 &&
            cc_sch_symbol_pin_count(handle, 0) == 2 && cc_sch_symbol_pin_at(handle, 0, 1).end_y == 104.14,
            "C ABI reports symbol shapes and pin lines");
    require(cc_sch_page_info(handle).width == 297 && cc_sch_page_info(handle).height == 210,
            "C ABI reports the page size");
    {
      std::string page = imported;
      page.replace(page.find("(paper \"A4\")"), 12,
                   "(paper \"A3\" portrait) (title_block (title \"Amp\") (rev \"B\") (company \"Corvusar\"))");
      const auto parsed = corvcad::parse_kicad_schematic(page);
      require(parsed.paper.x == 297 && parsed.paper.y == 420 && parsed.title == "Amp" && parsed.revision == "B" &&
              parsed.company == "Corvusar" && corvcad::parse_kicad_schematic(
                  std::string(imported).replace(imported.find("(paper \"A4\")"), 12, "(paper \"User\" 100 80)")).paper.x == 100,
              "paper sizes, portrait, user size and title block");
    }
    require(cc_sch_symbol_text_at(handle, 0).pin_names_visible == 1 && cc_sch_symbol_text_at(handle, 0).reference_visible == 0,
            "C ABI reports symbol text");
    cc_schematic_free(handle);

    // KiCad needs an explicit junction where a wire end lands on another wire's interior,
    // and none where a wire just continues another or ends on a pin.
    auto tee = corvcad::add_schematic_wire(initial_schematic, 50.8, 50.8, 71.12, 50.8);
    tee = corvcad::add_schematic_wire(tee, 71.12, 50.8, 81.28, 50.8);
    require(corvcad::parse_kicad_schematic(tee).junctions.empty(), "a continued wire needs no junction");
    tee = corvcad::add_schematic_wire(tee, 60.96, 50.8, 60.96, 60.96);
    const auto teed = corvcad::parse_kicad_schematic(tee);
    require(teed.junctions.size() == 1 && teed.junctions[0].position.x == 60.96 &&
            teed.junctions[0].position.y == 50.8, "a wire ending on another's interior writes a junction");
    tee = corvcad::add_schematic_wire(tee, 71.12, 50.8, 71.12, 40.64);
    require(corvcad::parse_kicad_schematic(tee).junctions.size() == 2, "three wire ends meeting write a junction");
    require(corvcad::parse_kicad_schematic(corvcad::add_schematic_wire(tee, 60.96, 50.8, 60.96, 40.64))
                .junctions.size() == 2, "an existing junction is not duplicated");

    const auto initial_board = read(board_path);
    auto board = corvcad::add_board_two_pad(initial_board, "R1", "10k", 30, 30);
    board = corvcad::add_board_mounting_hole(board, 10, 10);
    const auto parsed_board = corvcad::parse_kicad_board(board);
    require(parsed_board.placements.size() == 2 && parsed_board.pads.size() == 3,
            "board parts and pads load");
    require(parsed_board.mounting_holes.size() == 1 &&
            parsed_board.mounting_holes[0].diameter == 3.2,
            "mounting hole joins enclosure model");
    require(board.find("(attr board_only exclude_from_pos_files exclude_from_bom)") != std::string::npos,
            "mounting hole is board-only, so KiCad parity does not expect a symbol");
    require(board.substr(0, initial_board.size() - 2) ==
            initial_board.substr(0, initial_board.size() - 2),
            "existing board source is unchanged");
    bool duplicate = false;
    try { corvcad::add_board_two_pad(board, "R1", "10k", 40, 30); }
    catch (...) { duplicate = true; }
    require(duplicate, "duplicate reference rejected");
    const auto renamed_board = corvcad::rename_footprint(board, "R1", "R9");
    require(corvcad::parse_kicad_board(renamed_board).placements[0].reference == "R9" &&
            renamed_board.find("(gr_rect") != std::string::npos,
            "board rename keeps the outline");
    const auto turned_board = corvcad::parse_kicad_board(corvcad::rotate_footprint(board, "R1"));
    require(turned_board.placements[0].rotation == 90 && turned_board.pads[0].rotation == 90 &&
            std::abs(turned_board.pads[0].center.x - 30) < 1e-6 &&
            std::abs(std::abs(turned_board.pads[0].center.y - 30) - 0.8) < 1e-6,
            "footprint rotation turns its pads");
    char *rotated_c = cc_rotate_board_footprint(board.c_str(), "R1", error_buffer, sizeof(error_buffer));
    require(rotated_c != nullptr, "C ABI rotates a footprint");
    cc_free_text(rotated_c);
    const auto without_hole = corvcad::delete_footprint(board, "H1");
    require(corvcad::parse_kicad_board(without_hole).mounting_holes.empty() &&
            corvcad::parse_kicad_board(without_hole).placements.size() == 1,
            "hole deletion keeps other footprints");
    char error[256]{};
    char *from_c = cc_add_board_mounting_hole(board.c_str(), 65, 10, error, sizeof(error));
    require(from_c != nullptr, "C ABI adds a board part");
    cc_free_text(from_c);
    from_c = cc_rename_board_footprint(board.c_str(), "R1", "R7", error, sizeof(error));
    require(from_c != nullptr && corvcad::parse_kicad_board(from_c).placements[0].reference == "R7",
            "C ABI renames a board footprint");
    cc_free_text(from_c);
    from_c = cc_delete_schematic_item(schematic.c_str(), wire_uuid.c_str(), error, sizeof(error));
    require(from_c != nullptr && corvcad::parse_kicad_schematic(from_c).wires.empty(),
            "C ABI deletes a schematic item");
    cc_free_text(from_c);
    require(cc_add_board_mounting_hole(board.c_str(), 0, 0, error, sizeof(error)) == nullptr,
            "C ABI rejects placement at edge");
    require(!std::string(error).empty(), "C ABI reports placement error");
    std::ofstream(schematic_path, std::ios::binary) << schematic;
    std::ofstream(board_path, std::ios::binary) << board;
    if (const char *output = std::getenv("CORVCAD_TEST_AUTHORED_DIR")) {
      const fs::path target = corvcad::utf8_path(output);
      fs::create_directories(target);
      fs::copy(folder, target / "TouchTest", fs::copy_options::recursive |
               fs::copy_options::overwrite_existing);
    }
  } catch (...) {
    fs::remove_all(parent);
    throw;
  }
  fs::remove_all(parent);
}

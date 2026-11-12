#include "corvcad/eda.hpp"
#include "corvcad/c_api.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }

int main() {
  const std::string source = R"SCH((kicad_sch
    (version 20250114) (uuid "sheet-id")
    (lib_symbols (symbol "Device:R" (property "Reference" "R" (at 0 0 0))))
    (symbol (lib_id "Device:R") (at 30 40 90) (uuid "symbol-id")
      (property "Reference" "R1" (at 31 41 0))
      (property "Value" "1k" (at 32 42 0))
      (property "Footprint" "Resistor_SMD:R_0805" (at 30 40 0)))
    (wire (pts (xy 10 20) (xy 30 40)) (uuid "wire-id"))
    (bus (pts (xy 0 0) (xy 10 0)) (uuid "bus-id"))
    (bus_entry (at 5 0) (size 2.54 2.54) (stroke (width 0) (type default)) (uuid "entry-id"))
    (junction (at 10 20) (uuid "junction-id"))
    (label "OUT" (at 30 40 0) (uuid "label-id"))
    (global_label "VCC" (at 0 0 0) (uuid "global-id"))
    (hierarchical_label "DATA" (at 10 0 0) (uuid "hier-id"))
    (no_connect (at 12 12) (uuid "nc-id"))
    (sheet (at 40 40) (size 20 10) (uuid "child-id")
      (property "Sheetname" "Power" (at 40 40 0))
      (property "Sheetfile" "Power.kicad_sch" (at 40 42 0))
      (pin "EN" input (at 40 45 180) (uuid "pin-id") (effects (font (size 1.27 1.27)))))
    (unknown_future_field "preserve me")))SCH";
  const auto schematic = corvcad::parse_kicad_schematic(source);
  require(schematic.uuid == "sheet-id", "sheet ID");
  require(schematic.symbols.size() == 1 && schematic.symbols[0].reference == "R1", "placed symbol");
  require(schematic.symbols[0].position.x == 30 && schematic.symbols[0].rotation == 90,
          "symbol placement");
  require(schematic.wires.size() == 1 && schematic.wires[0].end.y == 40,
          "wire placement");
  require(schematic.junctions.size() == 1 && schematic.labels.size() == 3 &&
          schematic.labels[1].kind == "global_label", "junction and label kinds");
  require(schematic.buses.size() == 1 && schematic.no_connects.size() == 1,
          "bus and no-connect");
  require(schematic.sheets.size() == 1 && schematic.sheets[0].file == "Power.kicad_sch",
          "child sheet reference");
  require(schematic.bus_entries.size() == 1 && schematic.bus_entries[0].end.x > 7.539 && schematic.bus_entries[0].end.x < 7.541 &&
          schematic.bus_entries[0].end.y == 2.54, "bus entry from its position by its size");
  require(schematic.sheets[0].pins.size() == 1 && schematic.sheets[0].pins[0].name == "EN" &&
          schematic.sheets[0].pins[0].shape == "input" && schematic.sheets[0].pins[0].position.y == 45, "sheet pin");

  const auto moved = corvcad::move_schematic_symbol(source, "symbol-id", 35, 47);
  require(moved.find("(at 35 47 90)") != std::string::npos, "symbol moved");
  require(moved.find("(at 36 48 0)") != std::string::npos, "reference anchor moved");
  require(moved.find("(at 37 49 0)") != std::string::npos, "value anchor moved");
  require(moved.find("(wire (pts (xy 10 20) (xy 30 40))") != std::string::npos,
          "wires unchanged");
  require(moved.find("(unknown_future_field \"preserve me\")") != std::string::npos,
          "unknown fields unchanged");
  require(corvcad::parse_kicad_schematic(moved).symbols[0].position.y == 47,
          "moved output reparses");
  auto expected = source;
  const auto original = std::string("(at 30 40 90)");
  expected.replace(expected.find(original), original.size(), "(at 35 47 90)");
  expected.replace(expected.find("(at 31 41 0)"), std::string("(at 31 41 0)").size(), "(at 36 48 0)");
  expected.replace(expected.find("(at 32 42 0)"), std::string("(at 32 42 0)").size(), "(at 37 49 0)");
  expected.replace(expected.find("(at 30 40 0)"), std::string("(at 30 40 0)").size(), "(at 35 47 0)");
  require(moved == expected, "only intended coordinates changed");

  const auto valued = corvcad::set_schematic_property(source, "symbol-id", "Value", "2.2k");
  require(valued.find("(property \"Value\" \"2.2k\"") != std::string::npos,
          "value changed");
  require(valued.find("(property \"Reference\" \"R1\"") != std::string::npos,
          "reference unchanged");
  const auto escaped = corvcad::set_schematic_property(source, "symbol-id", "Value", "a\"b\\c");
  require(corvcad::parse_kicad_schematic(escaped).symbols[0].value == "a\"b\\c",
          "quoted property round trip");

  bool rejected = false;
  try { corvcad::move_schematic_symbol(source, "missing", 1, 1); }
  catch (...) { rejected = true; }
  require(rejected, "missing UUID rejected");
  rejected = false;
  try { corvcad::set_schematic_property(source, "symbol-id", "Reference", "R2"); }
  catch (...) { rejected = true; }
  require(rejected, "reference edit outside supported scope");
  rejected = false;
  try { corvcad::parse_kicad_schematic(source + "extra"); }
  catch (...) { rejected = true; }
  require(rejected, "trailing input rejected");

  char error[256]{};
  auto *handle = cc_schematic_parse(source.c_str(), error, sizeof(error));
  require(handle && cc_sch_symbol_count(handle) == 1 && cc_sch_wire_count(handle) == 1,
          "C API parse");
  require(cc_sch_junction_count(handle) == 1 && cc_sch_label_count(handle) == 3,
          "C API other items");
  require(cc_sch_symbol_at(handle, 0).x == 30, "C API symbol");
  auto output = std::filesystem::temp_directory_path() / "corvcad-test-edited.kicad_sch";
  require(cc_move_schematic_symbol_file(source.c_str(), "symbol-id", 35, 47,
                                        output.string().c_str(), error, sizeof(error)) == 1,
          "C API move");
  std::ifstream edited_file(output, std::ios::binary);
  const std::string edited((std::istreambuf_iterator<char>(edited_file)),
                           std::istreambuf_iterator<char>());
  require(edited == moved, "C API output");
  edited_file.close();
  std::filesystem::remove(output);
  require(cc_set_schematic_property_file(source.c_str(), "symbol-id", "Footprint",
                                         "Resistor_SMD:R_0603", output.string().c_str(),
                                         error, sizeof(error)) == 1,
          "C API footprint property edit");
  std::ifstream property_file(output, std::ios::binary);
  const std::string property_source((std::istreambuf_iterator<char>(property_file)),
                                    std::istreambuf_iterator<char>());
  require(corvcad::parse_kicad_schematic(property_source).symbols[0].footprint ==
              "Resistor_SMD:R_0603", "C API footprint output reparses");
  property_file.close();
  std::filesystem::remove(output);
  cc_schematic_free(handle);

  // Connectivity of the touch-authored round-trip sheet; the expected nets and names are
  // kicad-cli 10.0.6 netlists of the same text (see docs/KICAD_TEST_PROJECT.md).
  std::ifstream round_trip_file(CORVCAD_ROUNDTRIP, std::ios::binary);
  const std::string round_trip((std::istreambuf_iterator<char>(round_trip_file)),
                               std::istreambuf_iterator<char>());
  const auto nets_of = [](const std::string &text) {
    const auto sheet = corvcad::parse_kicad_schematic(text);
    std::set<std::string> result;
    for (const auto &net : corvcad::schematic_nets(sheet)) {
      std::set<std::string> pins;
      for (const auto &pin : net.pins) pins.insert(sheet.symbols[pin.symbol].reference + "." + pin.number);
      std::string line = net.name;
      for (const auto &pin : pins) line += " " + pin;
      result.insert(line);
    }
    return result;
  };
  const auto replaced = [&](std::string from, std::string to) {
    auto text = round_trip;
    text.replace(text.find(from), from.size(), to);
    return text;
  };
  require(nets_of(round_trip) == std::set<std::string>{"/NET1 R1.1 R2.2 R3.2", "Net-(R1-Pad2) R1.2 R2.1 R3.1"},
          "round-trip nets: detour crossing stays apart, tee joins through its junction");
  const auto junction = round_trip.find("(junction ");
  require(junction != std::string::npos, "fixture junction");
  auto without_junction = round_trip;
  without_junction.erase(junction, round_trip.find('\n', junction) - junction);
  require(nets_of(without_junction) == std::set<std::string>{
              "/NET1 R1.1 R2.2 R3.2", "Net-(R1-Pad2) R1.2 R2.1", "unconnected-(R3-Pad1) R3.1"},
          "a wire ending inside another wire needs the junction");
  require(nets_of(replaced("(label \"NET1\" (at 45.72 50.8 0)", "(label \"NET1\" (at 66.04 50.8 0)")) ==
              std::set<std::string>{"/NET1 R1.2 R2.1 R3.1 R3.2", "Net-(R1-Pad1) R1.1 R2.2"},
          "a label joins a wire along its length");
  require(nets_of(replaced("(at 76.2 25.4 90)", "(at 86.36 45.72 90)")) == std::set<std::string>{
              "/NET1 R1.1 R2.2", "Net-(R1-Pad2) R1.2 R2.1", "unconnected-(R3-Pad1) R3.1",
              "unconnected-(R3-Pad2) R3.2"},
          "a pin on a wire's interior does not connect");
  require(nets_of(replaced("(label \"NET1\" (at 45.72 50.8 0)",
                           "(global_label \"NET1\" (shape input) (at 45.72 50.8 0)")).count("NET1 R1.1 R2.2 R3.2"),
          "a global label takes over the local one of the same name");
  auto *round_trip_handle = cc_schematic_parse(round_trip.c_str(), error, sizeof(error));
  require(round_trip_handle && cc_sch_net_count(round_trip_handle) == 2, "C API nets");
  const long detour = cc_sch_wire_net(round_trip_handle, 0);
  require(detour >= 0 && detour == cc_sch_label_net(round_trip_handle, 1) &&
              std::string(cc_sch_net_at(round_trip_handle, static_cast<size_t>(detour)).name) == "/NET1" &&
              cc_sch_net_at(round_trip_handle, static_cast<size_t>(detour)).pin_count == 3,
          "C API wire and label share their net");
  require(std::string(cc_sch_net_pin_at(round_trip_handle, static_cast<size_t>(detour), 0).number).size() == 1,
          "C API net pin");
  // Live route previews use the parsed handle; it must plan exactly what the text route does.
  for (const auto &ends : std::vector<std::array<double, 4>>{{20.32, 20.32, 40.64, 30.48}, {127, 25.4, 139.7, 63.5}}) {
    double from_text[32]{}, from_handle[32]{};
    const size_t text_count = cc_route_schematic_wire(round_trip.c_str(), ends[0], ends[1], ends[2], ends[3],
                                                      from_text, 16, error, sizeof(error));
    const size_t handle_count = cc_sch_route(round_trip_handle, ends[0], ends[1], ends[2], ends[3],
                                             from_handle, 16, error, sizeof(error));
    require(text_count > 1 && text_count == handle_count &&
                std::equal(from_text, from_text + 2 * text_count, from_handle), "handle route matches text route");
  }
  cc_schematic_free(round_trip_handle);
}

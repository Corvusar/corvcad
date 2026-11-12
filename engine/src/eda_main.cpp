#include "corvcad/eda.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
std::string read_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open schematic input");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write_file(const std::string &path, const std::string &source) {
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("Cannot open schematic output");
  output << source;
  if (!output) throw std::runtime_error("Cannot write schematic output");
}
}

int main(int argc, char **argv) {
  try {
    if (argc < 3) throw std::runtime_error("Usage: corvcad_eda_cli inspect input.kicad_sch | nets input.kicad_sch | move-symbol input.kicad_sch uuid x y output.kicad_sch | set-property input.kicad_sch uuid Value|Footprint value output.kicad_sch");
    const std::string command = argv[1];
    const std::string source = read_file(argv[2]);
    if (command == "inspect" && argc == 3) {
      const auto schematic = corvcad::parse_kicad_schematic(source);
      std::cout << "sheet " << schematic.uuid << ": " << schematic.symbols.size()
                << " symbols, " << schematic.wires.size() << " wires, "
                << schematic.junctions.size() << " junctions, "
                << schematic.labels.size() << " labels\n";
      for (const auto &symbol : schematic.symbols)
        std::cout << symbol.reference << '\t' << symbol.value << '\t'
                  << symbol.library_id << '\t' << symbol.footprint << '\t'
                  << symbol.position.x << ',' << symbol.position.y << '\t'
                  << symbol.uuid << '\n';
    } else if (command == "nets" && argc == 3) {
      // One line per net with component pins: name, tab, space-separated REF.PIN.
      const auto schematic = corvcad::parse_kicad_schematic(source);
      for (const auto &net : corvcad::schematic_nets(schematic)) {
        std::string pins;
        for (const auto &pin : net.pins) {
          const auto &reference = schematic.symbols[pin.symbol].reference;
          if (reference.empty() || reference[0] == '#') continue;
          pins += (pins.empty() ? "" : " ") + reference + "." + pin.number;
        }
        if (!pins.empty()) std::cout << net.name << '\t' << pins << '\n';
      }
    } else if (command == "move-symbol" && argc == 7) {
      write_file(argv[6], corvcad::move_schematic_symbol(
          source, argv[3], std::stod(argv[4]), std::stod(argv[5])));
    } else if (command == "set-property" && argc == 7) {
      write_file(argv[6], corvcad::set_schematic_property(source, argv[3], argv[4], argv[5]));
    } else {
      throw std::runtime_error("Invalid schematic command or argument count");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

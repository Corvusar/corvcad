#include "corvcad/project.hpp"
#include "corvcad/engine.hpp"
#include "corvcad/eda.hpp"
#include "corvcad/c_api.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

std::string read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "created project file exists");
  return {std::istreambuf_iterator<char>(file), {}};
}

int main() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path parent = fs::temp_directory_path() / ("corvcad-project-test-" + std::to_string(nonce));
  require(fs::create_directory(parent), "unique temporary project directory");
  try {
    char error[256]{};
    require(cc_create_project(parent.string().c_str(), "Starter", 80, 60,
                              error, sizeof(error)) == 1, "C API project creation");
    const fs::path base = parent / "Starter" / "Starter";
    require(read(base.string() + ".kicad_pro") == "{}\n", "project config");
    const auto schematic = corvcad::parse_kicad_schematic(read(base.string() + ".kicad_sch"));
    require(!schematic.uuid.empty() && schematic.symbols.empty(), "blank schematic loads");
    const auto board_source = read(base.string() + ".kicad_pcb");
    const auto board = corvcad::parse_kicad_board(board_source);
    require(board.outline.size() == 4 && board.max_x - board.min_x == 80 &&
            board.max_y - board.min_y == 60, "starter board outline loads");
    require(!corvcad::make_model(board, {}).enclosure.empty(), "starter enclosure builds");
    require(board_source.find("(1 \"F.Mask\" user)") != std::string::npos &&
            board_source.find("(5 \"F.SilkS\" user \"F.Silkscreen\")") != std::string::npos,
            "starter board has KiCad's mask and silkscreen layers (plotted to Gerbers)");
    require(cc_create_project(parent.string().c_str(), "Starter", 50, 40,
                              error, sizeof(error)) == 0, "existing project is not overwritten");
    require(read(base.string() + ".kicad_pcb") == board_source, "duplicate preserves files");
    require(cc_create_project(parent.string().c_str(), "../escape", 50, 40,
                              error, sizeof(error)) == 0, "path traversal rejected");
    require(cc_create_project(parent.string().c_str(), "BadSize", 0, 40,
                              error, sizeof(error)) == 0, "invalid size rejected");
    require(!fs::exists(parent / "BadSize"), "invalid project leaves no folder");

    // Rename: the files follow, the board's sheetfile and the project's filename too.
    {
      std::ofstream board(base.string() + ".kicad_pcb", std::ios::app);
      board << "(footprint (sheetfile \"Starter.kicad_sch\"))\n";
    }
    { std::ofstream pro(base.string() + ".kicad_pro"); pro << "{\"meta\": {\"filename\": \"Starter.kicad_pro\"}}\n"; }
    require(cc_create_project(parent.string().c_str(), "Taken", 50, 40, error, sizeof(error)) == 1, "second project");
    require(cc_rename_project(parent.string().c_str(), "Starter", "Taken", error, sizeof(error)) == 0 &&
            fs::exists(base.string() + ".kicad_sch"), "rename onto an existing project is refused");
    require(cc_rename_project(parent.string().c_str(), "Starter", "../x", error, sizeof(error)) == 0,
            "rename path traversal rejected");
    require(cc_rename_project(parent.string().c_str(), "Starter", "Sensor board", error, sizeof(error)) == 1,
            "C API project rename");
    const fs::path renamed = parent / "Sensor board" / "Sensor board";
    require(!fs::exists(parent / "Starter") && fs::exists(renamed.string() + ".kicad_sch") &&
            fs::exists(parent / "Sensor board" / "CorvCAD.kicad_sym"), "folder and files renamed, libraries kept");
    const std::string renamed_board = read(renamed.string() + ".kicad_pcb");
    require(renamed_board.find("\"Sensor board.kicad_sch\"") != std::string::npos &&
            renamed_board.find("Starter") == std::string::npos, "board sheetfile follows");
    require(read(renamed.string() + ".kicad_pro").find("\"Sensor board.kicad_pro\"") != std::string::npos,
            "project filename follows");
  } catch (...) {
    fs::remove_all(parent);
    throw;
  }
  fs::remove_all(parent);
}

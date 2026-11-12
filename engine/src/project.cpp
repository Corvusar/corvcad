#include "corvcad/project.hpp"
#include "corvcad/authoring.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace corvcad {
namespace {
namespace fs = std::filesystem;

std::string uuid() {
  std::random_device random;
  std::uniform_int_distribution<unsigned> hex(0, 15);
  std::string value = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
  for (char &digit : value) {
    if (digit == 'x') digit = "0123456789abcdef"[hex(random)];
    else if (digit == 'y') digit = "89ab"[hex(random) % 4];
  }
  return value;
}

std::string number(double value) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::fixed << std::setprecision(3) << value;
  std::string text = output.str();
  while (text.back() == '0') text.pop_back();
  if (text.back() == '.') text.pop_back();
  return text;
}

void write(const fs::path &path, const std::string &contents) {
  std::ofstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Cannot create KiCad project file");
  file << contents;
  if (!file) throw std::runtime_error("Cannot finish writing KiCad project file");
}

void validate_name(const std::string &name) {
  if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ')
    throw std::runtime_error("Use a project name of 1–64 letters, numbers, spaces, hyphens or underscores");
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'))
      throw std::runtime_error("Project name contains unsupported characters");
  }
}
} // namespace

void create_project(const std::string &parent, const std::string &name,
                    double board_width, double board_height) {
  validate_name(name);
  if (!std::isfinite(board_width) || !std::isfinite(board_height) ||
      board_width < 10 || board_height < 10 || board_width > 500 || board_height > 500)
    throw std::runtime_error("Board width and height must be between 10 and 500 mm");
  if (parent.empty()) throw std::runtime_error("Missing project location");
  const fs::path folder = utf8_path(parent) / utf8_path(name);
  const std::string base = utf8_string(folder / utf8_path(name));
  if (!fs::create_directory(folder))
    throw std::runtime_error("A project with that name already exists");
  try {
    write(utf8_path(base + ".kicad_pro"), "{}\n");
    write(utf8_path(base + ".kicad_sch"),
          "(kicad_sch\n"
          "  (version 20250114)\n"
          "  (generator \"eeschema\")\n"
          "  (uuid \"" + uuid() + "\")\n"
          "  (paper \"A4\")\n"
          "  (lib_symbols)\n"
          "  (embedded_fonts no)\n"
          ")\n");
    write(utf8_path(base + ".kicad_pcb"),
          "(kicad_pcb\n"
          "  (version 20260206)\n"
          "  (generator \"pcbnew\")\n"
          "  (generator_version \"10.0\")\n"
          "  (general (thickness 1.6) (legacy_teardrops no))\n"
          "  (paper \"A4\")\n"
          // KiCad's two-layer board: without the mask, paste and silkscreen layers the Gerbers
          // would carry no solder mask or legend.
          "  (layers\n"
          "    (0 \"F.Cu\" signal)\n"
          "    (2 \"B.Cu\" signal)\n"
          "    (9 \"F.Adhes\" user \"F.Adhesive\")\n"
          "    (11 \"B.Adhes\" user \"B.Adhesive\")\n"
          "    (13 \"F.Paste\" user)\n"
          "    (15 \"B.Paste\" user)\n"
          "    (5 \"F.SilkS\" user \"F.Silkscreen\")\n"
          "    (7 \"B.SilkS\" user \"B.Silkscreen\")\n"
          "    (1 \"F.Mask\" user)\n"
          "    (3 \"B.Mask\" user)\n"
          "    (17 \"Dwgs.User\" user \"User.Drawings\")\n"
          "    (19 \"Cmts.User\" user \"User.Comments\")\n"
          "    (21 \"Eco1.User\" user \"User.Eco1\")\n"
          "    (23 \"Eco2.User\" user \"User.Eco2\")\n"
          "    (25 \"Edge.Cuts\" user)\n"
          "    (27 \"Margin\" user)\n"
          "    (31 \"F.CrtYd\" user \"F.Courtyard\")\n"
          "    (29 \"B.CrtYd\" user \"B.Courtyard\")\n"
          "    (35 \"F.Fab\" user)\n"
          "    (33 \"B.Fab\" user)\n"
          "  )\n"
          "  (gr_rect\n"
          "    (start 0 0) (end " + number(board_width) + " " + number(board_height) + ")\n"
          "    (stroke (width 0.05) (type default))\n"
          "    (fill no) (layer \"Edge.Cuts\")\n"
          "    (uuid \"" + uuid() + "\")\n"
          "  )\n"
          "  (embedded_fonts no)\n"
          ")\n");
    ensure_authoring_libraries(utf8_string(folder));
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(folder, ignored); // folder was created by this call
    throw;
  }
}

void rename_project(const std::string &parent, const std::string &name, const std::string &new_name) {
  validate_name(name);
  validate_name(new_name);
  if (parent.empty()) throw std::runtime_error("Missing project location");
  const fs::path folder = utf8_path(parent) / utf8_path(name);
  const fs::path target = utf8_path(parent) / utf8_path(new_name);
  if (!fs::is_directory(folder)) throw std::runtime_error("No project with that name");
  if (name == new_name) return;
  if (fs::exists(target)) throw std::runtime_error("A project with that name already exists");
  // KiCad's Save Project As in place: every "<name>.<ext>" file becomes "<new_name>.<ext>", and
  // the project file and board name the project's own files ("filename", "sheetfile").
  const std::string prefix = name + ".";
  std::vector<fs::path> files;
  for (const auto &entry : fs::directory_iterator(folder)) {
    const std::string file = utf8_string(entry.path().filename());
    if (entry.is_regular_file() && file.rfind(prefix, 0) == 0) files.push_back(entry.path());
  }
  for (const fs::path &file : files) {
    const std::string rest = utf8_string(file.filename()).substr(name.size());
    const fs::path renamed = folder / utf8_path(new_name + rest);
    if (rest == ".kicad_pro" || rest == ".kicad_pcb") {
      std::ifstream in(file, std::ios::binary);
      std::string text{std::istreambuf_iterator<char>(in), {}};
      for (const char *ext : {".kicad_sch\"", ".kicad_pro\"", ".kicad_pcb\""}) {
        const std::string from = "\"" + name + ext, to = "\"" + new_name + ext;
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
          text.replace(at, from.size(), to);
      }
      write(renamed, text);
      fs::remove(file);
    } else {
      fs::rename(file, renamed);
    }
  }
  fs::rename(folder, target);
}
} // namespace corvcad

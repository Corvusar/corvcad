#include "corvcad/c_api.h"
#include "corvcad/calculator.hpp"
#include "corvcad/engine.hpp"
#include "corvcad/gerber.hpp"
#include <cmath>
#include <cstdio>
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
bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }
std::string read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "fixture opens");
  return {std::istreambuf_iterator<char>(file), {}};
}

void test_gerber_features() {
  // KiCad's RoundRect macro, standard apertures, a line joined by a clockwise arc, a region, a
  // clear flash and a 2 x 2 step and repeat.
  const std::string text =
      "%FSLAX46Y46*%\n%MOMM*%\n"
      "%AMRoundRect*\n0 Rectangle with rounded corners*\n4,1,4,$2,$3,$4,$5,$6,$7,$8,$9,$2,$3,0*\n"
      "1,1,$1+$1,$2,$3*\n1,1,$1+$1,$4,$5*\n1,1,$1+$1,$6,$7*\n1,1,$1+$1,$8,$9*\n"
      "20,1,$1+$1,$2,$3,$4,$5,0*\n20,1,$1+$1,$4,$5,$6,$7,0*\n20,1,$1+$1,$6,$7,$8,$9,0*\n20,1,$1+$1,$8,$9,$2,$3,0*%\n"
      "%ADD10C,0.200000*%\n%ADD11RoundRect,0.25X-1X0.5X-1X-0.5X1X-0.5X1X0.5X0*%\n%ADD12R,2X1*%\n"
      "G04 #@! TF.FileFunction,Copper,L1,Top*\nG75*\nG01*\n"
      "D11*\nX0Y0D03*\n"
      "D10*\nX0Y0D02*\nX10000000Y0D01*\nG02*\nX20000000Y0I5000000J0D01*\nG01*\n"
      "G36*\nX0Y-10000000D02*\nX10000000Y-10000000D01*\nX10000000Y-20000000D01*\nX0Y-20000000D01*\nX0Y-10000000D01*\nG37*\n"
      "%LPC*%\nX5000000Y-15000000D03*\n%LPD*%\n"
      "%SRX2Y2I30J30*%\nD12*\nX40000000Y0D03*\n%SR*%\nM02*\n";
  const auto layer = corvcad::parse_gerber_file(text, "features.gbr");
  require(!layer.drill && layer.function == "Copper,L1,Top" && layer.warnings.empty(), "Gerber recognised, X2 function read");
  require(layer.flashes == 3 && layer.draws == 2 && layer.regions == 1 && layer.items.size() == 8,
          "flashes, joined draw, region and the repeated flash");
  require(near(layer.min_x, -1.25, 1e-9) && near(layer.max_x, 71) && near(layer.max_y, 30.5) && near(layer.min_y, -20),
          "extents include the macro's rounded corners and the repeated rectangles");
  const auto &draw = layer.items[1];
  double top = -1;
  for (const auto &p : draw.path) top = std::max(top, p.y);
  require(near(draw.width, 0.2) && near(draw.path.back().x, 20) && near(draw.path.back().y, 0) && near(top, 5, 1e-9),
          "clockwise arc from (10,0) over the top of its circle to (20,0)");
  require(!layer.items[3].dark && layer.items[2].polygons.size() == 1 && layer.items[2].polygons[0].points.size() == 4,
          "region contour and clear-polarity flash");

  const auto inch = corvcad::parse_gerber_file("%FSLAX24Y24*%%MOIN*%%ADD10C,0.01*%D10*X10000Y0D03*M02*", "inch.gbr");
  require(near(inch.max_x, 25.4 + 0.127, 1e-9), "inch coordinates and apertures in millimetres");

  const auto drill = corvcad::parse_gerber_file(
      "M48\nMETRIC\nT1C0.800\nT2C1.000\n%\nG90\nG05\nT1\nX1.0Y-2.0\nX3.5Y-2.0\nT2\nX10.0Y-5.0G85X12.0Y-5.0\n"
      "G00X20.0Y-5.0\nM15\nG01X22.0Y-5.0\nM16\nG05\nM30\n", "test.drl");
  require(drill.drill && drill.holes == 2 && drill.slots == 2 && near(drill.items[2].width, 1) &&
              near(drill.items[3].path.back().x, 22), "Excellon holes, G85 slot and routed slot");
}

void test_gerber_fixtures() {
  // kicad-cli 10.0.6 Gerbers and drill files of CorvCAD's own fixture boards, against the boards.
  const fs::path dir = CORVCAD_FIXTURES;
  const auto board = corvcad::parse_kicad_board(read(dir / "roundtrip" / "RoundTrip.kicad_pcb"));
  for (const auto &entry : fs::directory_iterator(dir / "gerber")) {
    const auto layer = corvcad::parse_gerber_file(read(entry.path()), entry.path().filename().string());
    require(layer.warnings.empty(), "fixture Gerbers read without warnings");
  }
  const auto gerber = [&](const char *name) { return corvcad::parse_gerber_file(read(dir / "gerber" / name), name); };
  const auto edge = gerber("RoundTrip-Edge_Cuts.gbr");
  require(edge.function.rfind("Profile", 0) == 0 && near(edge.min_x, board.min_x, 1e-6) && near(edge.max_x, board.max_x, 1e-6) &&
              near(edge.min_y, -board.max_y, 1e-6) && near(edge.max_y, -board.min_y, 1e-6),
          "Edge.Cuts extents are the board outline (Gerber y up)");
  int front = 0, back = 0, plated = 0, unplated = 0;
  for (const auto &pad : board.pads) {
    const bool all = pad.layers.find("*.Cu") != std::string::npos;
    front += all || pad.layers.find("F.Cu") != std::string::npos;
    back += all || pad.layers.find("B.Cu") != std::string::npos;
    if (pad.type == "thru_hole") ++plated;
    if (pad.type == "np_thru_hole") ++unplated;
  }
  const int vias = static_cast<int>(board.vias.size());
  require(gerber("RoundTrip-F_Cu.gbr").flashes == front + vias && gerber("RoundTrip-B_Cu.gbr").flashes == back + vias,
          "one copper flash per pad on the layer and per via");
  const auto pth = gerber("RoundTrip-PTH.drl"), npth = gerber("RoundTrip-NPTH.drl");
  require(pth.drill && pth.holes + pth.slots == plated + vias && npth.holes + npth.slots == unplated, "a drill hit per hole");
  require(gerber("Drawn-B_Cu.gbr").regions == 1 && gerber("Drawn-F_Cu.gbr").regions == 1 && gerber("Drawn-PTH.drl").holes == 1,
          "the drawn board's filled zones and its via");
  char error[256]{};
  char *json = cc_gerber_json(read(dir / "gerber" / "RoundTrip-NPTH.drl").c_str(), "RoundTrip-NPTH.drl", error, sizeof error);
  require(json && std::string(json).find("\"drill\":true") != std::string::npos &&
              std::string(json).find("\"holes\":2") != std::string::npos, "C API Gerber JSON");
  cc_free_text(json);

  // Optional corpus (KiCad's gerbview/gerber_test_files on the iMac): every file must parse.
  if (const char *corpus = std::getenv("CORVCAD_GERBER_CORPUS")) {
    int files = 0;
    for (const auto &entry : fs::recursive_directory_iterator(corpus)) {
      if (!entry.is_regular_file() || entry.path().extension() == ".gbrjob") continue;
      ++files;
      try {
        const auto layer = corvcad::parse_gerber_file(read(entry.path()), entry.path().filename().string());
        std::printf("%s: %s %d flashes %d draws %d regions %d holes %d slots%s%s\n", entry.path().filename().string().c_str(),
                    layer.drill ? "drill" : "gerber", layer.flashes, layer.draws, layer.regions, layer.holes, layer.slots,
                    layer.warnings.empty() ? "" : " | ", layer.warnings.empty() ? "" : layer.warnings.front().c_str());
      } catch (const std::exception &e) {
        std::printf("%s: FAILED %s\n", entry.path().filename().string().c_str(), e.what());
      }
    }
    std::printf("corpus: %d files\n", files);
  }
}

void test_calculator() {
  // Expected values: KiCad pcb_calculator's formulas recomputed independently (Python).
  const auto ext = corvcad::track(1, 0, 35, 10, 100, false);
  require(near(ext.width_mm, 0.300387, 1e-6) && near(ext.resistance_ohm, 0.163599, 1e-6) &&
              near(ext.voltage_drop_v, 0.163599, 1e-6), "1 A external track on 35 um copper, 10 C rise");
  require(near(corvcad::track(1, 0, 35, 10, 0, true).width_mm, 0.781437, 1e-6), "internal layer");
  require(near(corvcad::track(0, 0.25, 35, 10, 0, false).current_a, 0.875363, 1e-6), "current of a 0.25 mm track");
  const auto v = corvcad::via({});
  require(near(v.resistance_ohm, 8.244601e-4, 1e-9) && near(v.thermal_c_per_w, 119.355, 1e-3) &&
              near(v.ampacity_a, 2.310758, 1e-6) && near(v.capacitance_pf, 0.599508, 1e-6) &&
              near(v.rise_degradation_ps, 32.97294, 1e-5) && near(v.inductance_nh, 1.207228, 1e-6) &&
              near(v.reactance_ohm, 3.79262, 1e-5), "via with KiCad's defaults");
  const auto low = corvcad::electrical_spacing(12), high = corvcad::electrical_spacing(600);
  require(near(low[0], 0.05) && near(low[5], 0.13) && near(high[0], 0.5) && near(high[2], 15) &&
              near(corvcad::electrical_spacing(500)[1], 2.5), "IPC-2221 spacing, with the per-volt rows above 500 V");
  require(near(corvcad::regulator_vout(1.25, 240, 720, 50e-6), 5.036) &&
              near(corvcad::regulator_r2(1.25, 5, 240), 720), "LM317-style divider");
  require(corvcad::eseries(96).size() == 96 && corvcad::eseries(48).size() == 48 && corvcad::eseries(6).size() == 6 &&
              near(corvcad::nearest_eseries(4650, 24), 4700) && near(corvcad::nearest_eseries(9.8, 12), 10),
          "E-series values and nearest");
  const auto bands = corvcad::resistor_colours(4700, 24), precise = corvcad::resistor_colours(10000, 96);
  require(bands == std::vector<std::string>{"yellow", "violet", "red", "gold"} &&
              precise == std::vector<std::string>{"brown", "black", "black", "red", "brown"} &&
              corvcad::resistor_colours(4.7, 12) == std::vector<std::string>{"yellow", "violet", "gold", "silver"},
          "colour bands");
  char text[64]{}, error[128]{};
  double value = 0;
  require(cc_calc_resistor(332, 96, &value, text, sizeof text, error, sizeof error) == 1 && near(value, 332) &&
              std::string(text) == "orange,orange,red,black,brown", "C API resistor");
  bool refused = false;
  try { corvcad::via({0.4, 25, 1.6, 1.0, 0.8}); } catch (const std::exception &) { refused = true; }
  require(refused, "a clearance inside the pad is refused");
}

int main() {
  test_gerber_features();
  test_gerber_fixtures();
  test_calculator();
}

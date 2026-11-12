#include "corvcad/calculator.hpp"
#include <cmath>
#include <stdexcept>

namespace corvcad {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double mil = 2.54e-5; // metres
void positive(double v, const char *what) {
  if (!std::isfinite(v) || v <= 0) throw std::runtime_error(std::string("Enter a positive ") + what);
}
}

TrackResult track(double current_a, double width_mm, double thickness_um, double rise_c, double length_mm, bool internal,
                  double resistivity) {
  positive(thickness_um, "copper thickness");
  positive(rise_c, "temperature rise");
  const double k = internal ? 0.024 : 0.048;
  const double thickness_mil = thickness_um * 1e-6 / mil;
  TrackResult r;
  if (current_a > 0) {
    // A^0.725 = I / (k dT^0.44); width = A / thickness.
    const double area_mil2 = std::pow(current_a / (k * std::pow(rise_c, 0.44)), 1 / 0.725);
    r.current_a = current_a;
    r.width_mm = area_mil2 / thickness_mil * mil * 1e3;
  } else {
    positive(width_mm, "track width or current");
    const double area_mil2 = thickness_mil * (width_mm * 1e-3 / mil);
    r.width_mm = width_mm;
    r.current_a = k * std::pow(rise_c, 0.44) * std::pow(area_mil2, 0.725);
  }
  const double width_m = r.width_mm * 1e-3, thickness_m = thickness_um * 1e-6;
  r.area_mm2 = r.width_mm * thickness_um * 1e-3;
  r.resistance_ohm = length_mm > 0 ? resistivity * length_mm * 1e-3 / (width_m * thickness_m) : 0;
  r.voltage_drop_v = r.resistance_ohm * r.current_a;
  r.power_w = r.voltage_drop_v * r.current_a;
  return r;
}

ViaResult via(const ViaInput &in) {
  positive(in.hole_mm, "hole diameter");
  positive(in.plating_um, "plating thickness");
  positive(in.length_mm, "via length");
  positive(in.rise_time_ns, "rise time");
  if (in.clearance_mm <= in.pad_mm) throw std::runtime_error("The clearance diameter must be larger than the pad");
  const double d = in.hole_mm * 1e-3, t = in.plating_um * 1e-6, length = in.length_mm * 1e-3;
  const double pad = in.pad_mm * 1e-3, clearance = in.clearance_mm * 1e-3;
  const double area = pi * (d + t) * t; // plated barrel cross-section, m²
  ViaResult r;
  r.resistance_ohm = in.resistivity * length / area;
  r.thermal_c_per_w = 2.49e-3 * length / area; // copper, 401 W/(m K)
  r.voltage_drop_v = in.current_a * r.resistance_ohm;
  r.power_w = in.current_a * r.voltage_drop_v;
  r.ampacity_a = 0.048 * std::pow(in.rise_c, 0.44) * std::pow(area / (mil * mil), 0.725);
  r.capacitance_pf = 55.51 * in.permittivity * length * pad / (clearance - pad);
  r.rise_degradation_ps = 2.2 * r.capacitance_pf * in.impedance_ohm / 2;
  r.inductance_nh = 200 * length * (std::log(4 * length / d) + 1);
  r.reactance_ohm = pi * r.inductance_nh / in.rise_time_ns;
  return r;
}

std::array<double, 7> electrical_spacing(double volts) {
  if (!std::isfinite(volts) || volts < 0) throw std::runtime_error("Enter a voltage of 0 V or more");
  static const double table[10][7] = {
      {0.05, 0.1, 0.1, 0.05, 0.13, 0.13, 0.13},        // 0-15 V
      {0.05, 0.1, 0.1, 0.05, 0.13, 0.25, 0.13},        // 16-30 V
      {0.1, 0.6, 0.6, 0.13, 0.13, 0.4, 0.13},          // 31-50 V
      {0.1, 0.6, 1.5, 0.13, 0.13, 0.5, 0.13},          // 51-100 V
      {0.2, 0.6, 3.2, 0.4, 0.4, 0.8, 0.4},             // 101-150 V
      {0.2, 1.25, 3.2, 0.4, 0.4, 0.8, 0.4},            // 151-170 V
      {0.2, 1.25, 6.4, 0.4, 0.4, 0.8, 0.4},            // 171-250 V
      {0.2, 1.25, 12.5, 0.4, 0.4, 0.8, 0.8},           // 251-300 V
      {0.25, 2.5, 12.5, 0.8, 0.8, 1.5, 0.8},           // 301-500 V
      {0.0025, 0.005, 0.025, 0.00305, 0.00305, 0.00305, 0.00305}, // per volt above 500 V
  };
  static const double upper[9] = {15, 30, 50, 100, 150, 170, 250, 300, 500};
  std::array<double, 7> spacing{};
  for (int row = 0; row < 9; ++row)
    if (volts <= upper[row]) {
      for (int c = 0; c < 7; ++c) spacing[c] = table[row][c];
      return spacing;
    }
  for (int c = 0; c < 7; ++c) spacing[c] = table[8][c] + table[9][c] * (volts - 500);
  return spacing;
}

double regulator_vout(double vref, double r1, double r2, double iadj) {
  positive(vref, "reference voltage");
  positive(r1, "R1");
  if (!std::isfinite(r2) || r2 < 0) throw std::runtime_error("Enter R2 of 0 ohms or more");
  return vref * (r1 + r2) / r1 + iadj * r2;
}

double regulator_r2(double vref, double vout, double r1, double iadj) {
  positive(vref, "reference voltage");
  positive(r1, "R1");
  if (vout <= vref) throw std::runtime_error("Vout must be greater than Vref");
  // vout = vref + R2 (vref / R1 + iadj)
  return (vout - vref) / (vref / r1 + iadj);
}

const std::vector<double> &eseries(int series) {
  static const std::vector<double> e24 = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                          3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};
  static const std::vector<double> e96 = {
      1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30, 1.33, 1.37, 1.40, 1.43,
      1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74, 1.78, 1.82, 1.87, 1.91, 1.96, 2.00, 2.05, 2.10,
      2.15, 2.21, 2.26, 2.32, 2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87, 2.94, 3.01, 3.09,
      3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12, 4.22, 4.32, 4.42, 4.53,
      4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49, 5.62, 5.76, 5.90, 6.04, 6.19, 6.34, 6.49, 6.65,
      6.81, 6.98, 7.15, 7.32, 7.50, 7.68, 7.87, 8.06, 8.25, 8.45, 8.66, 8.87, 9.09, 9.31, 9.53, 9.76};
  // E12 and E6 take every second value of E24 and E12; E48 every second of E96.
  const auto every_other = [](const std::vector<double> &v) {
    std::vector<double> out;
    for (size_t i = 0; i < v.size(); i += 2) out.push_back(v[i]);
    return out;
  };
  static const std::vector<double> e12 = every_other(e24), e6 = every_other(e12), e48 = every_other(e96);
  switch (series) {
  case 6: return e6;
  case 12: return e12;
  case 24: return e24;
  case 48: return e48;
  case 96: return e96;
  default: throw std::runtime_error("Use the E6, E12, E24, E48 or E96 series");
  }
}

double nearest_eseries(double value, int series) {
  positive(value, "value");
  const auto &values = eseries(series);
  const double decade = std::pow(10.0, std::floor(std::log10(value)));
  const double mantissa = value / decade;
  double best = values.front(), error = INFINITY;
  for (double candidate : values) {
    const double e = std::abs(std::log(mantissa / candidate));
    if (e < error) { error = e; best = candidate; }
  }
  // The next decade's 1.0 can be nearer than the series' last value.
  if (std::abs(std::log(mantissa / 10)) < error) best = 10;
  return std::round(best * decade * 1e6) / 1e6;
}

std::vector<std::string> resistor_colours(double ohms, int series) {
  static const char *names[] = {"black", "brown", "red", "orange", "yellow", "green", "blue", "violet", "grey", "white"};
  const double value = nearest_eseries(ohms, series);
  const int digits = series >= 48 ? 3 : 2;
  // value = significand * 10^exponent with `digits` significant digits.
  int exponent = static_cast<int>(std::floor(std::log10(value))) - (digits - 1);
  long significand = std::lround(value / std::pow(10.0, exponent));
  if (significand >= static_cast<long>(std::pow(10, digits))) { significand /= 10; ++exponent; }
  if (exponent < -2 || exponent > 9) throw std::runtime_error("The value is outside the colour code's range");
  std::vector<std::string> bands;
  for (int i = digits - 1; i >= 0; --i) bands.push_back(names[(significand / static_cast<long>(std::pow(10, i))) % 10]);
  bands.push_back(exponent == -2 ? "silver" : exponent == -1 ? "gold" : names[exponent]);
  const char *tolerance = series == 12 ? "silver" : series == 24 ? "gold" : series == 48 ? "red" : series == 96 ? "brown" : "";
  if (*tolerance) bands.push_back(tolerance);
  return bands;
}
}

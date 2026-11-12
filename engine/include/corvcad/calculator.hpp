#pragma once

#include <array>
#include <string>
#include <vector>

namespace corvcad {
// PCB calculator: the formulas and constants of KiCad 10's pcb_calculator for the same inputs
// (IPC-2221 track and via ampacity, IPC-2221 electrical spacing, adjustable regulator
// dividers, IEC 60063 E-series and the resistor colour code). Lengths in millimetres, plating
// and copper thickness in micrometres.

struct TrackResult { double current_a = 0, width_mm = 0, area_mm2 = 0, resistance_ohm = 0, voltage_drop_v = 0, power_w = 0; };
// IPC-2221: I = k * dT^0.44 * A^0.725 (A in mil²; k 0.048 external, 0.024 internal). Give the
// current (width 0) for the width, or the width (current 0) for the current.
TrackResult track(double current_a, double width_mm, double thickness_um, double rise_c, double length_mm, bool internal,
                  double resistivity = 1.72e-8);

struct ViaInput {
  double hole_mm = 0.4, plating_um = 25, length_mm = 1.6, pad_mm = 0.6, clearance_mm = 1.0;
  double impedance_ohm = 50, current_a = 1, resistivity = 1.72e-8, permittivity = 4.5, rise_c = 10, rise_time_ns = 1;
};
struct ViaResult {
  double resistance_ohm = 0, voltage_drop_v = 0, power_w = 0, ampacity_a = 0, thermal_c_per_w = 0;
  double capacitance_pf = 0, rise_degradation_ps = 0, inductance_nh = 0, reactance_ohm = 0;
};
ViaResult via(const ViaInput &input);

// IPC-2221 minimum spacing in millimetres for a peak voltage, in KiCad's column order: B1
// internal; B2 external uncoated to 3050 m; B3 external uncoated above 3050 m; B4 external
// with polymer coating; A5 external with conformal coating over the assembly; A6 external
// component lead uncoated; A7 external component lead with conformal coating.
std::array<double, 7> electrical_spacing(double volts);

// Adjustable regulator: Vout = Vref * (R1 + R2) / R1 + Iadj * R2 (Iadj 0 for the standard type).
double regulator_vout(double vref, double r1_ohm, double r2_ohm, double iadj_a = 0);
double regulator_r2(double vref, double vout, double r1_ohm, double iadj_a = 0);

// IEC 60063 series E6, E12, E24, E48 or E96.
const std::vector<double> &eseries(int series);
double nearest_eseries(double value, int series);
// Resistor colour bands for a value: two significant digits for E6-E24, three for E48 and E96,
// then the multiplier and the series' tolerance band (none for E6's 20%).
std::vector<std::string> resistor_colours(double ohms, int series);
}

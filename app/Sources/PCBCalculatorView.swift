import SwiftUI

/// PCB calculator: KiCad pcb_calculator's track width, via, IPC-2221 clearance, regulator divider
/// and resistor colour code calculations, computed by the engine (corvcad/calculator.hpp).
struct PCBCalculatorView: View {
    @Environment(\.dismiss) private var dismiss
    private enum Tab: String, CaseIterable, Identifiable {
        case track = "Track width", via = "Via", spacing = "Clearance", regulator = "Regulator", resistor = "Resistor"
        var id: String { rawValue }
    }
    @State private var tab = Tab.track
    // Track width
    @State private var solveWidth = true
    @State private var current = 1.0
    @State private var width = 0.25
    @State private var thickness = 35.0
    @State private var rise = 10.0
    @State private var length = 50.0
    @State private var innerLayer = false
    // Via (KiCad's defaults)
    @State private var via = cc_via_calc_input(hole_mm: 0.4, plating_um: 25, length_mm: 1.6, pad_mm: 0.6, clearance_mm: 1.0,
                                               impedance_ohm: 50, current_a: 1, resistivity: 1.72e-8, permittivity: 4.5,
                                               rise_c: 10, rise_time_ns: 1)
    // Clearance
    @State private var volts = 50.0
    // Regulator
    @State private var vref = 1.25
    @State private var r1 = 240.0
    @State private var r2 = 720.0
    @State private var vout = 5.0
    @State private var iadj = 0.0
    @State private var solveR2 = false
    // Resistor
    @State private var ohms = 4700.0
    @State private var series: Int32 = 24

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                MonoLabel("PCB calculator", color: Corv.gold)
                Spacer()
                Button("Close") { dismiss() }.buttonStyle(.corvGhost)
            }
            Picker("Calculator", selection: $tab) {
                ForEach(Tab.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("calculatorTab")
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    switch tab {
                    case .track: trackForm
                    case .via: viaForm
                    case .spacing: spacingForm
                    case .regulator: regulatorForm
                    case .resistor: resistorForm
                    }
                }
                .frame(maxWidth: 760, alignment: .leading)
            }
        }
        .padding(20)
        .background(Corv.void)
    }

    private func message(_ text: String) -> some View {
        Text(text).font(.caption).foregroundStyle(Corv.red)
    }

    private func number(_ value: Double, _ digits: Int = 4) -> String {
        value == 0 ? "0" : String(format: "%.\(digits)g", value)
    }

    // MARK: Track width

    private var trackForm: some View {
        var result = cc_track_calc()
        var buffer = [CChar](repeating: 0, count: 256)
        let capacity = buffer.count
        let ok = cc_calc_track(solveWidth ? current : 0, solveWidth ? 0 : width, thickness, rise, length, innerLayer ? 1 : 0,
                               &result, &buffer, capacity) == 1
        return InspectorSection("IPC-2221 track width") {
            Picker("Solve for", selection: $solveWidth) {
                Text("Width from current").tag(true)
                Text("Current from width").tag(false)
            }
            .pickerStyle(.segmented)
            Picker("Layer", selection: $innerLayer) {
                Text("External layer").tag(false)
                Text("Internal layer").tag(true)
            }
            .pickerStyle(.segmented)
            HStack(spacing: 10) {
                if solveWidth { NumberField(title: "Current A", value: $current) } else { NumberField(title: "Width mm", value: $width) }
                NumberField(title: "Copper µm", value: $thickness)
                NumberField(title: "Rise °C", value: $rise)
                NumberField(title: "Length mm", value: $length)
            }
            if ok {
                StatGrid(items: [
                    solveWidth ? Stat("Width mm", number(result.width_mm)) : Stat("Current A", number(result.current_a)),
                    Stat("Area mm²", number(result.area_mm2)),
                    Stat("Resistance Ω", number(result.resistance_ohm)),
                    Stat("Drop V", number(result.voltage_drop_v)),
                    Stat("Loss W", number(result.power_w)),
                ])
            } else {
                message(String(cString: buffer))
            }
            CautionNote(text: "I = k · ΔT^0.44 · A^0.725 (A in mil², k 0.048 external, 0.024 internal), copper resistivity 1.72e-8 Ω·m, as KiCad's calculator.")
        }
    }

    // MARK: Via

    private var viaForm: some View {
        var result = cc_via_calc()
        var buffer = [CChar](repeating: 0, count: 256)
        let capacity = buffer.count
        let ok = cc_calc_via(via, &result, &buffer, capacity) == 1
        return InspectorSection("Via") {
            HStack(spacing: 10) {
                NumberField(title: "Hole mm", value: $via.hole_mm)
                NumberField(title: "Plating µm", value: $via.plating_um)
                NumberField(title: "Length mm", value: $via.length_mm)
            }
            HStack(spacing: 10) {
                NumberField(title: "Pad mm", value: $via.pad_mm)
                NumberField(title: "Antipad mm", value: $via.clearance_mm)
                NumberField(title: "Z0 Ω", value: $via.impedance_ohm)
            }
            HStack(spacing: 10) {
                NumberField(title: "Current A", value: $via.current_a)
                NumberField(title: "εr", value: $via.permittivity)
                NumberField(title: "Rise °C", value: $via.rise_c)
                NumberField(title: "Rise time ns", value: $via.rise_time_ns)
            }
            if ok {
                StatGrid(items: [
                    Stat("Resistance Ω", number(result.resistance_ohm)),
                    Stat("Drop V", number(result.voltage_drop_v)),
                    Stat("Loss W", number(result.power_w)),
                    Stat("Ampacity A", number(result.ampacity_a)),
                    Stat("Thermal °C/W", number(result.thermal_c_per_w)),
                    Stat("Capacitance pF", number(result.capacitance_pf)),
                    Stat("Rise degradation ps", number(result.rise_degradation_ps)),
                    Stat("Inductance nH", number(result.inductance_nh)),
                    Stat("Reactance Ω", number(result.reactance_ohm)),
                ])
            } else {
                message(String(cString: buffer))
            }
        }
    }

    // MARK: Clearance

    private var spacingForm: some View {
        var values = [Double](repeating: 0, count: 7)
        var buffer = [CChar](repeating: 0, count: 256)
        let capacity = buffer.count
        let ok = cc_calc_spacing(volts, &values, &buffer, capacity) == 1
        let rows = ["B1 · internal conductors", "B2 · external, uncoated, up to 3050 m", "B3 · external, uncoated, above 3050 m",
                    "B4 · external, permanent polymer coating", "A5 · external, conformal coating over assembly",
                    "A6 · external component lead, uncoated", "A7 · external component lead, conformal coating"]
        return InspectorSection("IPC-2221 electrical clearance") {
            NumberField(title: "Peak voltage V", value: $volts).frame(maxWidth: 220)
            if ok {
                VStack(spacing: 6) {
                    ForEach(0..<7, id: \.self) { index in
                        HStack {
                            Text(rows[index]).font(.caption).foregroundStyle(Corv.textDim)
                            Spacer()
                            Text(String(format: "%.3f mm", values[index])).font(.corvReadout).foregroundStyle(Corv.cyan)
                        }
                        .padding(.vertical, 6).padding(.horizontal, 10)
                        .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 8))
                    }
                }
            } else {
                message(String(cString: buffer))
            }
        }
    }

    // MARK: Regulator

    private var regulatorForm: some View {
        var result = 0.0
        var buffer = [CChar](repeating: 0, count: 256)
        let capacity = buffer.count
        let ok = cc_calc_regulator(vref, r1, r2, vout, iadj * 1e-6, solveR2 ? 1 : 0, &result, &buffer, capacity) == 1
        return InspectorSection("Adjustable regulator") {
            Picker("Solve for", selection: $solveR2) {
                Text("Vout from R1, R2").tag(false)
                Text("R2 from Vout, R1").tag(true)
            }
            .pickerStyle(.segmented)
            HStack(spacing: 10) {
                NumberField(title: "Vref V", value: $vref)
                NumberField(title: "R1 Ω", value: $r1)
                if solveR2 { NumberField(title: "Vout V", value: $vout) } else { NumberField(title: "R2 Ω", value: $r2) }
                NumberField(title: "Iadj µA", value: $iadj)
            }
            if ok {
                StatGrid(items: [solveR2 ? Stat("R2 Ω", number(result, 5)) : Stat("Vout V", number(result, 5))])
            } else {
                message(String(cString: buffer))
            }
            CautionNote(text: "Vout = Vref · (R1 + R2) / R1 + Iadj · R2. Leave Iadj at 0 for regulators without an adjust-pin current.")
        }
    }

    // MARK: Resistor

    private var resistorForm: some View {
        var value = 0.0
        var bands = [CChar](repeating: 0, count: 128)
        var buffer = [CChar](repeating: 0, count: 256)
        let bandCapacity = bands.count, capacity = buffer.count
        let ok = cc_calc_resistor(ohms, series, &value, &bands, bandCapacity, &buffer, capacity) == 1
        let colours = String(cString: bands).split(separator: ",").map(String.init)
        return InspectorSection("E-series and colour code") {
            HStack(spacing: 10) {
                NumberField(title: "Value Ω", value: $ohms).frame(maxWidth: 220)
                Picker("Series", selection: $series) {
                    ForEach([6, 12, 24, 48, 96] as [Int32], id: \.self) { Text("E\($0)").tag($0) }
                }
                .pickerStyle(.segmented)
            }
            if ok {
                StatGrid(items: [Stat("Nearest Ω", number(value, 4)), Stat("Bands", colours.joined(separator: " "))])
                ResistorBands(colours: colours)
                    .frame(height: 70)
                    .accessibilityLabel("Colour bands: \(colours.joined(separator: ", "))")
            } else {
                message(String(cString: buffer))
            }
        }
    }
}

/// A through-hole resistor body with its colour bands.
private struct ResistorBands: View {
    let colours: [String]
    private static let paint: [String: Color] = [
        "black": Color(white: 0.08), "brown": Color(red: 0.45, green: 0.25, blue: 0.1), "red": Color(red: 0.85, green: 0.1, blue: 0.1),
        "orange": Color(red: 1, green: 0.5, blue: 0), "yellow": Color(red: 1, green: 0.85, blue: 0), "green": Color(red: 0, green: 0.6, blue: 0.2),
        "blue": Color(red: 0.1, green: 0.3, blue: 0.9), "violet": Color(red: 0.55, green: 0.2, blue: 0.75), "grey": Color(white: 0.55),
        "white": Color(white: 0.95), "gold": Color(red: 0.83, green: 0.68, blue: 0.21), "silver": Color(white: 0.75),
    ]

    var body: some View {
        Canvas { context, size in
            let body = CGRect(x: size.width * 0.2, y: size.height * 0.2, width: size.width * 0.6, height: size.height * 0.6)
            var lead = Path()
            lead.move(to: CGPoint(x: 0, y: size.height / 2)); lead.addLine(to: CGPoint(x: size.width, y: size.height / 2))
            context.stroke(lead, with: .color(Corv.textFaint), lineWidth: 2)
            context.fill(Path(roundedRect: body, cornerRadius: body.height / 2), with: .color(Color(red: 0.82, green: 0.72, blue: 0.55)))
            let band = body.width / CGFloat(max(colours.count * 2 + 1, 1))
            for (index, name) in colours.enumerated() {
                // The tolerance band stands apart at the right end.
                let tolerance = colours.count > 3 && index == colours.count - 1
                let x = tolerance ? body.maxX - band * 1.6 : body.minX + band * CGFloat(index * 2 + 1)
                context.fill(Path(CGRect(x: x, y: body.minY, width: band * 0.8, height: body.height)),
                             with: .color(Self.paint[name] ?? .clear))
            }
        }
    }
}

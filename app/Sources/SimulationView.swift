import Charts
import SwiftUI

/// KiCad's node name in ngspice's results (NETLIST_EXPORTER_SPICE::ConvertToSpiceMarkup, then the
/// lower case ngspice uses).
enum SpiceNames {
    static func node(_ net: String) -> String {
        var name = net
            .replacing(/~\{([^}]*)\}/) { "~" + $0.1 }
            .replacing(/[_^]\{([^}]*)\}/) { String($0.1) }
        for character in "%(),[]<>~ " { name = name.replacingOccurrences(of: String(character), with: "_") }
        if name.hasSuffix("/0") && !name.hasSuffix("//0") { name = "0" }
        if name.lowercased() == "/gnd" { name.removeFirst() }
        if name.hasPrefix("//") { name = "/root/" + name.dropFirst(2) }
        return name.lowercased()
    }

    /// Nets at 0 V by SPICE's rules: they have no vector of their own.
    static func isGround(_ net: String) -> Bool {
        let name = node(net)
        return name == "0" || name == "gnd"
    }
}

/// One run: cc_sch_session_simulate's text, and the netlist it ran.
struct SimulationResult {
    struct Vector {
        let name: String
        let real: [Double]
        /// Complex vectors only (AC).
        let imaginary: [Double]?
    }
    /// A device in the netlist: its instance name and node names, lower case.
    struct Element {
        let name: String
        let nodes: [String]
    }

    let analysis: String
    let plot: String
    /// Points the run produced, and kept here.
    let points: Int
    let kept: Int
    let vectors: [String: Vector]
    let names: [String]
    let elements: [String: Element]

    init?(text: String, netlist: String) {
        let lines = text.split(separator: "\n", omittingEmptySubsequences: true)
        let header = lines.first?.split(separator: "\t", omittingEmptySubsequences: false) ?? []
        guard header.count == 4, let points = Int(header[2]), let kept = Int(header[3]) else { return nil }
        analysis = String(header[0])
        plot = String(header[1])
        self.points = points
        self.kept = kept
        var vectors: [String: Vector] = [:]
        for line in lines.dropFirst() {
            let fields = line.split(separator: "\t", maxSplits: 2, omittingEmptySubsequences: false)
            guard fields.count == 3 else { continue }
            let name = String(fields[0])
            if fields[1] == "complex" {
                var real: [Double] = [], imaginary: [Double] = []
                for value in fields[2].split(separator: " ") {
                    let parts = value.split(separator: ",")
                    real.append(Double(parts[0]) ?? 0)
                    imaginary.append(parts.count > 1 ? Double(parts[1]) ?? 0 : 0)
                }
                vectors[name] = Vector(name: name, real: real, imaginary: imaginary)
            } else {
                vectors[name] = Vector(name: name, real: fields[2].split(separator: " ").map { Double($0) ?? 0 }, imaginary: nil)
            }
        }
        self.vectors = vectors
        names = vectors.keys.sorted()
        var elements: [String: Element] = [:]
        for line in netlist.split(separator: "\n") {
            let tokens = line.split(whereSeparator: \.isWhitespace).map { $0.lowercased() }
            guard let first = tokens.first, let letter = first.first, "rcldviqmj".contains(letter), tokens.count >= 3 else { continue }
            let count = letter == "q" || letter == "j" ? 3 : letter == "m" ? 4 : 2
            elements[first] = Element(name: first, nodes: Array(tokens.dropFirst().prefix(count)))
        }
        self.elements = elements
    }

    /// The x axis: time, frequency or a DC sweep's source. Nil for an operating point.
    var scale: Vector? {
        vectors["time"] ?? vectors["frequency"] ?? names.first { $0.hasSuffix("-sweep") }.flatMap { vectors[$0] }
    }
    var isAC: Bool { vectors["frequency"]?.imaginary != nil }
    /// A transient, sweep or operating point has one state per instant; AC results don't.
    var animatable: Bool { !isAC && !vectors.isEmpty }
    var xUnit: String { vectors["time"] != nil ? "s" : vectors["frequency"] != nil ? "Hz" : names.contains("res-sweep") ? "Ω" : names.contains("i-sweep") ? "A" : names.contains("temp-sweep") ? "°C" : "V" }

    /// Everything but the x axis, the circuit's own signals before subcircuit internals.
    var signals: [String] {
        names.filter { $0 != scale?.name }.sorted { (isInternal($0) ? 1 : 0, $0) < (isInternal($1) ? 1 : 0, $1) }
    }
    func isInternal(_ name: String) -> Bool { name.contains(".") }
    var defaultTraces: [String] { signals.filter { !isInternal($0) && Self.unit(of: $0) == "V" } }

    /// A workbook's signal such as V(/out), I(R1) or P(R15), as a vector name, if the run has it.
    func vector(forSignal signal: String) -> String? {
        guard let match = signal.firstMatch(of: /^([VIP])\((.*)\)$/) else { return nil }
        let inner = match.2.lowercased()
        let name = match.1 == "V" ? inner : match.1 == "I" ? inner + "#branch" : inner + ":power"
        return vectors[name] != nil ? name : nil
    }

    static func unit(of name: String) -> String {
        name.hasSuffix(":power") ? "W" : name.contains("#branch") ? "A" : "V"
    }

    static func display(_ name: String) -> String {
        if name.hasSuffix(":power") { return "P(\(name.dropLast(6).uppercased()))" }
        if let range = name.range(of: "#branch") { return "I(\(name[..<range.lowerBound].uppercased()))" }
        return "V(\(name))"
    }

    /// The netlist device of a schematic or board reference: "R1" is R1, or a letter added
    /// in front when the model's type differs from the reference's (RJ1 for J1).
    func element(reference: String) -> Element? {
        let name = reference.lowercased()
        return elements[name] ?? elements.values.first { $0.name.dropFirst() == name }
    }

    func currentVector(reference: String) -> String? {
        guard let element = element(reference: reference) else { return nil }
        return [element.name + "#branch"].first { vectors[$0] != nil }
            ?? names.first { $0.hasPrefix(element.name + ":") && $0.hasSuffix("#branch") }
    }

    /// The current entering the net named `node` from this device, per point: nil when the run
    /// doesn't say. A vector's current flows into the device at its first node (or its terminal).
    func injection(reference: String, node: String) -> [Double]? {
        guard let element = element(reference: reference), element.nodes.contains(node) else { return nil }
        let letter = element.name.first!
        if element.nodes.count == 2 {
            guard let current = vectors[element.name + "#branch"]?.real else { return nil }
            if element.nodes[0] == element.nodes[1] { return current.map { _ in 0 } }
            let sign = element.nodes[0] == node ? -1.0 : 1.0
            return current.map { sign * $0 }
        }
        let terminals = Array(letter == "q" ? "cbe" : letter == "j" ? "dgs" : "dgsb")
        var sum: [Double]?
        for (position, terminal) in terminals.enumerated() where position < element.nodes.count && element.nodes[position] == node {
            guard let current = vectors["\(element.name):\(terminal)#branch"]?.real else { return nil }
            sum = zip(sum ?? current.map { _ in 0 }, current).map { $0 - $1 }
        }
        return sum
    }
}

struct SimulationTrace: Identifiable {
    /// Trace colours, validated for colour-blind separation on the panel's dark surface.
    static let palette: [Color] = [Color(hex: 0x00A3B0), Color(hex: 0xBD8400), Color(hex: 0xE04F86),
                                   Color(hex: 0x8F78E8), Color(hex: 0x45A63F)]
    static let limit = palette.count
    let name: String
    let slot: Int
    var id: String { name }
    var color: Color { Self.palette[slot] }
}

func siText(_ value: Double, unit: String = "") -> String {
    guard value.isFinite else { return "–" }
    guard value != 0 else { return "0 \(unit)" }
    // Rounded first, so 0.9999 reads 1 V and not 1e+03 mV.
    let value = Double(String(format: "%.3g", value)) ?? value
    for (scale, prefix) in [(1e9, "G"), (1e6, "M"), (1e3, "k"), (1, ""), (1e-3, "m"), (1e-6, "µ"), (1e-9, "n"), (1e-12, "p")]
    where abs(value) >= scale {
        return String(format: "%.3g %@%@", value / scale, prefix, unit)
    }
    return String(format: "%.2e %@", value, unit)
}

/// The inspector's simulator controls (NATIVE_KICAD built with ngspice).
struct SimulationSection: View {
    let schematic: SchematicDesign

    private enum Analysis: String, CaseIterable, Identifiable {
        case project = "From project", transient = "Transient", ac = "AC", dc = "DC sweep", operating = "Operating point", custom = "Custom"
        var id: String { rawValue }
    }
    @State private var analysis = Analysis.project
    @State private var step = "1u"
    @State private var stop = "10m"
    @State private var perDecade = "100"
    @State private var startFrequency = "10"
    @State private var stopFrequency = "1Meg"
    @State private var source = "V1"
    @State private var sweepStart = "0"
    @State private var sweepStop = "5"
    @State private var sweepStep = "0.1"
    @State private var custom = ".tran 1u 10m"
    @State private var currents = true

    private var analysisLine: String? {
        switch analysis {
        case .project: return nil
        case .transient: return ".tran \(step) \(stop)"
        case .ac: return ".ac dec \(perDecade) \(startFrequency) \(stopFrequency)"
        case .dc: return ".dc \(source) \(sweepStart) \(sweepStop) \(sweepStep)"
        case .operating: return ".op"
        case .custom: return custom
        }
    }

    var body: some View {
        let tab = schematic.workbookTab
        InspectorSection("Simulation · KiCad") {
            Picker("Analysis", selection: $analysis) {
                ForEach(Analysis.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.menu).tint(Corv.gold)
            .accessibilityIdentifier("simulationAnalysis")
            fields(tab: tab)
            Button { run(tab: tab) } label: {
                Label(schematic.simulationRunning ? "Simulating…" : "Run simulation", systemImage: "waveform.path.ecg")
            }
            .buttonStyle(.corvPrimary)
            .disabled(schematic.simulationRunning)
            .accessibilityIdentifier("runSimulation")
            if let line = analysisLine, schematic.readOnlyReason == nil {
                Button { schematic.addText(line, x: 25.4, y: 25.4) } label: {
                    Label("Add to schematic", systemImage: "text.badge.plus")
                }
                .buttonStyle(.corvGhost)
            }
            if let run = schematic.simulation {
                StatGrid(items: [Stat("Analysis", run.analysis), Stat("Points", "\(run.points)")])
                if !schematic.showsSimulationPlot {
                    Button { schematic.showsSimulationPlot = true } label: {
                        Label("Show plot", systemImage: "chart.xyaxis.line")
                    }
                    .buttonStyle(.corvGhost)
                }
                if run.kept < run.points {
                    CautionNote(text: "Showing \(run.kept) of the \(run.points) points; fast edges can fall between them.")
                }
            }
            CautionNote(text: "KiCad 10.0.6's simulator with ngspice 46, on the current schematic. Tap a net or a part with the Select tool to plot it. Verilog-A models are not available. An edit clears the result.")
        }
    }

    @ViewBuilder private func fields(tab: (commands: String, signals: [String])?) -> some View {
        switch analysis {
        case .project:
            if let tab {
                Text(tab.commands.split(separator: "\n").filter { !$0.hasPrefix(".kicad") && !$0.hasPrefix(".save") && !$0.hasPrefix(".probe") }.joined(separator: "\n"))
                    .font(.corvMono).foregroundStyle(Corv.cyan)
                MonoLabel("First tab of the project's workbook", color: Corv.textFaint, small: true)
            } else {
                MonoLabel("The schematic's simulation directive", color: Corv.textFaint, small: true)
            }
        case .transient:
            field("Step", $step); field("Stop", $stop); currentsToggle
        case .ac:
            field("Points per decade", $perDecade); field("Start Hz", $startFrequency); field("Stop Hz", $stopFrequency); currentsToggle
        case .dc:
            field("Source", $source); field("Start", $sweepStart); field("Stop", $sweepStop); field("Step", $sweepStep); currentsToggle
        case .operating:
            currentsToggle
        case .custom:
            TextField(".tran 1u 10m", text: $custom, axis: .vertical)
                .lineLimit(1...4).autocorrectionDisabled().textInputAutocapitalization(.never).corvField()
            currentsToggle
        }
    }

    private func field(_ title: String, _ text: Binding<String>) -> some View {
        HStack {
            MonoLabel(title, color: Corv.textDim, small: true)
            Spacer()
            TextField(title, text: text).autocorrectionDisabled().textInputAutocapitalization(.never)
                .multilineTextAlignment(.trailing).frame(width: 110).corvField()
        }
    }

    private var currentsToggle: some View {
        Toggle(isOn: $currents) { MonoLabel("Save currents and power", color: Corv.textDim, small: true) }
            .tint(Corv.gold)
    }

    private func run(tab: (commands: String, signals: [String])?) {
        let command: String
        if let line = analysisLine {
            // Without option lines the session uses a new tab's defaults, which save currents and
            // power; ngspice refuses those in circuits with digital nodes.
            command = currents ? line : line + "\n.kicad adjustpaths\n.save all"
        } else {
            command = tab?.commands ?? ""
        }
        let signals = analysisLine == nil ? tab?.signals ?? [] : []
        Task { await schematic.simulate(command, signals: signals) }
    }
}

/// Covers the app during a run: KiCad's state is in use on the simulation's thread.
struct SimulationRunningOverlay: View {
    var body: some View {
        ZStack {
            Color.black.opacity(0.55).ignoresSafeArea()
            VStack(spacing: 16) {
                ProgressView().tint(Corv.cyan).controlSize(.large)
                MonoLabel("Simulating with ngspice", color: Corv.text)
                Button { _ = cc_sch_simulate_stop() } label: { Label("Stop", systemImage: "stop.fill") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("stopSimulation")
            }
            .padding(28).corvPanel(radius: 20).frame(maxWidth: 280)
        }
    }
}

/// The plot over the schematic canvas; the canvas above it stays tappable for probes.
struct SimulationPlot: View {
    @Bindable var schematic: SchematicDesign
    let run: SimulationResult
    /// Nil without a board to show it on, or for AC results.
    let onShowOnBoard: (() -> Void)?
    @State private var phase = false
    @State private var picking = false

    fileprivate struct Sample: Identifiable { let id: Int; let x, y: Double }
    fileprivate struct Series: Identifiable {
        let trace: SimulationTrace
        let samples: [Sample]
        /// Every point of the run, for the cursor.
        let xs, ys: [Double]
        var id: String { trace.name }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            header
            legend
            if let note = schematic.simulationNote {
                Text(note).font(.caption).foregroundStyle(Corv.textDim)
            }
            if run.scale == nil {
                operatingPoint
            } else {
                let panes = self.panes
                if panes.isEmpty {
                    Text("Tap a net or a part on the schematic, or add a trace.").font(.callout).foregroundStyle(Corv.textDim)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                    VStack(spacing: 8) {
                        ForEach(panes, id: \.unit) { pane in
                            SimulationPane(unit: pane.unit, series: pane.series, logarithmic: run.isAC, xUnit: run.xUnit)
                        }
                    }
                }
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity)
        .frame(height: 320)
        .corvPanel(radius: 18)
        .padding(.horizontal, 16).padding(.bottom, 12)
        .sheet(isPresented: $picking) { SimulationVectorPicker(schematic: schematic, run: run) }
    }

    private var header: some View {
        // Controls first: the inspector can cover the panel's right side.
        HStack(spacing: 10) {
            Button { schematic.showsSimulationPlot = false } label: { Image(systemName: "xmark").frame(width: 36, height: 36) }
                .buttonStyle(.plain).foregroundStyle(Corv.textDim)
                .accessibilityLabel("Hide plot")
            Button { picking = true } label: { Label("Trace", systemImage: "plus") }
                .buttonStyle(.corvGhost).fixedSize()
                .accessibilityIdentifier("addTrace")
            if let onShowOnBoard {
                Button(action: onShowOnBoard) { Label("On board", systemImage: "bolt") }
                    .buttonStyle(.corvGhost).fixedSize()
                    .accessibilityIdentifier("showOnBoard")
            }
            if run.isAC {
                Picker("Show", selection: $phase) {
                    Text("Gain").tag(false)
                    Text("Phase").tag(true)
                }
                .pickerStyle(.segmented).frame(width: 150)
            }
            MonoLabel("\(run.analysis) · \(run.points) points", color: Corv.gold, small: true)
            Spacer()
        }
    }

    private var legend: some View {
        ScrollView(.horizontal) {
            HStack(spacing: 8) {
                ForEach(schematic.simulationTraces) { trace in
                    Button { schematic.simulationTraces.removeAll { $0.name == trace.name } } label: {
                        HStack(spacing: 6) {
                            RoundedRectangle(cornerRadius: 1).fill(trace.color).frame(width: 14, height: 2)
                            Text(SimulationResult.display(trace.name)).font(.corvMono).foregroundStyle(Corv.text)
                            Image(systemName: "xmark").font(.system(size: 8, weight: .bold)).foregroundStyle(Corv.textFaint)
                        }
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .background(Color.white.opacity(0.05), in: .capsule)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Remove \(SimulationResult.display(trace.name))")
                }
            }
        }
        .scrollIndicators(.hidden)
    }

    private var operatingPoint: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 4) {
                ForEach(run.signals, id: \.self) { name in
                    HStack {
                        Text(SimulationResult.display(name)).font(.corvMono).foregroundStyle(Corv.textDim)
                        Spacer()
                        Text(siText(run.vectors[name]?.real.first ?? 0, unit: SimulationResult.unit(of: name)))
                            .font(.corvReadout).foregroundStyle(Corv.cyan)
                    }
                }
            }
        }
    }

    /// One chart per unit: no chart has two y scales.
    private var panes: [(unit: String, series: [Series])] {
        guard let scale = run.scale else { return [] }
        var units: [String] = []
        var groups: [String: [Series]] = [:]
        for trace in schematic.simulationTraces {
            guard let vector = run.vectors[trace.name] else { continue }
            let unit = run.isAC ? (phase ? "°" : "dB") : SimulationResult.unit(of: trace.name)
            let ys = values(vector)
            let series = Series(trace: trace, samples: Self.thinned(xs: scale.real, ys: ys), xs: scale.real, ys: ys)
            if groups[unit] == nil { units.append(unit) }
            groups[unit, default: []].append(series)
        }
        return units.map { ($0, groups[$0]!) }
    }

    private func values(_ vector: SimulationResult.Vector) -> [Double] {
        guard let imaginary = vector.imaginary else { return vector.real }
        return zip(vector.real, imaginary).map { re, im in
            phase ? atan2(im, re) * 180 / .pi : 20 * log10(max(hypot(re, im), 1e-30))
        }
    }

    /// The smallest and largest value of each of 300 runs of points, so peaks survive the
    /// thinning Swift Charts needs.
    private static func thinned(xs: [Double], ys: [Double]) -> [Sample] {
        let count = min(xs.count, ys.count)
        guard count > 800 else { return (0..<count).map { Sample(id: $0, x: xs[$0], y: ys[$0]) } }
        var samples: [Sample] = []
        let step = Double(count) / 300
        for bucket in 0..<300 {
            let range = Int(Double(bucket) * step)..<min(count, Int(Double(bucket + 1) * step))
            guard let low = range.min(by: { ys[$0] < ys[$1] }), let high = range.max(by: { ys[$0] < ys[$1] }) else { continue }
            for index in low == high ? [low] : [min(low, high), max(low, high)] {
                samples.append(Sample(id: index, x: xs[index], y: ys[index]))
            }
        }
        return samples
    }
}

private struct SimulationPane: View {
    let unit: String
    let series: [SimulationPlot.Series]
    let logarithmic: Bool
    let xUnit: String

    private var domain: ClosedRange<Double> {
        let xs = series.first?.xs ?? []
        let low = xs.first ?? 0, high = xs.last ?? 1
        return low < high ? low...high : low...(low + 1)
    }

    var body: some View {
        Chart {
            ForEach(series) { line in
                ForEach(line.samples) { sample in
                    LineMark(x: .value(xUnit, sample.x), y: .value(unit, sample.y), series: .value("Trace", line.id))
                        .foregroundStyle(line.trace.color)
                        .lineStyle(StrokeStyle(lineWidth: 2))
                }
            }
        }
        .chartLegend(.hidden)
        .chartXScale(domain: domain, type: logarithmic ? .log : .linear)
        .chartXAxis {
            AxisMarks(values: .automatic(desiredCount: 6)) { value in
                AxisGridLine().foregroundStyle(Corv.line)
                AxisValueLabel { if let x = value.as(Double.self) { Text(siText(x, unit: xUnit)).font(.corvMonoSmall).foregroundStyle(Corv.textFaint) } }
            }
        }
        .chartYAxis {
            AxisMarks(values: .automatic(desiredCount: 4)) { value in
                AxisGridLine().foregroundStyle(Corv.line)
                AxisValueLabel { if let y = value.as(Double.self) { Text(siText(y, unit: unit)).font(.corvMonoSmall).foregroundStyle(Corv.textFaint) } }
            }
        }
        .chartOverlay { proxy in SimulationCursor(proxy: proxy, series: series, unit: unit, xUnit: xUnit) }
    }
}

/// The drag cursor. It keeps its own state so moving it doesn't rebuild the chart's marks.
private struct SimulationCursor: View {
    let proxy: ChartProxy
    let series: [SimulationPlot.Series]
    let unit: String
    let xUnit: String
    @State private var x: Double?

    var body: some View {
        GeometryReader { geometry in
            let plot = proxy.plotFrame.map { geometry[$0] } ?? .zero
            ZStack(alignment: .topLeading) {
                Color.clear.contentShape(.rect)
                    .gesture(DragGesture(minimumDistance: 0).onChanged { drag in
                        x = proxy.value(atX: drag.location.x - plot.minX, as: Double.self)
                    })
                if let x, let position = proxy.position(forX: x) {
                    Rectangle().fill(Corv.gold).frame(width: 1, height: plot.height)
                        .offset(x: plot.minX + position, y: plot.minY)
                        .allowsHitTesting(false)
                    readout(at: x)
                        .offset(x: plot.minX + 8, y: plot.minY + 4)
                        .allowsHitTesting(false)
                }
            }
        }
    }

    private func readout(at x: Double) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(siText(x, unit: xUnit)).font(.corvMonoSmall).foregroundStyle(Corv.gold)
            ForEach(series) { line in
                HStack(spacing: 5) {
                    RoundedRectangle(cornerRadius: 1).fill(line.trace.color).frame(width: 8, height: 2)
                    Text(siText(value(of: line, at: x), unit: unit)).font(.corvMonoSmall).foregroundStyle(Corv.text)
                }
            }
        }
        .padding(6)
        .background(Corv.void.opacity(0.85), in: .rect(cornerRadius: 6))
    }

    private func value(of line: SimulationPlot.Series, at x: Double) -> Double {
        var low = 0, high = line.xs.count - 1
        guard high >= 0 else { return 0 }
        while low < high {
            let middle = (low + high) / 2
            if line.xs[middle] < x { low = middle + 1 } else { high = middle }
        }
        return line.ys[min(low, line.ys.count - 1)]
    }
}

/// All the run's vectors, searchable: a circuit can have hundreds.
private struct SimulationVectorPicker: View {
    let schematic: SchematicDesign
    let run: SimulationResult
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""

    var body: some View {
        let needle = query.lowercased()
        let matches = run.signals.filter { needle.isEmpty || $0.contains(needle) }.prefix(300)
        NavigationStack {
            List(matches, id: \.self) { name in
                Button {
                    schematic.addTrace(name)
                    dismiss()
                } label: {
                    HStack {
                        Text(SimulationResult.display(name)).font(.corvMono).foregroundStyle(Corv.text)
                        Spacer()
                        if schematic.simulationTraces.contains(where: { $0.name == name }) {
                            Image(systemName: "checkmark").foregroundStyle(Corv.gold)
                        }
                    }
                }
                .listRowBackground(Corv.space)
            }
            .scrollContentBackground(.hidden)
            .background(Corv.void)
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Net or part")
            .navigationTitle("Add trace")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } } }
        }
        .preferredColorScheme(.dark)
    }
}

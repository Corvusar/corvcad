import SwiftUI

/// One KiCad DRC finding from DRC_REPORT's JSON, located at its first item (board millimetres).
struct DRCFinding: Identifiable {
    let id: Int
    let type: String
    let description: String
    let severity: String
    let detail: String
    let location: CGPoint?
    var isError: Bool { severity == "error" }
}

/// One unrouted connection of KiCad's ratsnest (cc_board_native_ratsnest), board millimetres.
struct RatsnestEdge {
    let start: CGPoint
    let end: CGPoint
    let net: String

    /// Empty in the compatible edition or when KiCad cannot load the file.
    /// KiCad keeps the board open between calls (board session) and reloads it only after the
    /// file changed, so the ratsnest, net rules and DRC after an edit share one load.
    static func load(board url: URL) -> [RatsnestEdge] {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_board_native_ratsnest(url.path, &buffer, capacity) else { return [] }
        defer { cc_free_text(text) }
        return String(cString: text).split(separator: "\n").compactMap { line in
            let fields = line.split(separator: " ", maxSplits: 4)
            guard fields.count == 5, let x0 = Double(fields[0]), let y0 = Double(fields[1]),
                  let x1 = Double(fields[2]), let y1 = Double(fields[3]) else { return nil }
            return RatsnestEdge(start: CGPoint(x: x0, y: y0), end: CGPoint(x: x1, y: y1), net: String(fields[4]))
        }
    }
}

/// KiCad's effective netclass sizes for a net (cc_board_native_net_rules), millimetres.
struct NetRule {
    let track: Double
    let via: Double
    let drill: Double
    let clearance: Double

    /// Net name -> rule; "" is the Default netclass. Empty in the compatible edition.
    static func load(board url: URL) -> [String: NetRule] {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_board_native_net_rules(url.path, &buffer, capacity) else { return [:] }
        defer { cc_free_text(text) }
        var rules: [String: NetRule] = [:]
        for line in String(cString: text).split(separator: "\n", omittingEmptySubsequences: false) {
            let fields = line.split(separator: " ", maxSplits: 4, omittingEmptySubsequences: false)
            guard fields.count >= 4, let track = Double(fields[0]), let via = Double(fields[1]),
                  let drill = Double(fields[2]), let clearance = Double(fields[3]) else { continue }
            rules[fields.count == 5 ? String(fields[4]) : ""] = NetRule(track: track, via: via, drill: drill, clearance: clearance)
        }
        return rules
    }
}

struct DRCResult {
    let findings: [DRCFinding]
    let refilled: Bool
    let seconds: Double
    var errors: Int { findings.filter(\.isError).count }
    var warnings: Int { findings.count - errors }

    /// Violations and unconnected items, as kicad-cli's JSON report lists them.
    init(json: String, refilled: Bool, seconds: Double) throws {
        struct Report: Decodable {
            struct Position: Decodable { let x, y: Double }
            struct Item: Decodable { let description: String; let pos: Position? }
            struct Entry: Decodable { let type, description, severity: String; let items: [Item] }
            let violations: [Entry]?
            let unconnected_items: [Entry]?
        }
        let report = try JSONDecoder().decode(Report.self, from: Data(json.utf8))
        let entries = (report.violations ?? []) + (report.unconnected_items ?? [])
        findings = entries.enumerated().map { index, entry in
            DRCFinding(id: index, type: entry.type, description: entry.description, severity: entry.severity,
                       detail: entry.items.map(\.description).joined(separator: "\n"),
                       location: entry.items.first?.pos.map { CGPoint(x: $0.x, y: $0.y) })
        }
        self.refilled = refilled
        self.seconds = seconds
    }
}

#if DEBUG
extension DRCResult {
    /// Screenshot fixture (CORVCAD_DRC_FIXTURE): a KiCad-format report with an error and a
    /// warning at the first footprints, parsed like a real one. Lets the compatible edition's
    /// simulator build show the DRC views, which otherwise need KiCad's board code.
    static func fixture(at points: [CGPoint]) -> DRCResult? {
        let entries = zip(points.prefix(2), [("clearance", "error", "Clearance violation (netclass 'Default' clearance 0.2000 mm; actual 0.1250 mm)"),
                                             ("silk_over_copper", "warning", "Silkscreen clipped by solder mask")])
            .map { point, kind in
                """
                {"type":"\(kind.0)","severity":"\(kind.1)","description":"\(kind.2)",\
                "items":[{"description":"Pad 1 of R1 on F.Cu","pos":{"x":\(point.x),"y":\(point.y)}}]}
                """
            }
        return try? DRCResult(json: "{\"violations\":[\(entries.joined(separator: ","))]}", refilled: false, seconds: 0.4)
    }
}
#endif

extension Design {
    /// True when the edition links KiCad's board code (NATIVE_KICAD), or in a debug build
    /// showing the screenshot fixture.
    static let hasKiCadDRC: Bool = {
        #if DEBUG
        if ProcessInfo.processInfo.environment["CORVCAD_DRC_FIXTURE"] != nil { return true }
        #endif
        return cc_board_native_backend() != nil
    }()

    /// Runs KiCad's DRC on the saved board file. The file is never changed, a refill included.
    @MainActor
    func runDRC(on board: URL, refillZones: Bool) async {
        drcRunning = true
        drcError = nil
        // Let "Checking" draw before the call blocks.
        try? await Task.sleep(for: .milliseconds(60))
        // ponytail: runs on the main thread because KiCad's program state is shared with the
        // schematic session's main-thread calls; a large board (the 81 MB Jetson demo: ~17 s on
        // an M2 iPad) freezes the UI meanwhile. Move every KiCad call to one serial executor
        // if that matters.
        let start = Date()
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = cc_board_native_drc(board.path, refillZones ? 1 : 0, &buffer, capacity)
        let seconds = Date().timeIntervalSince(start)
        drcRunning = false
        guard let text else {
            drcError = String(cString: buffer)
            return
        }
        defer { cc_free_text(text) }
        do {
            drc = try DRCResult(json: String(cString: text), refilled: refillZones, seconds: seconds)
        } catch {
            drcError = "Unreadable DRC report: \(error.localizedDescription)"
        }
    }

    /// KiCad's Update PCB from Schematic with the session's netlist: missing footprints from the
    /// libraries, pad nets, values and fields; nothing is deleted. Saved as one undoable step
    /// unless nothing changed. Returns KiCad's report, or nil with `error` set.
    @MainActor
    func updateFromNetlist(_ netlist: String, board url: URL) -> String? {
        var buffer = [CChar](repeating: 0, count: 512)
        var report = [CChar](repeating: 0, count: 16384)
        let capacity = buffer.count, reportCapacity = report.count
        let text = netlist.withCString {
            cc_board_native_update_from_netlist(url.path, $0, &report, reportCapacity, &buffer, capacity)
        }
        guard let text else { error = String(cString: buffer); return nil }
        if String(cString: text) == boardSource {
            cc_free_text(text)
        } else {
            saveAuthored(text, errorBuffer: buffer, to: url)
        }
        return String(cString: report)
    }

    /// Refills every zone with KiCad's zone filler and saves the board through KiCad's writer,
    /// as one undoable step. Teardrops are regenerated first, as kicad-cli does.
    @MainActor
    func fillZones(of board: URL) async {
        filling = true
        drcError = nil
        try? await Task.sleep(for: .milliseconds(60))
        // ponytail: main thread, as runDRC (KiCad's state is shared with the schematic session).
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = cc_board_native_fill_zones(board.path, &buffer, capacity)
        filling = false
        saveAuthored(text, errorBuffer: buffer, to: board)
    }

    /// KiCad's fabrication outputs (Gerbers with the job file, Excellon drill, placement CSV) of
    /// the saved board, zipped in the temporary directory for the share sheet.
    @MainActor
    func exportFabrication(of board: URL, refillZones: Bool) async {
        exportingFab = true
        drcError = nil
        fabZipURL = nil
        try? await Task.sleep(for: .milliseconds(60))
        defer { exportingFab = false }
        let name = board.deletingPathExtension().lastPathComponent
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("\(name)-fab", isDirectory: true)
        try? FileManager.default.removeItem(at: folder)
        // ponytail: main thread, as runDRC (KiCad's state is shared with the schematic session).
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_board_native_fab_outputs(board.path, folder.path, refillZones ? 1 : 0, &buffer, capacity) else {
            drcError = String(cString: buffer)
            return
        }
        cc_free_text(text)
        // NSFileCoordinator zips a folder read "for uploading"; the zip lives only inside the block.
        let zip = FileManager.default.temporaryDirectory.appendingPathComponent("\(name)-fab.zip")
        var coordinationError: NSError?
        var copyError: Error?
        NSFileCoordinator().coordinate(readingItemAt: folder, options: .forUploading, error: &coordinationError) { zipped in
            do {
                try? FileManager.default.removeItem(at: zip)
                try FileManager.default.copyItem(at: zipped, to: zip)
            } catch {
                copyError = error
            }
        }
        if let failure = coordinationError ?? copyError {
            drcError = "Cannot zip the fabrication files: \(failure.localizedDescription)"
            return
        }
        fabZipURL = zip
    }
}

/// KiCad's Board Setup as cc_board_native_board_setup writes it (tab-separated, millimetres).
struct BoardSetup: Equatable {
    struct NetClass: Equatable, Identifiable {
        var id = UUID()
        var name: String
        var clearance, track, via, drill, pairWidth, pairGap: Double
    }
    struct Assignment: Equatable, Identifiable {
        var id = UUID()
        var pattern: String
        var netClass: String
    }
    /// A dielectric layer of the physical stack-up, in board order (index is KiCad's).
    struct Dielectric: Equatable, Identifiable {
        var id: Int
        var kind: String
        var thickness: Double
        var material: String
        var epsilon: Double
    }
    var copperLayers = 2
    var dielectrics: [Dielectric] = []
    /// Rule name and value, in the engine's order.
    var rules: [(name: String, value: Double)] = []
    /// The Default netclass first.
    var classes: [NetClass] = []
    var assignments: [Assignment] = []

    static func == (a: BoardSetup, b: BoardSetup) -> Bool {
        a.copperLayers == b.copperLayers && a.rules.map(\.name) == b.rules.map(\.name)
            && a.rules.map(\.value) == b.rules.map(\.value) && a.classes == b.classes && a.assignments == b.assignments
            && a.dielectrics == b.dielectrics
    }

    static let ruleTitles = [
        "min_clearance": "Min clearance", "min_track_width": "Min track width",
        "min_annular_width": "Min annular ring", "min_via_diameter": "Min via diameter",
        "min_through_hole": "Min through hole", "copper_edge_clearance": "Copper to edge",
        "hole_clearance": "Hole to copper", "hole_to_hole": "Hole to hole",
    ]

    init() {}

    init(text: String) {
        for line in text.split(separator: "\n") {
            let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            switch (f.first, f.count) {
            case ("layers", 2): copperLayers = Int(f[1]) ?? 2
            case ("rule", 3): rules.append((f[1], Double(f[2]) ?? 0))
            case ("class", 8):
                let v = f[2...].map { Double($0) ?? 0 }
                classes.append(NetClass(name: f[1], clearance: v[0], track: v[1], via: v[2], drill: v[3],
                                        pairWidth: v[4], pairGap: v[5]))
            case ("assign", 3): assignments.append(Assignment(pattern: f[1], netClass: f[2]))
            case ("dielectric", 6):
                dielectrics.append(Dielectric(id: Int(f[1]) ?? dielectrics.count, kind: f[2], thickness: Double(f[3]) ?? 0,
                                              material: f[4], epsilon: Double(f[5]) ?? 1))
            default: break
            }
        }
    }

    var text: String {
        let number = { (value: Double) in String(format: "%g", value) }
        var lines = ["layers\t\(copperLayers)"] + rules.map { "rule\t\($0.name)\t\(number($0.value))" }
        lines += classes.map { c in
            (["class", c.name] + [c.clearance, c.track, c.via, c.drill, c.pairWidth, c.pairGap].map(number))
                .joined(separator: "\t")
        }
        lines += assignments.filter { !$0.pattern.isEmpty }.map { "assign\t\($0.pattern)\t\($0.netClass)" }
        lines += dielectrics.map { "dielectric\t\($0.id)\t\($0.kind)\t\(number($0.thickness))\t\($0.material)\t\(number($0.epsilon))" }
        return lines.joined(separator: "\n") + "\n"
    }
}

extension Design {
    /// The board's Board Setup from KiCad, or nil (error set) when KiCad cannot read it.
    @MainActor
    func boardSetup(of board: URL) -> BoardSetup? {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_board_native_board_setup(board.path, &buffer, capacity) else {
            drcError = String(cString: buffer)
            return nil
        }
        defer { cc_free_text(text) }
        return BoardSetup(text: String(cString: text))
    }

    /// Applies a Board Setup through KiCad and writes the board and its .kicad_pro (where the
    /// rules and netclasses live) as one undoable step.
    @MainActor
    func applyBoardSetup(_ setup: BoardSetup, to board: URL) {
        drcError = nil
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        var projectText: UnsafeMutablePointer<CChar>?
        guard let text = cc_board_native_apply_board_setup(board.path, setup.text, &projectText, &buffer, capacity) else {
            drcError = String(cString: buffer)
            return
        }
        defer { cc_free_text(text); cc_free_text(projectText) }
        let project = board.deletingPathExtension().appendingPathExtension("kicad_pro")
        let projectBefore = (try? String(contentsOf: project, encoding: .utf8)) ?? ""
        let boardAfter = String(cString: text), projectAfter = projectText.map { String(cString: $0) } ?? ""
        do {
            try Data(projectAfter.utf8).write(to: project, options: .atomic)
            try Data(boardAfter.utf8).write(to: board, options: .atomic)
            onCommitFiles?([.init(url: board, before: boardSource, after: boardAfter),
                            .init(url: project, before: projectBefore, after: projectAfter)])
            load(board)
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// KiCad's Plot of the board's plot layers: one PDF (a page per layer) or one SVG per layer,
    /// zipped when there are several files.
    @MainActor
    func plotBoard(_ board: URL, pdf: Bool) async -> URL? {
        drcError = nil
        try? await Task.sleep(for: .milliseconds(60))
        let name = board.deletingPathExtension().lastPathComponent
        let folder = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(name)-\(pdf ? "pdf" : "svg")", isDirectory: true)
        try? FileManager.default.removeItem(at: folder)
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_board_native_plot(board.path, folder.path, pdf ? 0 : 1, &buffer, capacity) else {
            drcError = String(cString: buffer)
            return nil
        }
        let files = String(cString: text).split(separator: "\n").map(String.init)
        cc_free_text(text)
        if pdf, let file = files.first { return folder.appendingPathComponent(file) }
        do {
            return try FileZip.zip(folder, name: "\(name)-svg")
        } catch {
            drcError = "Cannot zip the SVG files: \(error.localizedDescription)"
            return nil
        }
    }
}

/// KiCad's Board Setup for a project board (NATIVE_KICAD): copper layers, design rules,
/// netclasses and their pattern assignments.
struct BoardSetupSection: View {
    let design: Design
    let board: URL
    @State private var setup: BoardSetup?
    @State private var original: BoardSetup?

    var body: some View {
        InspectorSection("Board Setup · KiCad") {
            if let edited = setup {
                Picker("Copper layers", selection: Binding(get: { setup?.copperLayers ?? 2 },
                                                           set: { setup?.copperLayers = $0 })) {
                    ForEach([2, 4, 6, 8], id: \.self) { Text("\($0) copper layers").tag($0) }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                MonoLabel("Design rules · mm", color: Corv.textFaint, small: true)
                LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 10) {
                    ForEach(edited.rules.indices, id: \.self) { index in
                        NumberField(title: BoardSetup.ruleTitles[edited.rules[index].name] ?? edited.rules[index].name,
                                    value: Binding(get: { setup?.rules[index].value ?? 0 },
                                                   set: { setup?.rules[index].value = $0 }))
                    }
                }
                ForEach($setup.unwrapped.classes) { $netClass in
                    netClassEditor($netClass, removable: netClass.name != "Default")
                }
                Button { setup?.classes.append(.init(name: "Class\(edited.classes.count)", clearance: 0.2, track: 0.25,
                                                      via: 0.6, drill: 0.3, pairWidth: 0.2, pairGap: 0.25)) } label: {
                    Label("Add netclass", systemImage: "plus")
                }
                .buttonStyle(.corvGhost)
                MonoLabel("Net patterns → netclass", color: Corv.textFaint, small: true)
                ForEach($setup.unwrapped.assignments) { $assignment in
                    HStack(spacing: 8) {
                        TextField("*GND*", text: $assignment.pattern)
                            .textInputAutocapitalization(.never).autocorrectionDisabled().corvField()
                        Picker("Netclass", selection: $assignment.netClass) {
                            ForEach(edited.classes.map(\.name), id: \.self) { Text($0).tag($0) }
                        }
                        .pickerStyle(.menu).tint(Corv.gold)
                        Button { setup?.assignments.removeAll { $0.id == assignment.id } } label: {
                            Image(systemName: "minus.circle")
                        }
                        .buttonStyle(.plain).foregroundStyle(Corv.red)
                        .accessibilityLabel("Remove pattern")
                    }
                }
                Button { setup?.assignments.append(.init(pattern: "", netClass: "Default")) } label: {
                    Label("Add pattern", systemImage: "plus")
                }
                .buttonStyle(.corvGhost)
                if !edited.dielectrics.isEmpty {
                    MonoLabel("Stack-up · dielectric layers", color: Corv.textFaint, small: true)
                    ForEach($setup.unwrapped.dielectrics) { $layer in
                        VStack(alignment: .leading, spacing: 8) {
                            MonoLabel("\(layer.kind) \(layer.id + 1)", color: Corv.cyan, small: true)
                            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                                NumberField(title: "Thickness", value: $layer.thickness)
                                VStack(alignment: .leading, spacing: 5) {
                                    MonoLabel("Material", color: Corv.textFaint, small: true)
                                    TextField("FR4", text: $layer.material).autocorrectionDisabled().corvField()
                                }
                                NumberField(title: "Er", value: $layer.epsilon)
                            }
                        }
                        .padding(10)
                        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
                        .accessibilityIdentifier("dielectric-\(layer.id + 1)")
                    }
                    if edited.copperLayers != original?.copperLayers {
                        CautionNote(text: "Apply the new layer count first; then edit its dielectric layers.")
                    }
                }
                Button {
                    if let setup { design.applyBoardSetup(setup, to: board) }
                    reload()
                } label: { Label("Apply Board Setup", systemImage: "checkmark") }
                    .buttonStyle(.corvPrimary)
                    .disabled(setup == original)
                    .accessibilityIdentifier("applyBoardSetup")
                CautionNote(text: "KiCad 10.0.6 Board Setup: rules and netclasses are saved in the .kicad_pro, layers and the stack-up in the board (undoable). DRC and the router use them.")
            } else {
                Button { reload() } label: { Label("Edit Board Setup", systemImage: "slider.horizontal.3") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("editBoardSetup")
            }
        }
    }

    private func reload() {
        setup = design.boardSetup(of: board)
        original = setup
    }

    private func netClassEditor(_ netClass: Binding<BoardSetup.NetClass>, removable: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                if removable {
                    TextField("Name", text: netClass.name).autocorrectionDisabled().corvField()
                    Button { setup?.classes.removeAll { $0.id == netClass.wrappedValue.id } } label: {
                        Image(systemName: "trash")
                    }
                    .buttonStyle(.plain).foregroundStyle(Corv.red)
                    .accessibilityLabel("Remove netclass")
                } else {
                    MonoLabel("Default netclass", color: Corv.gold, small: true)
                }
            }
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                NumberField(title: "Clearance", value: netClass.clearance)
                NumberField(title: "Track", value: netClass.track)
                NumberField(title: "Via", value: netClass.via)
                NumberField(title: "Drill", value: netClass.drill)
                NumberField(title: "Pair width", value: netClass.pairWidth)
                NumberField(title: "Pair gap", value: netClass.pairGap)
            }
        }
        .padding(10)
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
    }
}

private extension Optional where Wrapped == BoardSetup {
    /// For ForEach bindings into a loaded setup; reads an empty one before loading.
    var unwrapped: BoardSetup {
        get { self ?? BoardSetup() }
        set { self = newValue }
    }
}

/// KiCad's fabrication outputs for a project board, shown only in the NATIVE_KICAD edition.
struct FabricationSection: View {
    let design: Design
    let board: URL
    @State private var refill = false
    @State private var plotURL: URL?
    @State private var viewing = false

    var body: some View {
        InspectorSection("Fabrication · KiCad") {
            if design.hasZones {
                Toggle(isOn: $refill) { MonoLabel("Refill zones first", small: true) }
                    .tint(Corv.gold)
            }
            Button {
                Task { await design.exportFabrication(of: board, refillZones: refill) }
            } label: {
                Label(design.exportingFab ? "Exporting…" : "Export Gerbers and drill", systemImage: "shippingbox")
            }
            .buttonStyle(.corvGhost)
            .disabled(design.exportingFab || design.drcRunning || design.filling)
            .accessibilityIdentifier("exportFabrication")
            if let url = design.fabZipURL {
                HStack(spacing: 10) {
                    ShareLink("Share \(url.lastPathComponent)", item: url).foregroundStyle(Corv.gold)
                    // The zip's folder (exportFabrication keeps it beside the zip).
                    Button { viewing = true } label: { Label("View", systemImage: "square.3.layers.3d.down.right") }
                        .buttonStyle(.corvGhost)
                        .accessibilityIdentifier("viewGerbers")
                }
            }
            HStack(spacing: 8) {
                Button { Task { plotURL = await design.plotBoard(board, pdf: true) } } label: {
                    Label("Plot PDF", systemImage: "doc.richtext")
                }
                .accessibilityIdentifier("plotPDF")
                Button { Task { plotURL = await design.plotBoard(board, pdf: false) } } label: {
                    Label("Plot SVG", systemImage: "square.on.square.dashed")
                }
            }
            .buttonStyle(.corvGhost)
            .disabled(design.exportingFab || design.drcRunning || design.filling)
            if let plotURL {
                ShareLink("Share \(plotURL.lastPathComponent)", item: plotURL).foregroundStyle(Corv.gold)
            }
            CautionNote(text: "KiCad 10.0.6 plotters: Gerbers of the board's plot settings with a job file, Excellon drill (mm, PTH and NPTH apart) and a placement CSV. Run DRC first; a refill is not saved. PDF and SVG plot the same layers.")
        }
        .fullScreenCover(isPresented: $viewing) {
            if let url = design.fabZipURL { GerberViewer(initial: [url.deletingPathExtension()]) }
        }
    }
}

/// KiCad's design rule check for a project board, shown only in the NATIVE_KICAD edition.
struct BoardCheckSection: View {
    let design: Design
    let board: URL
    @State private var refill = false

    var body: some View {
        InspectorSection("Design rules · KiCad") {
            Toggle(isOn: $refill) { MonoLabel("Refill zones first", small: true) }
                .tint(Corv.gold)
            Button {
                Task { await design.runDRC(on: board, refillZones: refill) }
            } label: {
                Label(design.drcRunning ? "Checking…" : "Run DRC", systemImage: "checkmark.shield")
            }
            .buttonStyle(.corvPrimary)
            .disabled(design.drcRunning || design.filling)
            .accessibilityIdentifier("runDRC")
            if design.hasZones {
                Button {
                    Task { await design.fillZones(of: board) }
                } label: {
                    Label(design.filling ? "Filling…" : "Refill zones", systemImage: "square.stack.3d.down.forward")
                }
                .buttonStyle(.corvGhost)
                .disabled(design.drcRunning || design.filling)
                .accessibilityIdentifier("fillZones")
            }
            if let error = design.drcError { CautionNote(text: error) }
            if let result = design.drc { results(result) }
            CautionNote(text: "KiCad 10.0.6 DRC without schematic parity; it never changes the board, a refill first included. Refill zones saves the board (undoable).")
        }
    }

    @ViewBuilder private func results(_ result: DRCResult) -> some View {
        StatGrid(items: [Stat("Errors", "\(result.errors)"), Stat("Warnings", "\(result.warnings)")])
        MonoLabel(String(format: "%.1f s", result.seconds) + (result.refilled ? " · zones refilled" : ""),
                  color: Corv.textFaint, small: true)
        // ponytail: first 200 findings; a filter by type when boards with thousands matter.
        ForEach(result.findings.prefix(200)) { finding in
            Button { design.selectedFinding = design.selectedFinding == finding.id ? nil : finding.id } label: {
                FindingRow(finding: finding, selected: design.selectedFinding == finding.id)
            }
            .buttonStyle(.plain)
        }
        if result.findings.count > 200 {
            MonoLabel("\(result.findings.count - 200) more", color: Corv.textFaint, small: true)
        }
    }
}

private struct FindingRow: View {
    let finding: DRCFinding
    let selected: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Circle().fill(finding.isError ? Corv.red : Corv.gold.opacity(0.7))
                .frame(width: 7, height: 7).padding(.top, 5)
            VStack(alignment: .leading, spacing: 3) {
                Text(finding.description).font(.caption).foregroundStyle(Corv.text)
                if selected && !finding.detail.isEmpty {
                    Text(finding.detail).font(.corvMonoSmall).foregroundStyle(Corv.textDim)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(8)
        .background(selected ? Corv.gold.opacity(0.08) : .clear, in: .rect(cornerRadius: 8))
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(finding.isError ? "Error" : "Warning"): \(finding.description)")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

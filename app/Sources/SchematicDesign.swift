import SwiftUI

func schematicText<T>(_ value: T) -> String {
    var copy = value
    return withUnsafeBytes(of: &copy) { bytes in
        String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
    }
}

/// One step of the sheet breadcrumb: a KiCad sheet-instance path and its display name.
struct SheetCrumb: Hashable {
    let path: String
    let name: String
}

/// Schematic state for the canvas. Local projects run through the engine's transactional
/// schematic session (hierarchy, grouped edits, safe saves); an individually imported
/// sheet is read-only and its inspector edits save copies.
@Observable
final class SchematicDesign {
    /// A net on the displayed sheet from the engine's portable connectivity (not KiCad's):
    /// wire and label indices and pin connection points in millimetres.
    struct SheetNet {
        let name: String
        let wires: Set<Int>
        let labels: Set<Int>
        let pins: [CGPoint]
        /// "<reference>/<pin number>" of each pin, to find the net's name in KiCad's netlist.
        let pinKeys: [String]
    }

    /// A placed symbol's body shapes and pin lines from the sheet's lib_symbols (millimetres).
    struct SymbolDrawing {
        struct Shape {
            let points: [CGPoint]
            let closed: Bool
            /// KiCad's fill: 0 none, 1 stroke colour, 2 background.
            let fill: Int
        }
        let shapes: [Shape]
        let pins: [cc_sch_symbol_pin]
        /// Field anchors from the sheet and the definition's pin name and number settings.
        let text: cc_sch_symbol_text
        /// Around the shapes and pin lines; nil without a definition.
        let box: CGRect?
    }

    var name = "No schematic loaded"
    var nets: [SheetNet] = []
    var symbols: [cc_sch_symbol] = []
    /// Parallel to `symbols`.
    var drawings: [SymbolDrawing] = []
    /// The sheet's paper size and title block, for the page frame.
    var page = cc_sch_page()
    var wires: [cc_sch_wire] = []
    var junctions: [cc_sch_junction] = []
    var labels: [cc_sch_label] = []
    var noConnects: [cc_sch_junction] = []
    /// Text notes and graphic note lines on the displayed sheet (no net).
    var texts: [cc_sch_label] = []
    var noteLines: [cc_sch_wire] = []
    /// Child-sheet boxes on the displayed sheet, and each box's pins (parallel to `sheets`).
    var sheets: [cc_sch_sheet] = []
    var sheetPins: [[cc_sch_label]] = []
    var buses: [cc_sch_wire] = []
    var busEntries: [cc_sch_wire] = []
    /// Breadcrumb from the root sheet to the displayed one (session mode only).
    private(set) var trail: [SheetCrumb] = []
    /// Set when the project can be displayed but not safely written by this backend.
    private(set) var readOnlyReason: String?
    /// True when the session is KiCad's own (sideloaded edition), not the compatible editor.
    private(set) var nativeKiCad = false
    var selectedSymbol = 0
    var editX = 0.0
    var editY = 0.0
    var editValue = ""
    var editFootprint = ""
    var editedURL: URL?
    var error: String?
    /// Called after each committed and saved session edit, for the app-wide undo stack.
    @ObservationIgnored var onSessionCommit: (() -> Void)?
    private var source = ""
    private var handle: OpaquePointer?
    private var session: OpaquePointer?
    private var instances: [Snapshot.Sheet] = []

    private struct Snapshot: Decodable {
        struct Sheet: Decodable {
            let path: String
            let name: String
            let sharedFile: Bool
            let unsupportedConstructs: [String]
        }
        let sheets: [Sheet]
    }

    var loaded: Bool { handle != nil }
    var sessionBacked: Bool { session != nil }

    /// KiCad's netlist of the open project (NATIVE_KICAD sessions only), for KiCad's own
    /// Update PCB from Schematic. Nil for the compatible session.
    func kicadNetlist() -> String? {
        guard let session, cc_sch_session_capabilities(session) & CC_SCH_NETLIST.rawValue != 0 else { return nil }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let text = cc_sch_session_netlist(session, &result) else { return nil }
        defer { cc_free_text(text) }
        return String(cString: text)
    }
    var sheetPath: String? { trail.last?.path }
    var hasHierarchy: Bool { instances.count > 1 }

    deinit {
        if let handle { cc_schematic_free(handle) }
        if let session { cc_sch_session_free(session) }
    }

    // MARK: Opening

    /// Opens a local project's schematic hierarchy through the engine session.
    func openProject(_ project: URL) {
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        // The previous session goes first: with none open, KiCad makes this project its active
        // one, whose library tables every library lookup uses. A failed open leaves none open.
        if let session {
            cc_sch_session_free(session)
            self.session = nil
            instances = []
            trail = []
        }
        guard let next = cc_sch_session_open(project.path, &result) else {
            error = message(result)
            return
        }
        guard let json = cc_sch_session_snapshot_json(next, &result) else {
            error = message(result)
            cc_sch_session_free(next)
            return
        }
        defer { cc_free_text(json) }
        do {
            let snapshot = try JSONDecoder().decode(Snapshot.self, from: Data(String(cString: json).utf8))
            guard let root = snapshot.sheets.first else { throw DesignError.message("The project has no schematic sheet.") }
            session = next
            instances = snapshot.sheets
            nativeKiCad = cc_sch_session_capabilities(next) & CC_SCH_NATIVE_KICAD.rawValue != 0
            readOnlyReason = cc_sch_session_validate_structure(next, &result) == 1 ? nil : message(result)
            editedURL = nil
            erc = nil
            clearSimulation()
            projectFile = project
            trail = [SheetCrumb(path: root.path, name: root.name)]
            refresh()
        } catch {
            cc_sch_session_free(next)
            self.error = error.localizedDescription
        }
    }

    /// Loads one imported sheet for read-only viewing.
    func load(_ url: URL) {
        guard url.pathExtension.lowercased() == "kicad_sch" else {
            error = "Choose a KiCad .kicad_sch file."
            return
        }
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        do {
            let text = try String(contentsOf: url, encoding: .utf8)
            if let session { cc_sch_session_free(session) }
            session = nil
            instances = []
            trail = []
            readOnlyReason = nil
            if show(text) { name = url.lastPathComponent; editedURL = nil }
        } catch { self.error = error.localizedDescription }
    }

    /// Parses `text` into the displayed arrays; false (with `error` set) if it fails.
    @discardableResult private func show(_ text: String) -> Bool {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let next = text.withCString({ cc_schematic_parse($0, &buffer, capacity) }) else {
            error = String(cString: buffer)
            return false
        }
        if let handle { cc_schematic_free(handle) }
        handle = next
        source = text
        symbols = (0..<cc_sch_symbol_count(next)).map { cc_sch_symbol_at(next, $0) }
        drawings = symbols.indices.map { symbol in
            let shapes = (0..<cc_sch_symbol_graphic_count(next, symbol)).map { index in
                let graphic = cc_sch_symbol_graphic_at(next, symbol, index)
                let points = (0..<graphic.point_count).map { point in
                    var x = 0.0, y = 0.0
                    cc_sch_symbol_graphic_point(next, symbol, index, point, &x, &y)
                    return CGPoint(x: x, y: y)
                }
                return SymbolDrawing.Shape(points: points, closed: graphic.closed != 0, fill: Int(graphic.fill))
            }
            let pins = (0..<cc_sch_symbol_pin_count(next, symbol)).map { cc_sch_symbol_pin_at(next, symbol, $0) }
            let corners = shapes.flatMap(\.points) + pins.flatMap { [CGPoint(x: $0.x, y: $0.y), CGPoint(x: $0.end_x, y: $0.end_y)] }
            let box = corners.isEmpty ? nil : corners.dropFirst().reduce(CGRect(origin: corners[0], size: .zero)) {
                $0.union(CGRect(origin: $1, size: .zero))
            }
            return SymbolDrawing(shapes: shapes, pins: pins, text: cc_sch_symbol_text_at(next, symbol), box: box)
        }
        page = cc_sch_page_info(next)
        wires = (0..<cc_sch_wire_count(next)).map { cc_sch_wire_at(next, $0) }
        junctions = (0..<cc_sch_junction_count(next)).map { cc_sch_junction_at(next, $0) }
        labels = (0..<cc_sch_label_count(next)).map { cc_sch_label_at(next, $0) }
        noConnects = (0..<cc_sch_no_connect_count(next)).map { cc_sch_no_connect_at(next, $0) }
        texts = (0..<cc_sch_text_count(next)).map { cc_sch_text_at(next, $0) }
        noteLines = (0..<cc_sch_note_line_count(next)).map { cc_sch_note_line_at(next, $0) }
        sheets = (0..<cc_sch_sheet_count(next)).map { cc_sch_sheet_at(next, $0) }
        sheetPins = sheets.indices.map { sheet in
            (0..<cc_sch_sheet_pin_count(next, sheet)).map { cc_sch_sheet_pin_at(next, sheet, $0) }
        }
        buses = (0..<cc_sch_bus_count(next)).map { cc_sch_bus_at(next, $0) }
        busEntries = (0..<cc_sch_bus_entry_count(next)).map { cc_sch_bus_entry_at(next, $0) }
        let wireNets = wires.indices.map { cc_sch_wire_net(next, $0) }
        let labelNets = labels.indices.map { cc_sch_label_net(next, $0) }
        nets = (0..<cc_sch_net_count(next)).map { index in
            let net = cc_sch_net_at(next, index)
            return SheetNet(name: schematicText(net.name),
                            wires: Set(wireNets.indices.filter { wireNets[$0] == index }),
                            labels: Set(labelNets.indices.filter { labelNets[$0] == index }),
                            pins: (0..<net.pin_count).map {
                                let pin = cc_sch_net_pin_at(next, index, $0)
                                return CGPoint(x: pin.x, y: pin.y)
                            },
                            pinKeys: (0..<net.pin_count).compactMap {
                                let pin = cc_sch_net_pin_at(next, index, $0)
                                guard symbols.indices.contains(pin.symbol) else { return nil }
                                return schematicText(symbols[pin.symbol].reference) + "/" + schematicText(pin.number)
                            })
        }
        select(min(selectedSymbol, max(symbols.count - 1, 0)))
        error = nil
        return true
    }

    /// Redisplays the current sheet instance from the session's committed state.
    private func refresh() {
        guard let session, let path = sheetPath else { return }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let text = cc_sch_session_sheet_source(session, path, &result) else {
            error = message(result)
            return
        }
        defer { cc_free_text(text) }
        if show(String(cString: text)) { name = trail.map(\.name).joined(separator: " / ") }
    }

    // MARK: Hierarchy

    /// Opens the child sheet with symbol UUID `uuid` on the displayed sheet.
    func enterSheet(_ uuid: String) {
        guard let path = sheetPath,
              let child = instances.first(where: { $0.path == path + "/" + uuid }) else { return }
        trail.append(SheetCrumb(path: child.path, name: child.name))
        selectedSymbol = 0
        refresh()
    }

    func goToCrumb(_ index: Int) {
        guard trail.indices.contains(index), index < trail.count - 1 else { return }
        trail.removeSubrange((index + 1)...)
        selectedSymbol = 0
        refresh()
    }

    // MARK: Session edits

    /// Runs one grouped edit on the displayed sheet, commits it, and saves the project.
    private func transact(_ edit: (OpaquePointer, String, UnsafeMutablePointer<cc_sch_result>) -> Int32) {
        guard let session, let path = sheetPath else { return }
        if let readOnlyReason { error = "This schematic is read-only here: \(readOnlyReason)"; return }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let transaction = cc_sch_transaction_begin(session, &result) else {
            error = message(result)
            return
        }
        defer { cc_sch_transaction_free(transaction) }
        guard edit(transaction, path, &result) == 1,
              cc_sch_transaction_commit(transaction, &result) == 1 else {
            error = message(result)
            return
        }
        onSessionCommit?()
        if cc_sch_session_save(session, &result) != 1 { error = message(result) }
        reloadInstances()
        refresh()
    }

    /// The sheet instances after an edit that may add or remove a sheet (or its undo).
    private func reloadInstances() {
        erc = nil // an edit or undo makes the last ERC stale
        clearSimulation() // and the last simulation
        guard let session else { return }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let json = cc_sch_session_snapshot_json(session, &result) else { return }
        defer { cc_free_text(json) }
        if let snapshot = try? JSONDecoder().decode(Snapshot.self, from: Data(String(cString: json).utf8)) {
            instances = snapshot.sheets
            // A sheet on the breadcrumb that an undo removed: back to the deepest one left.
            while trail.count > 1, !instances.contains(where: { $0.path == trail.last?.path }) { trail.removeLast() }
        }
    }

    /// Steps the session history and saves; used by the app-wide undo stack.
    func undo() throws { try step(cc_sch_session_undo) }
    func redo() throws { try step(cc_sch_session_redo) }

    private func step(_ action: (OpaquePointer?, UnsafeMutablePointer<cc_sch_result>?) -> Int32) throws {
        guard let session else { return }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard action(session, &result) == 1 else { throw DesignError.message(message(result)) }
        defer { reloadInstances(); refresh() }
        guard cc_sch_session_save(session, &result) == 1 else { throw DesignError.message(message(result)) }
    }

    func addTwoPin(x: Double, y: Double) {
        transact { cc_sch_transaction_add_two_pin($0, $1, x, y, $2) }
    }

    /// The session places symbols from KiCad's libraries (NATIVE_KICAD).
    var placesLibrarySymbols: Bool {
        guard let session else { return false }
        return cc_sch_session_capabilities(session) & CC_SCH_LIBRARY.rawValue != 0
    }

    /// Unit 1 of a library symbol ("Library:Name") with the next free reference for its prefix.
    func addLibrarySymbol(_ libraryId: String, x: Double, y: Double) {
        transact { cc_sch_transaction_add_library_symbol($0, $1, libraryId, x, y, $2) }
    }

    func addWire(x0: Double, y0: Double, x1: Double, y1: Double) {
        transact { cc_sch_transaction_add_routed_wire($0, $1, x0, y0, x1, y1, $2) }
    }

    func addLabel(_ label: String, x: Double, y: Double) {
        transact { cc_sch_transaction_add_label($0, $1, label, x, y, $2) }
    }

    // MARK: KiCad's editing tools (NATIVE_KICAD)

    /// The session runs KiCad's annotation, buses, sheets and BOM (CC_SCH_KICAD_TOOLS).
    var kicadTools: Bool {
        guard let session else { return false }
        return cc_sch_session_capabilities(session) & CC_SCH_KICAD_TOOLS.rawValue != 0
    }

    /// KiCad's Annotate for the whole hierarchy; `reset` renumbers every symbol.
    func annotate(reset: Bool) {
        transact { transaction, _, result in cc_sch_transaction_annotate(transaction, reset ? 1 : 0, result) }
    }

    func addBus(x0: Double, y0: Double, x1: Double, y1: Double) {
        transact { cc_sch_transaction_add_bus($0, $1, x0, y0, x1, y1, $2) }
    }

    /// Quadrant 1 up-right, 2 down-right, 3 down-left, 4 up-left from (x, y) on the bus.
    func addBusEntry(x: Double, y: Double, quadrant: Int32) {
        transact { cc_sch_transaction_add_bus_entry($0, $1, x, y, quadrant, $2) }
    }

    // Session API 8 (CC_SCH_KICAD_TOOLS): KiCad's no-connect flag, symbol mirror and notes.
    func addNoConnect(x: Double, y: Double) {
        transact { cc_sch_transaction_add_no_connect($0, $1, x, y, $2) }
    }

    /// Axis 0 flips top to bottom (KiCad's Mirror Vertically), 1 left to right.
    func mirrorSymbol(_ uuid: String, axis: Int32) {
        transact { cc_sch_transaction_mirror_symbol($0, $1, uuid, axis, $2) }
    }

    func addText(_ text: String, x: Double, y: Double) {
        transact { cc_sch_transaction_add_text($0, $1, text, x, y, $2) }
    }

    func addNoteLine(x0: Double, y0: Double, x1: Double, y1: Double) {
        transact { cc_sch_transaction_add_note_line($0, $1, x0, y0, x1, y1, $2) }
    }

    // Session API 9 (CC_SCH_KICAD_TOOLS): bus-to-bus entries and sheet resizing; deleteItem
    // also deletes a sheet there.
    func addBusBusEntry(x: Double, y: Double, quadrant: Int32) {
        transact { cc_sch_transaction_add_bus_bus_entry($0, $1, x, y, quadrant, $2) }
    }

    func resizeSheet(_ uuid: String, width: Double, height: Double) {
        transact { cc_sch_transaction_resize_sheet($0, $1, uuid, width, height, $2) }
    }

    /// Kind 0 local, 1 global, 2 hierarchical.
    func addLabel(_ label: String, x: Double, y: Double, kind: Int32) {
        transact { cc_sch_transaction_add_scoped_label($0, $1, label, x, y, kind, $2) }
    }

    /// A new hierarchical sheet <name>.kicad_sch in the box (millimetres, top left first).
    func addSheet(_ name: String, box: CGRect) {
        transact { cc_sch_transaction_add_sheet($0, $1, name, box.minX, box.minY, box.width, box.height, $2) }
    }

    /// Sheet pins for the child sheet's hierarchical labels that have none.
    func addSheetPins(_ uuid: String) {
        transact { cc_sch_transaction_add_sheet_pins($0, $1, uuid, $2) }
    }

    /// One finding of KiCad's ERC: the sheet it is on (KiCad's uuid_path) and where.
    struct ERCFinding: Identifiable {
        let id: Int
        let description: String
        let severity: String
        let detail: String
        let sheet: String
        let location: CGPoint?
        var isError: Bool { severity == "error" }
    }
    /// The last ERC run of the whole project, and the finding picked in the inspector.
    var erc: [ERCFinding]?
    var ercRunning = false
    var selectedERC: Int?

    /// KiCad runs ERC in this session (NATIVE_KICAD).
    var runsERC: Bool {
        guard let session else { return false }
        return cc_sch_session_capabilities(session) & CC_SCH_ERC.rawValue != 0
    }

    /// KiCad's ERC of the saved project with kicad-cli's defaults; the files are not changed.
    @MainActor
    func runERC() async {
        guard let session else { return }
        ercRunning = true
        // Let "Checking" draw before the call blocks (KiCad's state is shared, so it runs here).
        try? await Task.sleep(for: .milliseconds(60))
        defer { ercRunning = false }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let text = cc_sch_session_erc_json(session, &result) else { error = message(result); return }
        defer { cc_free_text(text) }
        struct Report: Decodable {
            struct Position: Decodable { let x, y: Double }
            struct Item: Decodable { let description: String; let pos: Position? }
            struct Entry: Decodable { let description, severity: String; let items: [Item] }
            struct Sheet: Decodable { let uuid_path: String?; let path: String?; let violations: [Entry] }
            let sheets: [Sheet]
        }
        do {
            let report = try JSONDecoder().decode(Report.self, from: Data(String(cString: text).utf8))
            var findings: [ERCFinding] = []
            for sheet in report.sheets {
                for entry in sheet.violations {
                    findings.append(ERCFinding(id: findings.count, description: entry.description, severity: entry.severity,
                                               detail: entry.items.map(\.description).joined(separator: "\n"),
                                               sheet: sheet.uuid_path ?? sheet.path ?? "/",
                                               location: entry.items.first?.pos.map { CGPoint(x: $0.x, y: $0.y) }))
                }
            }
            erc = findings
            selectedERC = nil
        } catch {
            self.error = "Cannot read KiCad's ERC report: \(error.localizedDescription)"
        }
    }

    /// Whether a finding's sheet is the one displayed: KiCad's uuid_path names the sheet by its
    /// last UUID, as the session's instance paths do ("/" is the root).
    func ercOnDisplayedSheet(_ finding: ERCFinding) -> Bool {
        guard let path = sheetPath else { return false }
        let last = { (text: String) in text.split(separator: "/").last.map(String.init) ?? "" }
        if trail.count == 1 { return finding.sheet == "/" || last(finding.sheet) == last(path) }
        return last(finding.sheet) == last(path)
    }

    /// The name of a finding's sheet, for findings off the displayed sheet.
    func ercSheetName(_ finding: ERCFinding) -> String {
        let last = { (text: String) in text.split(separator: "/").last.map(String.init) ?? "" }
        if finding.sheet == "/" { return trail.first?.name ?? "root" }
        return instances.first { last($0.path) == last(finding.sheet) }?.name ?? finding.sheet
    }

    /// KiCad's netlist of the project in the temporary directory, for the share sheet.
    func exportNetlist() -> URL? {
        guard let text = kicadNetlist() else { error = "KiCad's netlist is not available in this edition"; return nil }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(trail.first?.name ?? "schematic").net")
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            return url
        } catch {
            self.error = error.localizedDescription
            return nil
        }
    }

    /// KiCad plots this session's schematic (NATIVE_KICAD).
    var plots: Bool {
        guard let session else { return false }
        return cc_sch_session_capabilities(session) & CC_SCH_PLOT.rawValue != 0
    }

    /// KiCad's plot of every sheet as kicad-cli writes it: one PDF, or one SVG per sheet, in a
    /// fresh temporary folder for the share sheet.
    func plot(svg: Bool) -> [URL] {
        guard let session else { return [] }
        let name = trail.first?.name ?? "schematic"
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("plot-\(UUID().uuidString)")
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        } catch {
            self.error = error.localizedDescription
            return []
        }
        let output = svg ? folder : folder.appendingPathComponent("\(name).pdf")
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard output.path.withCString({ cc_sch_session_plot(session, svg ? 1 : 0, $0, &result) }) == 1 else {
            error = message(result)
            return []
        }
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    // MARK: Simulation (KiCad's simulator with ngspice)

    /// The last run, the plot's traces and whether the plot is showing.
    var simulation: SimulationResult?
    var simulationTraces: [SimulationTrace] = []
    var showsSimulationPlot = false
    var simulationRunning = false
    /// Says why a probe added nothing.
    var simulationNote: String?
    @ObservationIgnored private var projectFile: URL?
    /// "<reference>/<pin number>" to KiCad's net name, from the netlist of the last run.
    @ObservationIgnored private var pinNets: [String: String] = [:]

    /// KiCad's simulator can run in this session (NATIVE_KICAD built with ngspice).
    var simulates: Bool {
        guard let session else { return false }
        return cc_sch_session_capabilities(session) & CC_SCH_SIMULATE.rawValue != 0
    }

    private func clearSimulation() {
        simulation = nil
        simulationTraces = []
        showsSimulationPlot = false
        simulationNote = nil
    }

    /// The first tab of the project's simulator workbook: its commands as KiCad stores them
    /// (analysis and option lines) and the signals it plots.
    var workbookTab: (commands: String, signals: [String])? {
        guard let projectFile, let project = try? JSONSerialization.jsonObject(with: Data(contentsOf: projectFile)) as? [String: Any] else { return nil }
        let settings = (project["schematic"] as? [String: Any])?["ngspice"] as? [String: Any]
        guard let name = settings?["workbook_filename"] as? String, !name.isEmpty,
              let workbook = try? JSONSerialization.jsonObject(with: Data(contentsOf: projectFile.deletingLastPathComponent().appendingPathComponent(name))) as? [String: Any],
              let tab = (workbook["tabs"] as? [[String: Any]])?.first,
              let commands = tab["commands"] as? [String] else { return nil }
        let signals = (tab["traces"] as? [[String: Any]] ?? []).compactMap { $0["signal"] as? String }
        return (commands.joined(separator: "\n"), signals)
    }

    /// Runs one analysis (see cc_sch_session_simulate; empty uses the schematic's directive).
    /// KiCad's state is shared, so the caller shows a modal for the whole run. ngspice parses on
    /// this thread, with more stack than a task thread has.
    @MainActor
    func simulate(_ command: String, signals: [String] = []) async {
        guard let session else { return }
        simulationRunning = true
        defer { simulationRunning = false }
        struct Handle: @unchecked Sendable { let session: OpaquePointer }
        let handle = Handle(session: session)
        let outcome: (SimulationResult?, String?) = await withCheckedContinuation { continuation in
            let thread = Thread {
                var result = cc_sch_result()
                defer { cc_sch_result_clear(&result) }
                // ponytail: every n-th point, up to 4000 for the plot and the board; a switching
                // waveform can lose edges. Min/max decimation in the C API if that shows.
                guard let text = cc_sch_session_simulate(handle.session, command, 4000, &result) else {
                    continuation.resume(returning: (nil, result.message.map { String(cString: $0) } ?? "The simulation failed"))
                    return
                }
                defer { cc_free_text(text) }
                var netlist = ""
                if let spice = cc_sch_session_spice_netlist(handle.session, command, &result) {
                    netlist = String(cString: spice)
                    cc_free_text(spice)
                }
                continuation.resume(returning: (SimulationResult(text: String(cString: text), netlist: netlist), nil))
            }
            thread.stackSize = 16 << 20
            thread.start()
        }
        guard let run = outcome.0 else {
            error = outcome.1 ?? "KiCad's simulator returned nothing readable"
            return
        }
        simulation = run
        pinNets = Self.pinNets(netlist: kicadNetlist() ?? "")
        simulationNote = nil
        simulationTraces = []
        for name in signals.compactMap({ run.vector(forSignal: $0) }) + run.defaultTraces where simulationTraces.count < 3 {
            addTrace(name)
        }
        showsSimulationPlot = true
    }

    private static func pinNets(netlist: String) -> [String: String] {
        var nets: [String: String] = [:]
        // KiCad writes one token per line; only the order matters here.
        for block in netlist.replacing(/\s+/, with: " ").components(separatedBy: "(net (code").dropFirst() {
            guard let name = block.firstMatch(of: /\(name "((?:[^"\\]|\\.)*)"\)/)?.1 else { continue }
            for node in block.matches(of: /\(node \(ref "([^"]*)"\) \(pin "([^"]*)"/) {
                nets["\(node.1)/\(node.2)"] = String(name).replacingOccurrences(of: "\\\"", with: "\"")
            }
        }
        return nets
    }

    /// Adds a vector to the plot; a trace keeps its colour until it is removed.
    func addTrace(_ name: String) {
        guard !simulationTraces.contains(where: { $0.name == name }) else { return }
        guard let slot = (0..<SimulationTrace.limit).first(where: { slot in !simulationTraces.contains { $0.slot == slot } }) else {
            simulationNote = "\(SimulationTrace.limit) traces at most. Remove one first."
            return
        }
        simulationNote = nil
        simulationTraces.append(SimulationTrace(name: name, slot: slot))
    }

    /// A tapped net on the canvas: its voltage on the plot.
    func probeNet(_ index: Int) {
        guard let simulation, nets.indices.contains(index) else { return }
        let net = nets[index]
        let kicadName = net.pinKeys.lazy.compactMap { self.pinNets[$0] }.first ?? net.name
        guard let vector = simulation.vectors[SpiceNames.node(kicadName)] else {
            simulationNote = SpiceNames.isGround(kicadName) ? "\(kicadName) is the 0 V reference." : "No simulated voltage for \(kicadName)."
            return
        }
        addTrace(vector.name)
    }

    /// A tapped symbol: the current through it, its first terminal's for a transistor.
    func probeSymbol(_ index: Int) {
        guard let simulation, symbols.indices.contains(index) else { return }
        let reference = schematicText(symbols[index].reference)
        guard let vector = simulation.currentVector(reference: reference) else {
            simulationNote = "No simulated current for \(reference). Save currents and run again."
            return
        }
        addTrace(vector)
    }

    /// KiCad's BOM of the project as CSV in the temporary directory, for the share sheet.
    func exportBOM() -> URL? {
        guard let session else { return nil }
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        guard let text = cc_sch_session_bom_csv(session, &result) else { error = message(result); return nil }
        defer { cc_free_text(text) }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("\(trail.first?.name ?? "schematic")-bom.csv")
        do {
            try String(cString: text).write(to: url, atomically: true, encoding: .utf8)
            return url
        } catch {
            self.error = error.localizedDescription
            return nil
        }
    }

    func moveSymbol(_ uuid: String, x: Double, y: Double) {
        transact { cc_sch_transaction_move_symbol($0, $1, uuid, x, y, $2) }
    }

    func deleteItem(_ uuid: String) {
        transact { cc_sch_transaction_delete_item($0, $1, uuid, $2) }
    }

    func renameSymbol(_ uuid: String, reference: String) {
        transact { cc_sch_transaction_rename_symbol($0, $1, uuid, reference, $2) }
    }

    func renameLabel(_ uuid: String, name: String) {
        transact { cc_sch_transaction_rename_label($0, $1, uuid, name, $2) }
    }

    func rotateSymbol(_ uuid: String) {
        transact { cc_sch_transaction_rotate_symbol($0, $1, uuid, $2) }
    }

    /// Value and Footprint in one transaction, so one undo restores both.
    func setFields(_ uuid: String, value: String, footprint: String) {
        transact { transaction, path, result in
            cc_sch_transaction_set_property(transaction, path, uuid, "Value", value, result) == 1 ?
                cc_sch_transaction_set_property(transaction, path, uuid, "Footprint", footprint, result) : 0
        }
    }

    func moveLabel(_ uuid: String, x: Double, y: Double) {
        transact { cc_sch_transaction_move_label($0, $1, uuid, x, y, $2) }
    }

    /// The engine's planned route for a wire, in millimetres; nil when no clear route exists.
    /// Called on every finger move: KiCad's session routes from its in-memory model, and the
    /// compatible edition from the sheet already parsed for display (no text parse per call).
    func route(from start: CGPoint, to end: CGPoint) -> [CGPoint]? {
        var xy = [Double](repeating: 0, count: 32)
        let count: Int
        if nativeKiCad, let session, let path = sheetPath {
            var result = cc_sch_result()
            defer { cc_sch_result_clear(&result) }
            count = cc_sch_session_route(session, path, start.x, start.y, end.x, end.y, &xy, 16, &result)
        } else if let handle {
            var buffer = [CChar](repeating: 0, count: 512)
            let capacity = buffer.count
            count = cc_sch_route(handle, start.x, start.y, end.x, end.y, &xy, 16, &buffer, capacity)
        } else {
            return nil
        }
        guard count > 0, count <= 16 else { return nil }
        return (0..<count).map { CGPoint(x: xy[2 * $0], y: xy[2 * $0 + 1]) }
    }

    // MARK: Inspector

    func select(_ index: Int) {
        guard symbols.indices.contains(index) else { return }
        let symbol = symbols[index]
        selectedSymbol = index
        editX = symbol.x
        editY = symbol.y
        editValue = schematicText(symbol.value)
        editFootprint = schematicText(symbol.footprint)
    }

    private var selectedUUID: String? {
        symbols.indices.contains(selectedSymbol) ? schematicText(symbols[selectedSymbol].uuid) : nil
    }

    func saveMove() {
        guard let uuid = selectedUUID else { return }
        if sessionBacked { moveSymbol(uuid, x: editX, y: editY) } else { saveCopy(kind: "move", uuid: uuid) }
    }

    func saveValue() { saveProperty("Value", editValue) }
    func saveFootprint() { saveProperty("Footprint", editFootprint) }

    private func saveProperty(_ property: String, _ value: String) {
        guard let uuid = selectedUUID else { return }
        if sessionBacked {
            transact { cc_sch_transaction_set_property($0, $1, uuid, property, value, $2) }
        } else {
            saveCopy(kind: property, uuid: uuid)
        }
    }

    /// Imported sheets: writes the edit to a temporary copy and displays it.
    private func saveCopy(kind: String, uuid: String) {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("CorvCAD-Edited.kicad_sch")
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let ok: Int32 = source.withCString { text in
            if kind == "move" {
                return cc_move_schematic_symbol_file(text, uuid, editX, editY, url.path, &buffer, capacity)
            }
            let value = kind == "Value" ? editValue : editFootprint
            return cc_set_schematic_property_file(text, uuid, kind, value, url.path, &buffer, capacity)
        }
        guard ok == 1 else { error = String(cString: buffer); return }
        load(url)
        if error == nil {
            if let index = symbols.firstIndex(where: { schematicText($0.uuid) == uuid }) { select(index) }
            editedURL = url
        }
    }

    private func message(_ result: cc_sch_result) -> String {
        result.message.map { String(cString: $0) } ?? "The schematic operation failed."
    }
}

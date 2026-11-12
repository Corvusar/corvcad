import SceneKit
import SwiftUI
import UniformTypeIdentifiers

private func decodedCString<T>(_ value: T) -> String {
    var copy = value
    return withUnsafeBytes(of: &copy) { bytes in
        String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
    }
}

@Observable
final class Design {
    var boardName = "No board loaded"
    var width = 0.0
    var height = 0.0
    var holes: [String] = []
    var placements: [String] = []
    var footprintMarkers: [(Double, Double, String)] = []
    var outline: [CGPoint] = []
    var nets: [cc_net] = []
    var tracks: [cc_track] = []
    var vias: [cc_via] = []
    var pads: [cc_pad] = []
    var zones: [ZoneShape] = []
    /// Silkscreen and fab lines (net 0) and board texts.
    var graphics: [cc_track] = []
    var texts: [cc_board_text] = []
    /// KiCad's ratsnest (NATIVE_KICAD edition): unrouted connections, board millimetres.
    var ratsnest: [RatsnestEdge] = []
    /// KiCad's netclass track width and via size per net (NATIVE_KICAD edition); empty otherwise.
    var netRules: [String: NetRule] = [:]
    var scene = SCNScene()
    var exportURL: URL?
    var lidExportURL: URL?
    var editedBoardURL: URL?
    var error: String?
    var settings = cc_default_settings()
    private(set) var mechanical: MechanicalSession?
    private(set) var mechanicalInput: MechanicalBoardInput?
    private(set) var mechanicalError: String?
    private(set) var mechanicalCanUndo = false
    private(set) var mechanicalCanRedo = false
    /// The enclosure document's PartDesign features in tree order (FreeCAD edition).
    private(set) var mechanicalFeatures: [MechanicalFeature] = []
    /// The tray's wall cutouts and its height (FreeCAD edition).
    private(set) var cutouts: [WallCutout] = []
    private(set) var vents: [VentRow] = []
    private(set) var screws: LidScrews?
    /// Sketches on the lid or a wall, cut in or extruded out (FreeCAD edition).
    private(set) var sketches: [UserSketch] = []
    private(set) var screwPosts: [String: Double] = [:]
    private(set) var trayHeight = 0.0
    private(set) var mechanicalDirty = false
    private(set) var mechanicalSavedURL: URL?
    private(set) var nativeTriangleCounts = [0, 0]
    private var mechanicalProject: URL?
    var mechanicalReady: Bool { mechanical != nil && mechanicalError == nil }
    var showLid = true
    var exploded = true
    var showParts = true
    let parts = BoardParts()
    var bounds = CGRect.zero
    var selectedFootprint = 0
    var editX = 0.0
    var editY = 0.0
    var editRotation = 0.0
    /// KiCad DRC of the loaded board (NATIVE_KICAD edition, BoardCheck.swift); cleared on load,
    /// so an edit never leaves a stale report.
    var drc: DRCResult?
    var drcRunning = false
    var drcError: String?
    var selectedFinding: Int?
    var hasZones: Bool { boardSource.contains("(zone\n") || boardSource.contains("(zone ") }
    /// The board's thickness from its (general (thickness …)) block, mm.
    var thickness: Double? { boardSource.firstMatch(of: /\(thickness ([0-9.]+)\)/).flatMap { Double($0.1) } }
    /// Copper layers in the board's layer table.
    var copperLayers: Int? {
        let count = boardSource.matches(of: /\(\d+ "(?:F|B|In\d+)\.Cu"/).count
        return count > 0 ? count : nil
    }
    var filling = false
    /// KiCad's Gerbers, drill and placement files of the loaded board, zipped (BoardCheck.swift).
    var fabZipURL: URL?
    var exportingFab = false
    /// A route in progress with KiCad's router (BoardRouting.swift); nil when not routing.
    var routing: BoardRoute?
    /// Called after a local project file is replaced: (file, text before, text after).
    @ObservationIgnored var onCommit: ((URL, String, String) -> Void)?
    /// Called after one edit replaced several project files (Board Setup).
    @ObservationIgnored var onCommitFiles: (([EditHistory.FileChange]) -> Void)?
    private var board: OpaquePointer?
    /// A mechanical-only project's inside space, in place of a board (FreeCAD edition).
    private(set) var box: BoxSize?
    private var boardSourceData = Data()
    private(set) var boardSource = ""

    deinit { if let board { cc_board_free(board) } }

    func load(_ url: URL) {
        guard url.pathExtension.lowercased() == "kicad_pcb" else {
            error = "Choose a KiCad .kicad_pcb file."
            return
        }
        let access = url.startAccessingSecurityScopedResource()
        defer { if access { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            guard let source = String(data: data, encoding: .utf8) else {
                throw DesignError.message("The KiCad board must contain UTF-8 text.")
            }
            var buffer = [CChar](repeating: 0, count: 512)
            let capacity = buffer.count
            let next = source.withCString { cc_board_parse($0, &buffer, capacity) }
            guard let next else { throw DesignError.message(String(cString: buffer)) }
            if let board { cc_board_free(board) }
            board = next
            box = nil
            boardSource = source
            boardSourceData = data
            boardName = url.lastPathComponent
            var x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0
            cc_board_bounds(next, &x0, &y0, &x1, &y1)
            width = x1 - x0; height = y1 - y0
            bounds = CGRect(x: CGFloat(x0), y: CGFloat(y0), width: CGFloat(width), height: CGFloat(height))
            outline = (0..<cc_outline_count(next)).map { index in
                var x = 0.0, y = 0.0
                cc_outline_point(next, index, &x, &y)
                return CGPoint(x: CGFloat(x), y: CGFloat(y))
            }
            nets = (0..<cc_net_count(next)).map { cc_net_at(next, $0) }
            tracks = (0..<cc_track_count(next)).map { cc_track_at(next, $0) }
            vias = (0..<cc_via_count(next)).map { cc_via_at(next, $0) }
            pads = (0..<cc_pad_count(next)).map { cc_pad_at(next, $0) }
            zones = (0..<cc_zone_count(next)).map { index in
                let zone = cc_zone_at(next, index)
                let points = (0..<zone.point_count).map { point in
                    var x = 0.0, y = 0.0
                    cc_zone_point(next, index, point, &x, &y)
                    return CGPoint(x: x, y: y)
                }
                return ZoneShape(net: zone.net, layer: decodedCString(zone.layer), filled: zone.filled != 0, points: points,
                                 uuid: decodedCString(zone.uuid), properties: zone)
            }
            graphics = (0..<cc_graphic_count(next)).map { cc_graphic_at(next, $0) }
            texts = (0..<cc_text_count(next)).map { cc_text_at(next, $0) }
            holes = (0..<cc_hole_count(next)).map { index in
                let h = cc_hole_at(next, index)
                return "\(decodedCString(h.reference)) · Ø\(String(format: "%.1f", h.diameter)) mm"
            }
            placements = (0..<cc_placement_count(next)).map { index in
                let p = cc_placement_at(next, index)
                let ref = decodedCString(p.reference)
                let value = decodedCString(p.value)
                return "\(ref) · \(value) (\(String(format: "%.1f", p.x)), \(String(format: "%.1f", p.y)))"
            }
            footprintMarkers = (0..<cc_placement_count(next)).map { index in
                let p = cc_placement_at(next, index)
                return (p.x, p.y, decodedCString(p.reference))
            }
            selectedFootprint = 0
            selectFootprint(0)
            exportURL = nil
            lidExportURL = nil
            editedBoardURL = nil
            drc = nil
            drcError = nil
            fabZipURL = nil
            selectedFinding = nil
            if routing != nil { cc_board_native_route_cancel(); routing = nil }
            ratsnest = Self.hasKiCadDRC ? RatsnestEdge.load(board: url) : []
            netRules = Self.hasKiCadDRC ? NetRule.load(board: url) : [:]
            error = nil
            if MechanicalSession.enabled { prepareMechanical() }
            rebuild()
        } catch { self.error = error.localizedDescription }
    }

    /// Opens a mechanical-only project: no board, an enclosure shaped around `size`.
    func loadBox(_ size: BoxSize, name: String) {
        if let board { cc_board_free(board) }
        board = nil
        box = size
        boardSource = ""
        boardSourceData = Data()
        boardName = name
        (width, height) = (size.width, size.depth)
        bounds = CGRect(x: 0, y: 0, width: size.width, height: size.depth)
        (outline, nets, tracks, vias, pads, zones, graphics, texts) = ([], [], [], [], [], [], [], [])
        (holes, placements, footprintMarkers, ratsnest, netRules) = ([], [], [], [], [:])
        exportURL = nil
        lidExportURL = nil
        error = nil
        if MechanicalSession.enabled { prepareMechanical() }
        rebuild()
    }

    func selectFootprint(_ index: Int) {
        guard index >= 0 && index < footprintMarkers.count, let board else { return }
        let placement = cc_placement_at(board, index)
        selectedFootprint = index
        editX = placement.x
        editY = placement.y
        editRotation = placement.rotation
    }

    func saveMovedFootprint(to projectURL: URL? = nil) {
        guard selectedFootprint < footprintMarkers.count else { return }
        let reference = footprintMarkers[selectedFootprint].2
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("CorvCAD-Edited.kicad_pcb")
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let ok = boardSource.withCString { source in
            reference.withCString { ref in
                url.path.withCString { path in
                    cc_move_footprint_file(source, ref, editX, editY, editRotation,
                                           path, &buffer, capacity)
                }
            }
        }
        guard ok == 1 else { error = String(cString: buffer); return }
        if let projectURL {
            do {
                let data = try Data(contentsOf: url)
                try data.write(to: projectURL, options: .atomic)
                onCommit?(projectURL, boardSource, String(decoding: data, as: UTF8.self))
                load(projectURL)
                if let index = footprintMarkers.firstIndex(where: { $0.2 == reference }) {
                    selectFootprint(index)
                }
            } catch { self.error = error.localizedDescription }
        } else {
            load(url)
            editedBoardURL = url
        }
    }

    func saveAuthored(_ text: UnsafeMutablePointer<CChar>?,
                              errorBuffer: [CChar], to url: URL) {
        guard let text else { error = String(cString: errorBuffer); return }
        defer { cc_free_text(text) }
        do {
            let after = String(cString: text)
            try Data(after.utf8).write(to: url, options: .atomic)
            onCommit?(url, boardSource, after)
            load(url)
        } catch { self.error = error.localizedDescription }
    }

    func addTwoPad(reference: String, value: String, x: Double, y: Double, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            reference.withCString { ref in
                value.withCString { partValue in
                    cc_add_board_two_pad(source, ref, partValue, x, y, &buffer, capacity)
                }
            }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func addMountingHole(x: Double, y: Double, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString {
            cc_add_board_mounting_hole($0, x, y, &buffer, capacity)
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func moveFootprint(_ reference: String, x: Double, y: Double, to url: URL) {
        guard let index = footprintMarkers.firstIndex(where: { $0.2 == reference }) else { return }
        selectFootprint(index)
        editX = x
        editY = y
        saveMovedFootprint(to: url)
    }

    func deleteFootprint(_ reference: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            reference.withCString { cc_delete_board_footprint(source, $0, &buffer, capacity) }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func renameFootprint(_ reference: String, as updated: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            reference.withCString { old in
                updated.withCString { cc_rename_board_footprint(source, old, $0, &buffer, capacity) }
            }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func rotateFootprint(_ reference: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            reference.withCString { cc_rotate_board_footprint(source, $0, &buffer, capacity) }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func addTrack(from start: CGPoint, to end: CGPoint, layer: String, net: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            layer.withCString { layerName in
                net.withCString {
                    cc_add_board_track(source, start.x, start.y, end.x, end.y, layerName, $0, rule(net)?.track ?? 0,
                                       &buffer, capacity)
                }
            }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    /// The net's netclass sizes, or the Default netclass for a track or via without a net.
    private func rule(_ net: String) -> NetRule? { netRules[net] ?? netRules[""] }

    func addVia(at point: CGPoint, net: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            net.withCString {
                cc_add_board_via(source, point.x, point.y, $0, rule(net)?.via ?? 0, rule(net)?.drill ?? 0, &buffer, capacity)
            }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    func deleteCopper(_ uuid: String, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            uuid.withCString { cc_delete_board_copper(source, $0, &buffer, capacity) }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    /// Adds a drawing. In the NATIVE_KICAD edition a new zone, or a new outline on a board with
    /// zones, is filled by KiCad's zone filler in the same undo step.
    /// KiCad's board edits on the project board (NATIVE_KICAD, cc_board_native_edit): one
    /// tab-separated request, written back as one undo step; a zone edit is refilled in it.
    func nativeEdit(_ fields: [String], to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = fields.joined(separator: "\t").withCString { cc_board_native_edit(url.path, $0, &buffer, capacity) }
        saveDrawing(text, errorBuffer: buffer, refill: fields.first == "zone", to: url)
    }

    func draw(_ drawing: BoardDrawing, to url: URL) {
        if case let .dimension(start, end, height, layer) = drawing {
            nativeEdit(["dimension", layer] + [start.x, start.y, end.x, end.y, height].map { String(format: "%.6f", locale: Locale(identifier: "en_US_POSIX"), Double($0)) }, to: url)
            return
        }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        func corners(_ points: [CGPoint]) -> [Double] { points.flatMap { [Double($0.x), Double($0.y)] } }
        var refill = false
        let text: UnsafeMutablePointer<CChar>? = boardSource.withCString { source in
            switch drawing {
            case .outline(let points):
                return corners(points).withUnsafeBufferPointer {
                    cc_set_board_outline(source, $0.baseAddress, points.count, &buffer, capacity)
                }
            case .zone(let points, let layer, let net):
                return corners(points).withUnsafeBufferPointer { xy in
                    cc_add_board_zone(source, xy.baseAddress, points.count, layer, net, &buffer, capacity)
                }
            case .line(let start, let end, let layer):
                return cc_add_board_line(source, start.x, start.y, end.x, end.y, layer, 0, &buffer, capacity)
            case .text(let value, let point, let layer, let size):
                return cc_add_board_text(source, value, point.x, point.y, layer, size, &buffer, capacity)
            case .dimension:
                return nil  // KiCad's edit above
            }
        }
        switch drawing {
        case .zone: refill = true
        case .outline: refill = hasZones
        case .line, .text, .dimension: break
        }
        saveDrawing(text, errorBuffer: buffer, refill: refill, to: url)
    }

    /// Moves a drawn line, text or zone. A moved zone loses its stale fill; the NATIVE_KICAD
    /// edition refills it in the same undo step.
    func moveDrawing(_ uuid: String, by offset: CGSize, zone: Bool, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            uuid.withCString { cc_move_board_drawing(source, $0, offset.width, offset.height, &buffer, capacity) }
        }
        saveDrawing(text, errorBuffer: buffer, refill: zone, to: url)
    }

    func setText(_ uuid: String, text value: String, size: Double, to url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let text = boardSource.withCString { source in
            uuid.withCString { cc_set_board_text(source, $0, value, size, &buffer, capacity) }
        }
        saveAuthored(text, errorBuffer: buffer, to: url)
    }

    private func saveDrawing(_ text: UnsafeMutablePointer<CChar>?, errorBuffer buffer: [CChar], refill: Bool, to url: URL) {
        var buffer = buffer
        let capacity = buffer.count
        guard refill, Self.hasKiCadDRC, let text else { saveAuthored(text, errorBuffer: buffer, to: url); return }
        let before = boardSource
        var after = String(cString: text)
        cc_free_text(text)
        do {
            try Data(after.utf8).write(to: url, options: .atomic)
            // ponytail: main thread, as fillZones (KiCad's state is shared with the schematic session).
            let filled = cc_board_native_fill_zones(url.path, &buffer, capacity)
            if let filled {
                after = String(cString: filled)
                cc_free_text(filled)
                try Data(after.utf8).write(to: url, options: .atomic)
            }
            onCommit?(url, before, after)
            load(url)
            if filled == nil { drcError = "Zone not filled: " + String(cString: buffer) }
        } catch { self.error = error.localizedDescription }
    }

    /// Update PCB from schematic (engine subset: pad nets, missing two-pad parts). Returns the
    /// engine's report ("" when every symbol was handled), or nil with `error` set.
    func updateFromSchematic(_ schematicSource: String, to url: URL) -> String? {
        var buffer = [CChar](repeating: 0, count: 512)
        var report = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count, reportCapacity = report.count
        let text = boardSource.withCString { board in
            schematicSource.withCString {
                cc_update_board_from_schematic(board, $0, &report, reportCapacity, &buffer, capacity)
            }
        }
        guard let text else { error = String(cString: buffer); return nil }
        if String(cString: text) == boardSource {
            cc_free_text(text) // already in sync: no file write, no undo step
        } else {
            saveAuthored(text, errorBuffer: buffer, to: url)
        }
        return String(cString: report)
    }

    func setMechanicalProject(_ project: URL?) {
        mechanical = nil
        mechanicalInput = nil
        mechanicalError = MechanicalSession.enabled && project == nil
            ? "Open a local project to create and save an enclosure." : nil
        mechanicalSavedURL = nil
        mechanicalDirty = false
        mechanicalCanUndo = false
        mechanicalCanRedo = false
        mechanicalFeatures = []
        cutouts = []
        vents = []
        screws = nil
        sketches = []
        screwPosts = [:]
        trayHeight = 0
        nativeTriangleCounts = [0, 0]
        mechanicalProject = project
    }

    private func prepareMechanical() {
        guard board != nil || box != nil, let project = mechanicalProject else {
            mechanicalError = "Open a local project to create and save an enclosure."
            return
        }
        do {
            let input = try box.map { try MechanicalBoardInput(box: $0) }
                ?? MechanicalBoardInput(board: board!, source: boardSourceData, name: boardName, outline: outline)
            if let previous = mechanicalInput, previous.identity != input.identity, mechanical != nil {
                // Preserve an unsaved session while the PCB changes. Restoring its layout re-enables it.
                throw DesignError.message("The board's outline, thickness or mounting holes changed. Restore its layout or reopen the project before editing the enclosure.")
            }
            if mechanical == nil {
                var initial = cc_default_settings()
                if box != nil {  // the typed size is the inside: no margin, standoffs or top clearance
                    (initial.margin, initial.standoff_height, initial.board_clearance) = (0, 0, 0)
                }
                mechanical = try MechanicalSession(project: project, input: input, initial: initial)
                mechanicalInput = input
                mechanicalDirty = !FileManager.default.fileExists(atPath: mechanical!.documentURL.path)
                mechanicalSavedURL = mechanicalDirty ? nil : mechanical!.documentURL
            }
            try refreshMechanical()
            mechanicalError = nil
        } catch { mechanicalError = error.localizedDescription }
    }

    private func refreshMechanical() throws {
        guard let mechanical else { throw DesignError.message("Open an enclosure first.") }
        settings = try mechanical.settings
        mechanicalCanUndo = mechanical.canUndo
        mechanicalCanRedo = mechanical.canRedo
        mechanicalFeatures = try mechanical.features()
        let openings = try mechanical.openings()
        (cutouts, vents, screws, sketches) = (openings.cutouts, openings.vents, openings.screws, openings.sketches)
        (screwPosts, trayHeight) = (openings.screw_posts, openings.tray_height)
    }

    func setCutouts(_ next: [WallCutout]) { _ = setOpenings(cutouts: next, vents: vents, screws: screws) }
    func setVents(_ next: [VentRow]) { _ = setOpenings(cutouts: cutouts, vents: next, screws: screws) }
    /// False when the session refused the sketches (the error says why); the enclosure is unchanged.
    @discardableResult func setSketches(_ next: [UserSketch]) -> Bool {
        setOpenings(cutouts: cutouts, vents: vents, screws: screws, sketches: next)
    }

    /// A polygon edge set to `length` by FreeCAD's Sketcher; nil (and `error` set) if it has no solution.
    func solveSketch(_ points: [CGPoint], bulges: [Double], edge: Int, length: Double) -> SketchShape? {
        guard let mechanical else { return nil }
        do { return try mechanical.solveSketch(points: points, bulges: bulges, edge: edge, length: length) } catch {
            self.error = error.localizedDescription
            return nil
        }
    }

    /// Lid screws on or off; turning them on raises the board margin to their corner posts' size
    /// in the same undo step.
    func setScrews(_ next: LidScrews?) {
        let needed = next.flatMap { screwPosts[$0.postKey] } ?? 0
        _ = setOpenings(cutouts: cutouts, vents: vents, screws: next, margin: needed > settings.margin ? needed : nil)
    }

    /// One undoable change of the openings; a rejected one keeps the last valid enclosure.
    @discardableResult
    func setOpenings(cutouts nextCutouts: [WallCutout], vents nextVents: [VentRow], screws nextScrews: LidScrews?,
                     sketches nextSketches: [UserSketch]? = nil, margin: Double? = nil) -> Bool {
        let nextSketches = nextSketches ?? sketches
        guard mechanicalReady, let mechanical else { return false }
        guard nextCutouts != cutouts || nextVents != vents || nextScrews != screws || nextSketches != sketches
                || margin != nil else { return true }
        do {
            try mechanical.setOpenings(cutouts: nextCutouts, vents: nextVents, screws: nextScrews, sketches: nextSketches,
                                       margin: margin)
            mechanicalDirty = true
            try refreshMechanical()
            rebuild()
            return true
        } catch {
            try? refreshMechanical()
            self.error = error.localizedDescription
            return false
        }
    }

    /// A new vent row that fits: on the lid, 2 mm slots 5 mm apart over the middle of the board;
    /// on a wall, vertical 2 mm slots 4 mm apart over the middle half of that edge.
    func defaultVents(on face: String) -> VentRow {
        let x0 = Double(bounds.minX), y0 = Double(bounds.minY), w = Double(bounds.width), d = Double(bounds.height)
        if face == "lid" {
            let count = min(12, max(1, Int((0.6 * w - 2) / 5) + 1))
            return VentRow(face: face, center: x0 + w / 2, across: y0 + d / 2, count: count, width: 2,
                           length: max(4, min(30, (0.4 * d).rounded())), pitch: 5)
        }
        let along = face == "front" || face == "back" ? (x0, w) : (y0, d)
        let length = max(1, min(6, trayHeight - 3))
        return VentRow(face: face, center: along.0 + along.1 / 2, across: max(0, (trayHeight - length) / 2),
                       count: min(10, max(1, Int((0.5 * along.1 - 2) / 4) + 1)), width: 2, length: length, pitch: 4)
    }

    /// A cutout for each connector (J, P, CN, USB or X footprint) whose pads come within 6 mm of a
    /// board edge: on that wall, as wide as its pads along the edge plus 1 mm each side, from just
    /// below the board's top face, up to 0.5 mm over its generated body (5 mm high without one).
    /// Keyed by reference.
    var connectorCutouts: [(reference: String, cutout: WallCutout)] {
        guard trayHeight > 0, width > 0 else { return [] }
        let x0 = Double(bounds.minX), y0 = Double(bounds.minY), x1 = Double(bounds.maxX), y1 = Double(bounds.maxY)
        let margin = settings.margin
        let boardTop = trayHeight - settings.board_clearance
        let groups = Dictionary(grouping: pads, by: { decodedCString($0.reference) })
        return groups.keys.sorted().compactMap { reference in
            guard ["J", "P", "CN", "USB", "X"].contains(where: { reference.hasPrefix($0) }),
                  let rest = reference.drop(while: \.isLetter).first, rest.isNumber, let group = groups[reference] else { return nil }
            let lo = (x: group.map { $0.x - $0.size_x / 2 }.min()!, y: group.map { $0.y - $0.size_y / 2 }.min()!)
            let hi = (x: group.map { $0.x + $0.size_x / 2 }.max()!, y: group.map { $0.y + $0.size_y / 2 }.max()!)
            let distances = [("front", lo.y - y0), ("back", y1 - hi.y), ("left", lo.x - x0), ("right", x1 - hi.x)]
            guard let (side, distance) = distances.min(by: { $0.1 < $1.1 }), distance <= 6 else { return nil }
            let horizontal = side == "front" || side == "back"
            let (a, b, low, high) = horizontal ? (lo.x, hi.x, x0, x1) : (lo.y, hi.y, y0, y1)
            let span = (low - margin, high + margin)
            let width = min(max(b - a + 2, 4), span.1 - span.0)
            let center = min(max((a + b) / 2, span.0 + width / 2), span.1 - width / 2)
            let bottom = min(max(boardTop - 0.5, 0), trayHeight - 1)
            let height = parts.heights[reference].map { max(1, (boardTop - bottom) + $0.above + 0.5) } ?? 5
            return (reference, WallCutout(side: side, center: center, width: width, bottom: bottom, height: height))
        }
    }

    /// A net's tracks solved as resistors between these pads (indices into `pads`), or nil when
    /// the copper doesn't join them.
    func netFlow(net: Int32, pins: [Int]) -> (edges: [cc_flow_edge], unit: [Float])? {
        guard let board else { return nil }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let flow = cc_net_flow_make(board, net, pins, pins.count, &buffer, capacity) else { return nil }
        defer { cc_net_flow_free(flow) }
        let count = cc_net_flow_edge_count(flow)
        let edges = (0..<count).map { cc_net_flow_edge_at(flow, $0) }
        var unit: [Float] = []
        unit.reserveCapacity(count * pins.count)
        for edge in 0..<count {
            for pin in 0..<pins.count { unit.append(Float(cc_net_flow_unit(flow, edge, pin))) }
        }
        return (edges, unit)
    }

    func commitParameter(_ name: String, value: Double) {
        guard MechanicalSession.enabled else { rebuild(); return }
        guard mechanicalReady, let mechanical else { return }
        do {
            let before = try mechanical.settings
            // A completed drag with no effective change must not add an undo step.
            let values: [String: Double] = ["margin": before.margin, "wall": before.wall, "floor": before.floor,
                "board_clearance": before.board_clearance, "standoff_height": before.standoff_height,
                "standoff_diameter": before.standoff_diameter, "lid_thickness": before.lid_thickness, "lid_gap": before.lid_gap]
            guard values[name] != value else { settings = before; return }
            try mechanical.set(name, value: value)
            mechanicalDirty = true
            try refreshMechanical()
            rebuild()
        } catch {
            // A rejected transaction keeps the last valid settings, preview and history.
            try? refreshMechanical()
            self.error = error.localizedDescription
        }
    }

    func stepMechanical(forward: Bool) {
        guard mechanicalReady, let mechanical else { return }
        do {
            if forward { try mechanical.redo() } else { try mechanical.undo() }
            mechanicalDirty = true
            try refreshMechanical()
            rebuild()
        } catch { self.error = error.localizedDescription }
    }

    func saveMechanical() {
        guard mechanicalReady, let mechanical else { return }
        do {
            try mechanical.save()
            mechanicalSavedURL = mechanical.documentURL
            mechanicalDirty = false
        } catch { self.error = error.localizedDescription }
    }

    func rebuild() {
        guard board != nil || box != nil else { return }
        exportURL = nil
        lidExportURL = nil
        if MechanicalSession.enabled && !mechanicalReady { scene = SCNScene(); return }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let next = SCNScene()
        // Kinds: 1 board, 2 standoffs, 3 floor, 4 walls, 5 rim, 6 lid.
        let colors: [Int: UIColor] = [1: UIColor(hex: 0x0E3B2E), 2: UIColor(hex: 0xFFB300),
                                     3: UIColor(hex: 0x2A2A31), 4: UIColor(hex: 0x3A3A42),
                                     5: UIColor(hex: 0xC9C9D1), 6: UIColor(hex: 0x3FD8E0)]
        let kinds = MechanicalSession.enabled ? [3, 1, 6] : [4, 3, 5, 2, 1, 6]
        for kind in kinds {
            if kind == 6 && !showLid { continue }
            var triangles: [cc_triangle]
            if let mechanical, let mechanicalInput, MechanicalSession.enabled {
                do {
                    if kind == 1 {
                        if box != nil { continue }  // nothing inside a mechanical-only box
                        triangles = mechanicalInput.boardTriangles(bounds: bounds, settings: settings)
                    } else {
                        let part: Int32 = kind == 6 ? 1 : 0
                        triangles = try mechanical.triangles(part: part)
                        nativeTriangleCounts[Int(part)] = triangles.count
                    }
                } catch { self.error = error.localizedDescription; return }
            } else {
                guard let board else { return }
                let count = cc_model_triangles(board, settings, Int32(kind), nil, 0, &buffer, capacity)
                guard count > 0 || kind == 2 else { error = String(cString: buffer); return }
                if count == 0 { continue }
                triangles = [cc_triangle](repeating: cc_triangle(), count: count)
                let written = triangles.withUnsafeMutableBufferPointer {
                    cc_model_triangles(board, settings, Int32(kind), $0.baseAddress, $0.count, &buffer, capacity)
                }
                guard written == count else { error = String(cString: buffer); return }
            }
            let vertices: [SCNVector3] = triangles.flatMap { t in
                [SCNVector3(t.ax, t.az, -t.ay), SCNVector3(t.bx, t.bz, -t.by), SCNVector3(t.cx, t.cz, -t.cy)]
            }
            let source = SCNGeometrySource(vertices: vertices)
            let indices = (0..<UInt32(vertices.count)).map { $0 }
            let data = indices.withUnsafeBytes { Data($0) }
            let element = SCNGeometryElement(data: data, primitiveType: .triangles,
                                             primitiveCount: triangles.count, bytesPerIndex: MemoryLayout<UInt32>.size)
            let geometry = SCNGeometry(sources: [source], elements: [element])
            let material = SCNMaterial()
            material.diffuse.contents = colors[kind] ?? .systemGray
            material.isDoubleSided = true
            material.lightingModel = .physicallyBased
            material.metalness.contents = kind == 5 || kind == 2 ? 0.8 : 0.15
            material.roughness.contents = kind == 6 ? 0.15 : 0.5
            if kind == 1 { material.transparency = 0.85 }
            if kind == 6 { material.roughness.contents = 0.35 }
            geometry.materials = [material]
            let node = SCNNode(geometry: geometry)
            if kind == 6 {
                // Tinted acrylic lid: node opacity blends reliably with PBR materials.
                node.opacity = 0.3
                node.renderingOrder = 10
                if exploded { node.position.y = 7 }
            }
            next.rootNode.addChildNode(node)
        }
        if let board, box == nil {
            // Built even when hidden: the enclosure's clearance checks read the parts' heights.
            let bottom = Float(settings.floor + settings.standoff_height)
            let top = bottom + Float(MechanicalSession.enabled ? thickness ?? 1.6 : 1.6)
            let placements = (0..<cc_placement_count(board)).map { cc_placement_at(board, $0) }
            let node = parts.node(placements, top: top, bottom: bottom)
            if showParts { next.rootNode.addChildNode(node) }
        }
        let x0 = Double(bounds.minX), y0 = Double(bounds.minY), x1 = Double(bounds.maxX), y1 = Double(bounds.maxY)
        let span = max(x1 - x0, y1 - y0, 20)
        let target = SCNVector3(Float((x0+x1)/2), Float(settings.floor+settings.standoff_height), Float(-(y0+y1)/2))
        let camera = SCNCamera()
        camera.zFar = 10000
        let cameraNode = SCNNode()
        cameraNode.camera = camera
        cameraNode.position = SCNVector3(target.x + Float(span*0.65), target.y + Float(span*0.9), target.z + Float(span*1.2))
        cameraNode.look(at: target)
        next.rootNode.addChildNode(cameraNode)
        let key = SCNNode(), fill = SCNNode(), ambient = SCNNode()
        key.light = SCNLight(); key.light?.type = .directional; key.light?.intensity = 1400
        key.light?.color = UIColor(hex: 0xFFF1D6)
        key.eulerAngles = SCNVector3(-Float.pi / 3, Float.pi / 5, 0)
        fill.light = SCNLight(); fill.light?.type = .directional; fill.light?.intensity = 500
        fill.light?.color = UIColor(hex: 0x9FF8FF)
        fill.eulerAngles = SCNVector3(-Float.pi / 6, -Float.pi * 0.8, 0)
        ambient.light = SCNLight(); ambient.light?.type = .ambient; ambient.light?.intensity = 260
        [key, fill, ambient].forEach(next.rootNode.addChildNode)
        let floor = SCNFloor()
        floor.reflectivity = 0.12
        floor.reflectionFalloffEnd = 30
        floor.firstMaterial?.diffuse.contents = UIColor(hex: 0x07070A)
        floor.firstMaterial?.lightingModel = .constant
        let floorNode = SCNNode(geometry: floor)
        floorNode.position.y = -0.05
        next.rootNode.addChildNode(floorNode)
        next.background.contents = UIColor(hex: 0x030303)
        scene = next
        error = nil
    }

    func exportSTL() {
        if MechanicalSession.enabled {
            guard mechanicalReady, let mechanical else { return }
            do { exportURL = try mechanical.exportSTL(part: 0) }
            catch { self.error = error.localizedDescription }
            return
        }
        guard let board else { return }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("CorvCAD-Enclosure.stl")
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let ok = url.path.withCString { cc_export_stl(board, settings, $0, &buffer, capacity) }
        if ok == 1 { exportURL = url }
        else { error = String(cString: buffer) }
    }

    func exportLidSTL() {
        if MechanicalSession.enabled {
            guard mechanicalReady, let mechanical else { return }
            do { lidExportURL = try mechanical.exportSTL(part: 1) }
            catch { self.error = error.localizedDescription }
            return
        }
        guard let board else { return }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("CorvCAD-Lid.stl")
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let ok = url.path.withCString { cc_export_lid_stl(board, settings, $0, &buffer, capacity) }
        if ok == 1 { lidExportURL = url }
        else { error = String(cString: buffer) }
    }
}

/// Which parts a project has, in its corvcad.project.json. Without the file (projects made before
/// it, and imported boards) a project has an enclosure.
struct ProjectEngines: Codable, Equatable {
    var enclosure = true
    /// A mechanical-only project (FreeCAD edition): the inside size its enclosure is shaped
    /// around, and no KiCad files. Nil for an electronics project.
    var box: BoxSize?

    static let file = "corvcad.project.json"

    static func load(_ folder: URL) -> ProjectEngines {
        (try? JSONDecoder().decode(ProjectEngines.self, from: Data(contentsOf: folder.appendingPathComponent(file))))
            ?? ProjectEngines()
    }

    func save(to folder: URL) throws {
        try JSONEncoder().encode(self).write(to: folder.appendingPathComponent(Self.file), options: .atomic)
    }
}

/// A mechanical-only project's inside space, millimetres.
struct BoxSize: Codable, Equatable {
    var width: Double
    var depth: Double
    var height: Double
}

enum DesignError: LocalizedError {
    case message(String)
    var errorDescription: String? { if case let .message(text) = self { return text }; return nil }
}

/// App-wide undo/redo in edit order. Board edits rewrite a whole KiCad file, so their step
/// is the file's text before and after. Schematic edits are session transactions whose
/// own history the engine keeps; their step only marks where one happened.
@Observable
final class EditHistory {
    struct FileChange {
        let url: URL
        let before: String
        let after: String
    }
    enum Step {
        case file(url: URL, before: String, after: String)
        /// Several files changed by one edit (Board Setup: the board and its .kicad_pro); the
        /// first is the board to reload.
        case files([FileChange])
        /// Project library files (ProjectLibraries.snapshot): nil contents mean absent; folders
        /// are .pretty libraries. Undo returns the project folder (a directory URL).
        case library(folder: URL, [LibraryFile])
        case schematic
    }
    struct LibraryFile {
        let url: URL
        let before: Data?
        let after: Data?
        let folder: Bool
    }
    private(set) var undoStack: [Step] = []
    private(set) var redoStack: [Step] = []
    /// Matches the schematic session's 50-transaction history.
    private let limit = 50

    func record(_ url: URL, before: String, after: String) {
        guard before != after else { return }
        push(.file(url: url, before: before, after: after))
    }

    func record(_ changes: [FileChange]) {
        guard changes.contains(where: { $0.before != $0.after }) else { return }
        push(.files(changes))
    }

    func recordSchematic() { push(.schematic) }

    func recordLibrary(_ folder: URL, _ files: [LibraryFile]) {
        guard !files.isEmpty else { return }
        push(.library(folder: URL(fileURLWithPath: folder.path, isDirectory: true), files))
    }

    private func push(_ step: Step) {
        undoStack.append(step)
        if undoStack.count > limit { undoStack.removeFirst() }
        redoStack.removeAll()
    }

    func clear() { undoStack.removeAll(); redoStack.removeAll() }

    /// Applies the newest step. Returns a board file to reload, or nil for a schematic step.
    func undo(schematic: () throws -> Void) throws -> URL? {
        guard let step = undoStack.last else { return nil }
        let url = try apply(step, forward: false, schematic: schematic)
        redoStack.append(undoStack.removeLast())
        return url
    }

    func redo(schematic: () throws -> Void) throws -> URL? {
        guard let step = redoStack.last else { return nil }
        let url = try apply(step, forward: true, schematic: schematic)
        undoStack.append(redoStack.removeLast())
        return url
    }

    private func apply(_ step: Step, forward: Bool, schematic: () throws -> Void) throws -> URL? {
        switch step {
        case let .file(url, before, after):
            try Data((forward ? after : before).utf8).write(to: url, options: .atomic)
            return url
        case let .files(changes):
            for change in changes {
                try Data((forward ? change.after : change.before).utf8).write(to: change.url, options: .atomic)
            }
            return changes.first?.url
        case let .library(folder, files):
            let manager = FileManager.default
            for file in files where file.folder && (forward ? file.after : file.before) != nil {
                try manager.createDirectory(at: file.url, withIntermediateDirectories: true)
            }
            for file in files where !file.folder {
                if let data = forward ? file.after : file.before {
                    try data.write(to: file.url, options: .atomic)
                } else if manager.fileExists(atPath: file.url.path) {
                    try manager.removeItem(at: file.url)
                }
            }
            for file in files where file.folder && (forward ? file.after : file.before) == nil {
                if manager.fileExists(atPath: file.url.path) { try manager.removeItem(at: file.url) }
            }
            return folder
        case .schematic:
            try schematic()
            return nil
        }
    }
}

private struct ProjectPreferences: Codable {
    var margin: Double
    var wall: Double
    var floor: Double
    var boardClearance: Double
    var standoffHeight: Double
    var standoffDiameter: Double
    var lidThickness: Double
    var lidGap: Double
    var showLid: Bool
    var exploded: Bool

    init(_ design: Design) {
        margin = design.settings.margin
        wall = design.settings.wall
        floor = design.settings.floor
        boardClearance = design.settings.board_clearance
        standoffHeight = design.settings.standoff_height
        standoffDiameter = design.settings.standoff_diameter
        lidThickness = design.settings.lid_thickness
        lidGap = design.settings.lid_gap
        showLid = design.showLid
        exploded = design.exploded
    }

    func apply(to design: Design) {
        design.settings.margin = margin
        design.settings.wall = wall
        design.settings.floor = floor
        design.settings.board_clearance = boardClearance
        design.settings.standoff_height = standoffHeight
        design.settings.standoff_diameter = standoffDiameter
        design.settings.lid_thickness = lidThickness
        design.settings.lid_gap = lidGap
        design.showLid = showLid
        design.exploded = exploded
    }
}

struct ModelPreview: UIViewRepresentable {
    let scene: SCNScene
    func makeUIView(context: Context) -> SCNView {
        let view = SCNView(frame: .zero, options: [
            SCNView.Option.preferredRenderingAPI.rawValue: NSNumber(value: SCNRenderingAPI.metal.rawValue)
        ])
        view.allowsCameraControl = true
        view.antialiasingMode = .multisampling4X
        view.backgroundColor = UIColor(hex: 0x030303)
        view.isAccessibilityElement = true
        view.accessibilityLabel = "Enclosure preview"
        return view
    }
    func updateUIView(_ view: SCNView, context: Context) { view.scene = scene }
}

struct ContentView: View {
    @State private var design = Design()
    @State private var schematic = SchematicDesign()
    @State private var electrification = Electrification()
    @State private var history = EditHistory()
    @State private var importing = false
    @State private var importingSchematic = false
    /// KiCad's importers: other CAD files picked for a new project, and what the import reported.
    @State private var importingOther = false
    @State private var importReport: String?
    @State private var mode = WorkspaceMode.schematic
    @State private var inWorkspace = false
    @State private var creatingProject = false
    @State private var projectName = ""
    @State private var projectWidth = 80.0
    /// The new-project sheet's Enclosure and Electronics cards, and a mechanical-only box's height.
    @State private var projectEnclosure = true
    @State private var projectElectronics = true
    @State private var projectBoxHeight = 30.0
    /// The open project's parts; imported boards have every mode.
    @State private var engines = ProjectEngines()
    @State private var projectHeight = 60.0
    @State private var projectError: String?
    @State private var projects: [URL] = []
    @State private var currentProject: URL?
    @State private var pendingProject: URL?
    @State private var partReference = "R1"
    @State private var partValue = "10k"
    /// Outcome of the last Update PCB from schematic, shown as an alert.
    @State private var boardUpdateReport: String?
    /// The KiCad library part the schematic Part tool places (NATIVE_KICAD edition).
    @State private var chosenPart: PartLibrary.Part?
    @State private var showPartPicker = false
    /// Loaded on first use: the bundled index of KiCad's official symbols.
    private static let partLibrary = PartLibrary.bundled()
    private static let footprintLibrary = FootprintLibrary.bundled()
    /// The open project's libraries, and the pickers' indexes with their parts first; rebuilt
    /// when the project opens or a library changes.
    @State private var projectLibraries = ProjectLibraries()
    @State private var parts = ContentView.partLibrary
    @State private var footprints = ContentView.footprintLibrary
    @State private var showLibraries = false
    /// The Gerber viewer and the files or folder it opens with (empty: it starts with its Open
    /// button). An item, so the files arrive with the presentation.
    @State private var gerbers: GerberRequest?
    @State private var showCalculator = false
    /// The sidecar as last saved: the "before" side of the next enclosure undo step.
    @State private var savedPreferences = Data()

    private static let preferencesFile = "enclosure.corvcad.json"

    private var projectsDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Projects", isDirectory: true)
    }

    private var modes: [WorkspaceMode] {
        if engines.box != nil { return design.box != nil ? [.enclosure, .fabricate] : [] }
        return (schematic.loaded ? [.schematic] : [])
            + (design.width > 0 ? [.pcb] + (engines.enclosure ? [.enclosure] : []) + [.fabricate] : [])
    }

    private var activeMode: WorkspaceMode { modes.contains(mode) ? mode : modes.first ?? .schematic }

    private var sessionTitle: String? {
        guard !modes.isEmpty else { return nil }
        return currentProject?.lastPathComponent ?? (design.width > 0 ? design.boardName : schematic.name)
    }

    var body: some View {
        ZStack {
            if inWorkspace, let title = sessionTitle {
                workspace(title)
                    .transition(.asymmetric(insertion: .opacity.combined(with: .scale(scale: 1.02)),
                                            removal: .opacity))
            } else {
                HomeView(projects: projects, resumeTitle: sessionTitle,
                         onNew: { creatingProject = true },
                         onImportBoard: { importing = true },
                         onImportSchematic: { importingSchematic = true },
                         onOpen: openProject,
                         onResume: { inWorkspace = true },
                         onRename: renameProject,
                         onDelete: deleteProject,
                         onGerbers: { gerbers = GerberRequest(files: []) },
                         onCalculator: { showCalculator = true },
                         onImportOther: Design.hasKiCadDRC && cc_board_native_backend() != nil ? { importingOther = true } : nil)
                    .transition(.opacity)
            }
        }
        .overlay { if schematic.simulationRunning { SimulationRunningOverlay() } }
        .animation(.easeInOut(duration: 0.35), value: inWorkspace)
        .preferredColorScheme(.dark)
        .tint(Corv.gold)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item]) { result in
            switch result {
            case .success(let url):
                guard !MechanicalSession.enabled || !design.mechanicalDirty else {
                    design.error = "Resume and save the current enclosure before importing another file."
                    return
                }
                currentProject = nil
                engines = ProjectEngines()
                history.clear()
                design.setMechanicalProject(nil)
                design.load(url)
                if design.error == nil && design.width > 0 { mode = .pcb; inWorkspace = true }
            case .failure(let error): design.error = error.localizedDescription
            }
        }
        .fileImporter(isPresented: $importingSchematic, allowedContentTypes: [.item]) { result in
            switch result {
            case .success(let url):
                guard !MechanicalSession.enabled || !design.mechanicalDirty else {
                    design.error = "Resume and save the current enclosure before importing another file."
                    return
                }
                currentProject = nil
                history.clear()
                if MechanicalSession.enabled {
                    design.setMechanicalProject(nil)
                    design.rebuild()
                }
                schematic.load(url)
                if schematic.error == nil && schematic.loaded { mode = .schematic; inWorkspace = true }
            case .failure(let error): schematic.error = error.localizedDescription
            }
        }
        .alert("CorvCAD", isPresented: Binding(
            get: { design.error != nil || schematic.error != nil },
            set: { if !$0 { design.error = nil; schematic.error = nil } }
        )) {
            Button("OK") { design.error = nil; schematic.error = nil }
        } message: { Text(schematic.error ?? design.error ?? "") }
        .fileImporter(isPresented: $importingOther, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            switch result {
            case .success(let urls): importOtherCAD(urls)
            case .failure(let error): design.error = error.localizedDescription
            }
        }
        .alert("Imported with KiCad", isPresented: Binding(
            get: { importReport != nil },
            set: { if !$0 { importReport = nil } }
        )) {
            Button("OK") { importReport = nil }
        } message: { Text(importReport ?? "") }
        .alert("Update PCB", isPresented: Binding(
            get: { boardUpdateReport != nil },
            set: { if !$0 { boardUpdateReport = nil } }
        )) {
            Button("OK") { boardUpdateReport = nil }
        } message: { Text(boardUpdateReport ?? "") }
        .sheet(isPresented: $showPartPicker) {
            if let library = parts {
                PartPicker(library: library) { chosenPart = $0 }
            }
        }
        .confirmationDialog("Save enclosure changes?", isPresented: Binding(
            get: { pendingProject != nil }, set: { if !$0 { pendingProject = nil } }
        ), titleVisibility: .visible) {
            if let folder = pendingProject {
                Button("Save and open project") {
                    design.saveMechanical()
                    if !design.mechanicalDirty { loadProject(folder) }
                    pendingProject = nil
                }
                Button("Discard changes and open", role: .destructive) {
                    loadProject(folder)
                    pendingProject = nil
                }
            }
            Button("Cancel", role: .cancel) { pendingProject = nil }
        } message: {
            Text("The current enclosure has changes that have not been saved.")
        }
        .fullScreenCover(item: $gerbers) { GerberViewer(initial: $0.files) }
        .fullScreenCover(isPresented: $showCalculator) { PCBCalculatorView() }
        // Full screen: the browser's grid and the editor's canvas need the width.
        .fullScreenCover(isPresented: $showLibraries) {
            if let folder = currentProject {
                LibrariesSheet(folder: folder, libraries: projectLibraries,
                               pickersListProjectParts: Self.partLibrary != nil && schematic.placesLibrarySymbols,
                               perform: { changeLibraries(in: folder, $0) })
            }
        }
        .sheet(isPresented: $creatingProject) {
            NewProjectSheet(name: $projectName, width: $projectWidth, height: $projectHeight,
                            enclosure: $projectEnclosure, electronics: $projectElectronics, boxHeight: $projectBoxHeight,
                            error: projectError,
                            onCancel: { creatingProject = false },
                            onCreate: createProject,
                            onDismissError: { projectError = nil })
        }
        .onAppear {
            refreshProjects()
            design.onCommit = history.record
            design.onCommitFiles = history.record
            schematic.onSessionCommit = history.recordSchematic
            #if CORVCAD_DIAGNOSTICS
            Diagnostics.install()
            #endif
            #if DEBUG
            // Simulator automation: SIMCTL_CHILD_CORVCAD_OPEN=<project folder>, optional
            // CORVCAD_MODE (schematic/pcb/enclosure) and CORVCAD_SHEET (child sheet UUID).
            let environment = ProcessInfo.processInfo.environment
            // CORVCAD_GERBERS=<folder under Documents>: the Gerber viewer on that folder.
            if let folder = environment["CORVCAD_GERBERS"] {
                let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                gerbers = GerberRequest(files: [documents.appendingPathComponent(folder, isDirectory: true)])
            }
            // CORVCAD_IMPORT=<files under Documents, "|" between them>: KiCad's importers on them.
            if let files = environment["CORVCAD_IMPORT"], Design.hasKiCadDRC, cc_board_native_backend() != nil {
                let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                importOtherCAD(files.split(separator: "|").map { documents.appendingPathComponent(String($0)) })
            }
            if let name = environment["CORVCAD_OPEN"] {
                openProject(projectsDirectory.appendingPathComponent(name, isDirectory: true))
                if let raw = environment["CORVCAD_MODE"],
                   let requested = WorkspaceMode.allCases.first(where: { $0.rawValue.lowercased() == raw }) {
                    mode = requested
                }
                if let sheet = environment["CORVCAD_SHEET"] { schematic.enterSheet(sheet) }
                if environment["CORVCAD_DRC_FIXTURE"] != nil {
                    design.drc = DRCResult.fixture(at: design.footprintMarkers.map { CGPoint(x: $0.0, y: $0.1) })
                    design.selectedFinding = 0
                }
            }
            #endif
        }
    }

    private func stepHistory(forward: Bool) {
        if MechanicalSession.enabled && activeMode == .enclosure {
            design.stepMechanical(forward: forward)
            return
        }
        do {
            let url = forward ? try history.redo(schematic: schematic.redo)
                              : try history.undo(schematic: schematic.undo)
            if let url, url.hasDirectoryPath {
                // A library step: the project's library files changed back.
                reloadProjectLibraries()
            } else if let url, url.lastPathComponent == Self.preferencesFile {
                // An enclosure step: re-apply the sidecar and rebuild the preview.
                let data = try Data(contentsOf: url)
                try JSONDecoder().decode(ProjectPreferences.self, from: data).apply(to: design)
                savedPreferences = data
                design.rebuild()
            } else if let url {
                design.load(url)
                chooseNextPartReference()
            }
        } catch {
            design.error = error.localizedDescription
        }
    }

    private func workspace(_ title: String) -> some View {
        WorkspaceShell(title: title,
                       subtitle: currentProject == nil ? "Imported · read-only" : "Local project",
                       modes: modes,
                       mode: Binding(get: { activeMode }, set: { mode = $0 }),
                       shareItems: shareItems,
                       history: currentProject == nil ? nil : WorkspaceHistory(
                           canUndo: MechanicalSession.enabled && activeMode == .enclosure
                               ? design.mechanicalReady && design.mechanicalCanUndo : !history.undoStack.isEmpty,
                           canRedo: MechanicalSession.enabled && activeMode == .enclosure
                               ? design.mechanicalReady && design.mechanicalCanRedo : !history.redoStack.isEmpty,
                           undo: { stepHistory(forward: false) }, redo: { stepHistory(forward: true) }),
                       showsInspector: activeMode != .fabricate,
                       onBack: { refreshProjects(); inWorkspace = false }) {
            switch activeMode {
            case .fabricate:
                FabricateView(design: design, schematic: schematic,
                              projectBoard: engines.box == nil ? currentProject.map { projectFile($0, "kicad_pcb") } : nil,
                              projectFolder: currentProject, electronics: engines.box == nil,
                              onAddEnclosure: engines.enclosure ? nil : addEnclosure)
            case .enclosure: enclosureCanvas
            case .pcb:
                PCBOverview(boardBounds: design.bounds, outline: design.outline,
                            tracks: design.tracks, vias: design.vias, pads: design.pads,
                            nets: design.nets, footprints: design.footprintMarkers,
                            editable: currentProject != nil,
                            partReference: $partReference, partValue: $partValue,
                            onAddTwoPad: addBoardTwoPad, onAddHole: addBoardHole,
                            onSelectFootprint: design.selectFootprint,
                            onMoveFootprint: moveBoardFootprint,
                            onDeleteFootprint: deleteBoardFootprint,
                            onRenameFootprint: renameBoardFootprint,
                            onRotateFootprint: rotateBoardFootprint,
                            onUpdateFromSchematic: updateBoardFromSchematic,
                            onAddTrack: { start, end, layer, net in
                                guard let folder = currentProject else { return }
                                design.addTrack(from: start, to: end, layer: layer, net: net,
                                                to: projectFile(folder, "kicad_pcb"))
                            },
                            onAddVia: { point, net in
                                guard let folder = currentProject else { return }
                                design.addVia(at: point, net: net, to: projectFile(folder, "kicad_pcb"))
                            },
                            onDeleteCopper: { uuid in
                                guard let folder = currentProject else { return }
                                design.deleteCopper(uuid, to: projectFile(folder, "kicad_pcb"))
                            },
                            onDraw: { drawing in
                                guard let folder = currentProject else { return }
                                design.draw(drawing, to: projectFile(folder, "kicad_pcb"))
                            },
                            onMoveDrawing: { uuid, offset, zone in
                                guard let folder = currentProject else { return }
                                design.moveDrawing(uuid, by: offset, zone: zone, to: projectFile(folder, "kicad_pcb"))
                            },
                            onEditText: { uuid, text, size in
                                guard let folder = currentProject else { return }
                                design.setText(uuid, text: text, size: size, to: projectFile(folder, "kicad_pcb"))
                            },
                            onBoardEdit: Design.hasKiCadDRC && cc_board_native_backend() != nil ? { fields in
                                guard let folder = currentProject else { return }
                                design.nativeEdit(fields, to: projectFile(folder, "kicad_pcb"))
                                if fields.first == "place", design.error == nil { chooseNextPartReference() }
                            } : nil,
                            footprintLibrary: Design.hasKiCadDRC && cc_board_native_backend() != nil ? footprints : nil,
                            zones: design.zones,
                            graphics: design.graphics,
                            texts: design.texts,
                            ratsnest: design.ratsnest,
                            findings: design.drc?.findings ?? [],
                            selectedFinding: design.selectedFinding,
                            route: design.routing,
                            onRoute: currentProject != nil && Design.hasKiCadRouter ? { action in
                                guard let folder = currentProject else { return }
                                design.route(action, board: projectFile(folder, "kicad_pcb"))
                            } : nil)
            case .schematic:
                SchematicOverview(symbols: schematic.symbols, wires: schematic.wires,
                                  junctions: schematic.junctions, labels: schematic.labels,
                                  noConnects: schematic.noConnects, nets: schematic.nets,
                                  sheets: schematic.sheets, trail: schematic.trail,
                                  editable: currentProject != nil && schematic.readOnlyReason == nil,
                                  readOnlyReason: currentProject == nil ? nil : schematic.readOnlyReason,
                                  onAddTwoPin: addSchematicTwoPin,
                                  onAddWire: schematic.addWire,
                                  onAddLabel: schematic.addLabel,
                                  onSelectSymbol: schematic.select,
                                  onDelete: schematic.deleteItem,
                                  onRenameSymbol: schematic.renameSymbol,
                                  onRenameLabel: schematic.renameLabel,
                                  onMoveSymbol: schematic.moveSymbol,
                                  onMoveLabel: schematic.moveLabel,
                                  onSetFields: { schematic.setFields($0, value: $1, footprint: $2) },
                                  onRotateSymbol: schematic.rotateSymbol,
                                  onRoute: schematic.route,
                                  onEnterSheet: schematic.enterSheet,
                                  onCrumb: schematic.goToCrumb,
                                  document: schematic.name,
                                  chosenPart: schematic.placesLibrarySymbols ? chosenPart?.id : nil,
                                  onChoosePart: schematic.placesLibrarySymbols && Self.partLibrary != nil
                                      ? { showPartPicker = true } : nil,
                                  drawings: schematic.drawings,
                                  page: schematic.page,
                                  footprints: schematic.placesLibrarySymbols ? footprints : nil,
                                  parts: parts,
                                  buses: schematic.buses,
                                  busEntries: schematic.busEntries,
                                  sheetPins: schematic.sheetPins,
                                  kicadTools: schematic.kicadTools,
                                  onAddBus: schematic.addBus,
                                  onAddBusEntry: { schematic.addBusEntry(x: $0, y: $1, quadrant: $2) },
                                  onAddScopedLabel: { schematic.addLabel($0, x: $1, y: $2, kind: $3) },
                                  onAddSheet: schematic.addSheet,
                                  onAddSheetPins: schematic.addSheetPins,
                                  texts: schematic.texts,
                                  noteLines: schematic.noteLines,
                                  onAddNoConnect: { schematic.addNoConnect(x: $0, y: $1) },
                                  onMirrorSymbol: { schematic.mirrorSymbol($0, axis: $1) },
                                  onAddText: { schematic.addText($0, x: $1, y: $2) },
                                  onAddNoteLine: { schematic.addNoteLine(x0: $0, y0: $1, x1: $2, y1: $3) },
                                  onAddBusBusEntry: { schematic.addBusBusEntry(x: $0, y: $1, quadrant: $2) },
                                  onResizeSheet: { schematic.resizeSheet($0, width: $1, height: $2) },
                                  onChoosePower: parts.map { library in { (id: String) in
                                      if let part = library.part(id) { chosenPart = part } else { schematic.error = "\(id) is not in the bundled libraries." }
                                  } },
                                  ercMarks: (schematic.erc ?? []).compactMap { finding -> (point: CGPoint, error: Bool, selected: Bool)? in
                                      guard let point = finding.location, schematic.ercOnDisplayedSheet(finding) else { return nil }
                                      return (point: point, error: finding.isError, selected: finding.id == schematic.selectedERC)
                                  },
                                  onProbeNet: schematic.showsSimulationPlot ? schematic.probeNet : nil,
                                  onProbeSymbol: schematic.showsSimulationPlot ? schematic.probeSymbol : nil)
                .overlay(alignment: .bottom) {
                    if schematic.showsSimulationPlot, let run = schematic.simulation {
                        SimulationPlot(schematic: schematic, run: run,
                                       onShowOnBoard: run.animatable && !design.pads.isEmpty && modes.contains(.enclosure)
                                           ? { showOnBoard(run) } : nil)
                    }
                }
            }
        } inspector: {
            switch activeMode {
            case .fabricate:
                EmptyView()
            case .enclosure:
                EnclosureInspector(design: design) { name, value in
                    design.commitParameter(name, value: value)
                    saveProjectPreferences()
                }
            case .pcb:
                BoardInspector(design: design, projectBoard: currentProject.map { projectFile($0, "kicad_pcb") })
            case .schematic:
                SchematicInspector(schematic: schematic)
                if currentProject != nil {
                    InspectorSection("Libraries") {
                        StatGrid(items: [
                            Stat("Symbol libs", "\(projectLibraries.symbols.count)"),
                            Stat("Footprint libs", "\(projectLibraries.footprints.count)"),
                        ])
                        Button { showLibraries = true } label: {
                            Label("Project libraries", systemImage: "books.vertical")
                        }
                        .buttonStyle(.corvGhost)
                        .accessibilityIdentifier("projectLibraries")
                    }
                }
            }
        }
    }

    /// Runs one library change (an engine call returning its error or nil) as one undo step.
    private func changeLibraries(in folder: URL, _ change: () -> String?) -> String? {
        let before = ProjectLibraries.snapshot(folder)
        let error = change()
        history.recordLibrary(folder, ProjectLibraries.changes(folder, from: before, to: ProjectLibraries.snapshot(folder)))
        reloadProjectLibraries()
        return error
    }

    /// Re-reads the project's libraries after it opens or a library changed, and has KiCad
    /// (NATIVE_KICAD edition) reload the project's library tables and libraries.
    private func reloadProjectLibraries() {
        guard let folder = currentProject else {
            projectLibraries = ProjectLibraries()
            parts = Self.partLibrary
            footprints = Self.footprintLibrary
            return
        }
        cc_board_native_reload_libraries()
        let loaded = ProjectLibraries.load(folder)
        projectLibraries = loaded.libraries
        parts = Self.partLibrary?.adding(loaded.libraries.parts)
        footprints = Self.footprintLibrary?.adding(loaded.libraries.footprintEntries)
        if let error = loaded.error { design.error = error }
    }

    private func showOnBoard(_ run: SimulationResult) {
        mode = .enclosure
        design.showLid = false // it would tint the copper
        design.rebuild() // the canvas isn't on screen yet to see the change
        electrification.start(design: design, run: run, autoplay: !UIAccessibility.isReduceMotionEnabled)
    }

    private var enclosureCanvas: some View {
        ModelPreview(scene: design.scene)
            .accessibilityIdentifier("enclosurePreview")
            // The board's copper belongs to the scene: a rebuilt scene needs it again.
            .onChange(of: design.scene) {
                if electrification.enabled, let run = schematic.simulation {
                    electrification.start(design: design, run: run, autoplay: !UIAccessibility.isReduceMotionEnabled)
                }
            }
            .onChange(of: schematic.simulation == nil) { _, gone in if gone { electrification.stop() } }
            .overlay(alignment: .bottom) {
                if let run = schematic.simulation, run.animatable, !design.pads.isEmpty {
                    ElectrifyBar(electrification: electrification, design: design, run: run)
                        .padding(.horizontal, 16).padding(.bottom, 84)
                }
            }
            .overlay {
                if MechanicalSession.enabled, let message = design.mechanicalError {
                    Text(message).font(.callout).foregroundStyle(Corv.textDim)
                        .padding(24).corvPanel(radius: 18).padding(40)
                        .accessibilityIdentifier("enclosureUnavailable")
                }
            }
            .overlay(alignment: .topLeading) {
                HStack(spacing: 8) {
                    MonoLabel(design.box == nil ? "Board" : "Inside", color: Corv.textFaint, small: true)
                    Text(design.box.map { String(format: "%.0f × %.0f × %.0f mm", $0.width, $0.depth, $0.height) }
                         ?? String(format: "%.1f × %.1f mm", design.width, design.height))
                        .font(.corvReadout).foregroundStyle(Corv.cyan)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .corvPanel(radius: 14)
                .padding(18)
            }
            .overlay(alignment: .topTrailing) {
                if design.showParts, design.box == nil,
                   design.parts.generated + design.parts.placeholders > 0 || design.parts.generating {
                    PartsStatus(parts: design.parts).padding(18)
                }
            }
            .overlay(alignment: .bottom) {
                ChipBar {
                    if design.box == nil { pill("Parts", icon: "cpu", isOn: $design.showParts) }
                    pill("Lid", icon: "square.stack.3d.up", isOn: $design.showLid)
                    pill("Exploded", icon: "arrow.up.and.down", isOn: $design.exploded)
                        .disabled(!design.showLid).opacity(design.showLid ? 1 : 0.4)
                    MonoLabel("Drag to orbit · pinch to zoom", color: Corv.textFaint, small: true)
                        .padding(.horizontal, 8)
                }
                .padding(.bottom, 18)
            }
            .onChange(of: design.showParts) { design.rebuild() }
            .onChange(of: design.showLid) { design.rebuild(); saveProjectPreferences() }
            .onChange(of: design.exploded) { design.rebuild(); saveProjectPreferences() }
            #if DEBUG
            .overlay(alignment: .bottomLeading) {
                if MechanicalSession.enabled {
                    Color.clear.frame(width: 1, height: 1)
                        .allowsHitTesting(false)
                        .accessibilityElement()
                        .accessibilityIdentifier("mechanicalProbe")
                        .accessibilityLabel(design.mechanicalDiagnostic)
                }
            }
            #endif
    }

    private func pill(_ title: String, icon: String, isOn: Binding<Bool>) -> some View {
        Button { isOn.wrappedValue.toggle() } label: {
            Label(title, systemImage: icon).font(.system(size: 14, weight: .semibold))
                .padding(.horizontal, 14).padding(.vertical, 9)
                .foregroundStyle(isOn.wrappedValue ? Corv.void : Corv.text)
                .background(Capsule().fill(isOn.wrappedValue ? AnyShapeStyle(Corv.gold) : AnyShapeStyle(Color.white.opacity(0.05))))
        }
        .buttonStyle(.plain)
        .animation(.easeOut(duration: 0.15), value: isOn.wrappedValue)
        .accessibilityAddTraits(isOn.wrappedValue ? .isSelected : [])
        .sensoryFeedback(.selection, trigger: isOn.wrappedValue)
    }

    private var shareItems: [ShareItem] {
        var items: [ShareItem] = []
        if let currentProject {
            items += [ShareItem(title: "KiCad project settings", url: projectFile(currentProject, "kicad_pro")),
                      ShareItem(title: "KiCad schematic", url: projectFile(currentProject, "kicad_sch")),
                      ShareItem(title: "KiCad board", url: projectFile(currentProject, "kicad_pcb"))]
        }
        if let url = schematic.editedURL { items.append(ShareItem(title: "Edited schematic copy", url: url)) }
        if let url = design.editedBoardURL { items.append(ShareItem(title: "Edited board copy", url: url)) }
        if let url = design.exportURL { items.append(ShareItem(title: "Tray STL", url: url)) }
        if let url = design.lidExportURL { items.append(ShareItem(title: "Lid STL", url: url)) }
        if let url = design.mechanicalSavedURL { items.append(ShareItem(title: "Enclosure document", url: url)) }
        return items
    }

    private func projectFile(_ folder: URL, _ extensionName: String) -> URL {
        folder.appendingPathComponent(folder.lastPathComponent).appendingPathExtension(extensionName)
    }

    private func refreshProjects() {
        let folders = (try? FileManager.default.contentsOfDirectory(
            at: projectsDirectory, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles])) ?? []
        projects = folders.filter { folder in
            (try? folder.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true &&
            ProjectEngines.load(folder).box != nil ||
            FileManager.default.fileExists(atPath: projectFile(folder, "kicad_pro").path) &&
            FileManager.default.fileExists(atPath: projectFile(folder, "kicad_sch").path) &&
            FileManager.default.fileExists(atPath: projectFile(folder, "kicad_pcb").path)
        }.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    private func openProject(_ folder: URL) {
        if MechanicalSession.enabled {
            if currentProject?.standardizedFileURL == folder.standardizedFileURL {
                inWorkspace = true
                return
            }
            if design.mechanicalDirty, design.mechanical != nil {
                pendingProject = folder
                return
            }
        }
        loadProject(folder)
    }

    private func loadProject(_ folder: URL) {
        currentProject = nil
        history.clear()
        design.settings = cc_default_settings()
        design.showLid = true
        design.exploded = true
        engines = ProjectEngines.load(folder)
        if let box = engines.box {
            guard MechanicalSession.enabled else {
                design.error = "This mechanical-only project needs the FreeCAD edition."
                engines = ProjectEngines()
                return
            }
            schematic = SchematicDesign()
            schematic.onSessionCommit = history.recordSchematic
            design.setMechanicalProject(folder)
            design.loadBox(box, name: folder.lastPathComponent)
            guard design.error == nil else { return }
            currentProject = folder
            mode = .enclosure
            inWorkspace = true
            return
        }
        // An electronics-only project never starts FreeCAD.
        design.setMechanicalProject(engines.enclosure ? folder : nil)
        let preferencesURL = folder.appendingPathComponent(Self.preferencesFile)
        if !MechanicalSession.enabled, let data = try? Data(contentsOf: preferencesURL),
           let preferences = try? JSONDecoder().decode(ProjectPreferences.self, from: data) {
            preferences.apply(to: design)
        }
        savedPreferences = (try? JSONEncoder().encode(ProjectPreferences(design))) ?? Data()
        design.load(projectFile(folder, "kicad_pcb"))
        guard design.error == nil else { return }
        schematic.openProject(projectFile(folder, "kicad_pro"))
        guard schematic.error == nil else { return }
        currentProject = folder
        reloadProjectLibraries()
        chooseNextPartReference()
        mode = .schematic
        inWorkspace = true
    }

    private func prepareLibraries(_ folder: URL) -> Bool {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let prepared = folder.path.withCString {
            cc_prepare_project_libraries($0, &buffer, capacity)
        }
        if prepared == 0 { design.error = String(cString: buffer); return false }
        return true
    }

    private func chooseNextPartReference() {
        let used = Set(design.footprintMarkers.map { $0.2 })
        for number in 1...9999 where !used.contains("R\(number)") {
            partReference = "R\(number)"
            return
        }
    }

    private func addSchematicTwoPin(_ x: Double, _ y: Double) {
        guard let folder = currentProject, prepareLibraries(folder) else { return }
        if let chosenPart, schematic.placesLibrarySymbols {
            schematic.addLibrarySymbol(chosenPart.id, x: x, y: y)
        } else {
            schematic.addTwoPin(x: x, y: y)
        }
    }

    private func addBoardTwoPad(_ reference: String, _ value: String,
                                _ x: Double, _ y: Double) {
        guard let folder = currentProject, prepareLibraries(folder) else { return }
        design.addTwoPad(reference: reference, value: value, x: x, y: y,
                         to: projectFile(folder, "kicad_pcb"))
        if design.error == nil { chooseNextPartReference() }
    }

    private func addBoardHole(_ x: Double, _ y: Double) {
        guard let folder = currentProject, prepareLibraries(folder) else { return }
        design.addMountingHole(x: x, y: y, to: projectFile(folder, "kicad_pcb"))
    }

    private func moveBoardFootprint(_ reference: String, _ x: Double, _ y: Double) {
        guard let folder = currentProject else { return }
        design.moveFootprint(reference, x: x, y: y, to: projectFile(folder, "kicad_pcb"))
    }

    private func deleteBoardFootprint(_ reference: String) {
        guard let folder = currentProject else { return }
        design.deleteFootprint(reference, to: projectFile(folder, "kicad_pcb"))
        if design.error == nil { chooseNextPartReference() }
    }

    /// Carries the saved root schematic's nets onto board pads and adds missing two-pad parts.
    private func updateBoardFromSchematic() {
        guard let folder = currentProject, prepareLibraries(folder) else { return }
        // NATIVE_KICAD edition: KiCad's own updater with the session's netlist (any footprint).
        if Design.hasKiCadDRC, let netlist = schematic.kicadNetlist() {
            guard let report = design.updateFromNetlist(netlist, board: projectFile(folder, "kicad_pcb")) else { return }
            chooseNextPartReference()
            let lines = report.split(separator: "\n").prefix(12)
            boardUpdateReport = lines.isEmpty ? "The board already matches the schematic." : lines.joined(separator: "\n")
            return
        }
        do {
            let source = try String(contentsOf: projectFile(folder, "kicad_sch"), encoding: .utf8)
            guard let report = design.updateFromSchematic(source, to: projectFile(folder, "kicad_pcb")) else { return }
            chooseNextPartReference()
            boardUpdateReport = (["Pad nets now match the schematic."] + (report.isEmpty ? [] : [report]))
                .joined(separator: " ")
        } catch {
            design.error = error.localizedDescription
        }
    }

    private func rotateBoardFootprint(_ reference: String) {
        guard let folder = currentProject else { return }
        design.rotateFootprint(reference, to: projectFile(folder, "kicad_pcb"))
    }

    private func renameBoardFootprint(_ reference: String, _ updated: String) {
        guard let folder = currentProject else { return }
        design.renameFootprint(reference, as: updated, to: projectFile(folder, "kicad_pcb"))
        if design.error == nil { chooseNextPartReference() }
    }

    /// Writes the sidecar and records the change, so undo/redo restore enclosure parameters.
    private func saveProjectPreferences() {
        guard !MechanicalSession.enabled, let currentProject else { return }
        do {
            let data = try JSONEncoder().encode(ProjectPreferences(design))
            let url = currentProject.appendingPathComponent(Self.preferencesFile)
            try data.write(to: url, options: .atomic)
            history.record(url, before: String(decoding: savedPreferences, as: UTF8.self),
                           after: String(decoding: data, as: UTF8.self))
            savedPreferences = data
        } catch {
            design.error = error.localizedDescription
        }
    }

    /// Long press on a project card: the engine renames the folder and its KiCad files, which
    /// keep referring to each other. An open project is closed first.
    private func renameProject(_ folder: URL, to name: String) {
        let hasEnclosure = FileManager.default.fileExists(atPath: folder.appendingPathComponent("enclosure.FCStd").path)
            || (currentProject?.standardizedFileURL == folder.standardizedFileURL && design.mechanical != nil)
        if MechanicalSession.enabled && hasEnclosure {
            design.error = "Renaming a project with a native enclosure is not supported yet. The project has not been changed."
            return
        }
        closeIfOpen(folder)
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let newName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let renamed = projectsDirectory.path.withCString { parent in
            folder.lastPathComponent.withCString { old in
                newName.withCString { cc_rename_project(parent, old, $0, &buffer, capacity) }
            }
        }
        if renamed != 1 { design.error = String(cString: buffer) }
        refreshProjects()
    }

    /// Deletes the project folder (confirmed on the home screen). An open project is closed first.
    private func deleteProject(_ folder: URL) {
        closeIfOpen(folder)
        do {
            try FileManager.default.removeItem(at: folder)
        } catch {
            design.error = error.localizedDescription
        }
        refreshProjects()
    }

    private func closeIfOpen(_ folder: URL) {
        guard currentProject?.standardizedFileURL == folder.standardizedFileURL else { return }
        currentProject = nil
        history.clear()
        design = Design()
        schematic = SchematicDesign()
        design.onCommit = history.record
        design.onCommitFiles = history.record
        schematic.onSessionCommit = history.recordSchematic
    }

    /// Gives an electronics-only project its enclosure (FreeCAD starts with the board's reload).
    private func addEnclosure() {
        guard let folder = currentProject else { return }
        do {
            try ProjectEngines(enclosure: true).save(to: folder)
        } catch {
            design.error = error.localizedDescription
            return
        }
        engines.enclosure = true
        design.setMechanicalProject(folder)
        design.load(projectFile(folder, "kicad_pcb"))
        mode = .enclosure
    }

    private func createProject() {
        let name = projectName.trimmingCharacters(in: .whitespacesAndNewlines)
        do {
            try FileManager.default.createDirectory(at: projectsDirectory,
                                                    withIntermediateDirectories: true)
        } catch {
            projectError = error.localizedDescription
            return
        }
        if !projectElectronics {
            createBox(name)
            return
        }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let created = projectsDirectory.path.withCString { parent in
            name.withCString { project in
                cc_create_project(parent, project, projectWidth, projectHeight, &buffer, capacity)
            }
        }
        guard created == 1 else { projectError = String(cString: buffer); return }
        let folder = projectsDirectory.appendingPathComponent(name, isDirectory: true)
        do {
            try ProjectEngines(enclosure: projectEnclosure).save(to: folder)
        } catch {
            projectError = error.localizedDescription
            return
        }
        refreshProjects()
        openProject(folder)
        if let error = design.error ?? schematic.error {
            projectError = error
            design.error = nil
            schematic.error = nil
            return
        }
        creatingProject = false
        projectName = ""
    }

    /// KiCad's importers (NATIVE_KICAD): a new project named after the first file, each picked
    /// file imported as its schematic (cc_sch_import) and/or board (cc_board_native_import; an
    /// EasyEDA Pro file holds both), then opened. Other CAD formats need no enclosure, so the
    /// project starts without one.
    private func importOtherCAD(_ urls: [URL]) {
        guard let first = urls.first else { return }
        guard !MechanicalSession.enabled || !design.mechanicalDirty else {
            design.error = "Save the current enclosure before importing."
            return
        }
        let base = String(first.deletingPathExtension().lastPathComponent
            .map { $0.isLetter || $0.isNumber || $0 == " " || $0 == "-" || $0 == "_" ? $0 : "_" })
            .trimmingCharacters(in: .whitespaces)
        var name = base.isEmpty ? "Imported" : base
        var copy = 2
        while FileManager.default.fileExists(atPath: projectsDirectory.appendingPathComponent(name).path) {
            name = "\(base) \(copy)"; copy += 1
        }
        // Picked files are security-scoped: the importers read copies.
        let staging = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: staging) }
        do {
            try FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: projectsDirectory, withIntermediateDirectories: true)
            for url in urls {
                let access = url.startAccessingSecurityScopedResource()
                defer { if access { url.stopAccessingSecurityScopedResource() } }
                try FileManager.default.copyItem(at: url, to: staging.appendingPathComponent(url.lastPathComponent))
            }
        } catch {
            design.error = error.localizedDescription
            return
        }
        // The open project's schematic session would keep KiCad's project active.
        if let currentProject { closeIfOpen(currentProject) }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let created = projectsDirectory.path.withCString { parent in
            name.withCString { cc_create_project(parent, $0, 100, 80, &buffer, capacity) }
        }
        guard created == 1 else { design.error = String(cString: buffer); return }
        let folder = projectsDirectory.appendingPathComponent(name, isDirectory: true)
        try? ProjectEngines(enclosure: false).save(to: folder)
        var lines: [String] = [], failures: [String] = []
        for url in urls {
            let file = staging.appendingPathComponent(url.lastPathComponent).path
            var result = cc_sch_result()
            if let report = cc_sch_import(file, "auto", projectFile(folder, "kicad_sch").path, &result) {
                let f = String(cString: report).split(separator: "\t").map(String.init)
                cc_free_text(report)
                if f.count >= 3 { lines.append("\(url.lastPathComponent): schematic, \(f[1]) sheets, \(f[2]) symbols (\(f[0]))") }
            } else if Int(result.code) != Int(CC_SCH_INVALID.rawValue), let message = result.message {
                failures.append("\(url.lastPathComponent): \(String(cString: message))")
            }
            cc_sch_result_clear(&result)
            if let report = cc_board_native_import(file, "auto", projectFile(folder, "kicad_pcb").path, &buffer, capacity) {
                let f = String(cString: report).split(separator: "\t").map(String.init)
                cc_free_text(report)
                if f.count >= 5 { lines.append("\(url.lastPathComponent): board, \(f[1]) footprints, \(f[2]) tracks, \(f[3]) vias, \(f[4]) zones (\(f[0]))") }
            } else if !String(cString: buffer).hasPrefix("Not a board format") {
                failures.append("\(url.lastPathComponent): \(String(cString: buffer))")
            }
        }
        guard !lines.isEmpty else {
            try? FileManager.default.removeItem(at: folder)
            design.error = (["KiCad could not import these files."] + failures).joined(separator: "\n")
            return
        }
        refreshProjects()
        openProject(folder)
        importReport = (lines + failures).joined(separator: "\n")
    }

    /// A mechanical-only project: a folder with its corvcad.project.json; FreeCAD makes the
    /// enclosure document when it opens.
    private func createBox(_ name: String) {
        let box = BoxSize(width: projectWidth, depth: projectHeight, height: projectBoxHeight)
        guard name.range(of: "^[A-Za-z0-9 _-]+$", options: .regularExpression) != nil else {
            projectError = "Use letters, numbers, spaces, hyphens or underscores."
            return
        }
        guard [box.width, box.depth, box.height].allSatisfy({ $0.isFinite && $0 >= 1 && $0 <= 1000 }) else {
            projectError = "The box needs a width, depth and height from 1 to 1000 mm."
            return
        }
        let folder = projectsDirectory.appendingPathComponent(name, isDirectory: true)
        guard !FileManager.default.fileExists(atPath: folder.path) else {
            projectError = "A project with this name already exists."
            return
        }
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: false)
            try ProjectEngines(enclosure: true, box: box).save(to: folder)
        } catch {
            projectError = error.localizedDescription
            return
        }
        refreshProjects()
        openProject(folder)
        if let error = design.error {
            projectError = error
            design.error = nil
            return
        }
        creatingProject = false
        projectName = ""
    }
}

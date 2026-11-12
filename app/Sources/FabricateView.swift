import SwiftUI

/// The Fabricate workspace: what will be made (board and enclosure specs) and every output this
/// edition can write, in one place. KiCad's outputs appear only where KiCad is linked; STEP and the
/// FreeCAD document only where FreeCAD is.
struct FabricateView: View {
    let design: Design
    let schematic: SchematicDesign
    /// The project's board file; nil for an individually imported board.
    let projectBoard: URL?
    let projectFolder: URL?
    /// False for a mechanical-only project: no board and no electronics outputs.
    var electronics = true
    /// Set for an electronics-only project: adds the enclosure. Nil when it has one.
    var onAddEnclosure: (() -> Void)?
    @State private var material = PrintMaterial.pla
    @State private var parts: EnclosureParts?
    @State private var rules: BoardSetup?
    @State private var files: [String: [URL]] = [:]
    @State private var working: String?
    @State private var failure: String?

    private var nativeBoard: URL? { cc_board_native_backend() != nil ? projectBoard : nil }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                header
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 360), spacing: 20, alignment: .top)], spacing: 20) {
                    if electronics { boardCard }
                    enclosureCard
                }
                if let failure { CautionNote(text: failure) }
                outputs(electronics ? "Electronics" : "Project", tiles: electronicsTiles)
                if onAddEnclosure == nil {
                    outputs("Enclosure", tiles: enclosureTiles)
                }
            }
            .frame(maxWidth: 1180, alignment: .leading)
            .padding(24)
            .frame(maxWidth: .infinity)
        }
        .scrollIndicators(.hidden)
        .background(Corv.void)
        .task(id: enclosureKey) { parts = onAddEnclosure == nil ? EnclosureParts.measure(design) : nil }
        .task(id: nativeBoard) { rules = nativeBoard.flatMap(design.boardSetup(of:)) }
    }

    // MARK: Specs

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            MonoLabel("Fabricate", color: Corv.gold)
            Text(projectFolder?.lastPathComponent ?? design.boardName).font(.corvDisplay(30)).foregroundStyle(Corv.text)
            MonoLabel([design.copperLayers.map { "\($0)-layer board" },
                       electronics ? String(format: "%.1f × %.1f mm", design.width, design.height) : nil,
                       parts.map { String(format: "enclosure %.0f × %.0f × %.0f mm", $0.tray.size.x, $0.tray.size.y, $0.height) }]
                .compactMap { $0 }.joined(separator: "  ·  "), color: Corv.cyan, small: true)
        }
        .accessibilityElement(children: .combine)
    }

    private var boardCard: some View {
        card("Board") {
            StatGrid(items: [
                Stat("Size mm", String(format: "%.1f×%.1f", design.width, design.height)),
                Stat("Thickness mm", design.thickness.map { String(format: "%.2f", $0) } ?? "—"),
                Stat("Copper layers", design.copperLayers.map(String.init) ?? "—"),
                Stat("Footprints", "\(design.placements.count)"),
                Stat("Pads", "\(design.pads.count)"),
                Stat("Nets", "\(design.nets.count)"),
                Stat("Vias", "\(design.vias.count)"),
                Stat("Mounting holes", "\(design.holes.count)"),
            ] + (Design.hasKiCadDRC ? [Stat("Unrouted", "\(design.ratsnest.count)"),
                                       Stat("DRC", design.drc.map { "\($0.errors) err" } ?? "Not run")] : [])
              + (schematic.runsERC ? [Stat("ERC", schematic.erc.map { "\($0.filter(\.isError).count) err" } ?? "Not run")] : []))
            let drills = Set(design.vias.map(\.drill)).sorted()
            if !drills.isEmpty {
                row("Via drills", drills.map { String(format: "%.2f", $0) }.joined(separator: " · ") + " mm")
            }
            if let netClass = rules?.classes.first {
                row("Default netclass", String(format: "track %.2f · clearance %.2f · via %.2f/%.2f mm",
                                               netClass.track, netClass.clearance, netClass.via, netClass.drill))
            }
            CautionNote(text: Design.hasKiCadDRC
                ? "DRC and ERC run from the PCB and schematic inspectors; edits clear their results."
                : "Run KiCad's DRC and ERC on a desktop before ordering boards.")
        }
    }

    private var enclosureCard: some View {
        card("Enclosure") {
            if let onAddEnclosure {
                Text("This project has no enclosure yet. Add one to shape a tray and lid around the board.")
                    .font(.callout).foregroundStyle(Corv.textDim)
                Button(action: onAddEnclosure) { Label("Add enclosure", systemImage: "plus") }
                    .buttonStyle(.corvPrimary)
                    .accessibilityIdentifier("addEnclosure")
            } else if let parts {
                let solid = parts.volume / 1000
                StatGrid(items: [
                    Stat("Tray mm", parts.tray.label),
                    Stat("Lid mm", parts.lid?.label ?? "—"),
                    Stat("Wall · floor mm", String(format: "%.1f · %.1f", design.settings.wall, design.settings.floor)),
                    Stat("Volume cm³", String(format: "%.1f", solid)),
                    Stat("Mass g", String(format: "%.0f", solid * material.density)),
                    Stat(material.printed ? "Filament m" : "Process", material.printed
                         ? String(format: "%.1f", parts.volume / (Double.pi * 0.875 * 0.875) / 1000) : "Machined"),
                ])
                .accessibilityIdentifier("enclosureMeasured")
                Picker("Material", selection: $material) {
                    ForEach(PrintMaterial.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                .accessibilityIdentifier("material")
                CautionNote(text: "Sizes and volume come from the exported STL meshes. Mass and filament (1.75 mm) assume solid parts; a slicer's walls and infill change both.")
            } else {
                Text(MechanicalSession.enabled && !design.mechanicalReady ? "Enclosure unavailable" : "Measuring…")
                    .font(.callout).foregroundStyle(Corv.textDim)
            }
        }
    }

    // MARK: Outputs

    private var electronicsTiles: [Tile] {
        var tiles: [Tile] = []
        if let folder = projectFolder {
            tiles.append(Tile(id: "project", title: electronics ? "KiCad project" : "Project", detail: "Folder as .zip", icon: "folder") {
                [try FileZip.zip(folder, name: folder.lastPathComponent)]
            })
        }
        if let board = nativeBoard {
            tiles.append(Tile(id: "fab", title: "Fabrication package", detail: "Gerbers · drill · placement",
                              icon: "shippingbox") {
                await design.exportFabrication(of: board, refillZones: false)
                return design.fabZipURL.map { [$0] } ?? []
            })
            tiles.append(Tile(id: "boardPDF", title: "Board PDF", detail: "Plot layers", icon: "doc.richtext") {
                await design.plotBoard(board, pdf: true).map { [$0] } ?? []
            })
            tiles.append(Tile(id: "boardSVG", title: "Board SVG", detail: "One per layer, .zip", icon: "square.on.square.dashed") {
                await design.plotBoard(board, pdf: false).map { [$0] } ?? []
            })
        }
        if schematic.plots {
            tiles.append(Tile(id: "schPDF", title: "Schematic PDF", detail: "Every sheet", icon: "doc.text") {
                schematic.plot(svg: false)
            })
            tiles.append(Tile(id: "schSVG", title: "Schematic SVG", detail: "One per sheet", icon: "square.on.square") {
                schematic.plot(svg: true)
            })
        }
        if schematic.kicadTools {
            tiles.append(Tile(id: "bom", title: "Bill of materials", detail: "CSV, by value and footprint",
                              icon: "list.bullet.rectangle") { schematic.exportBOM().map { [$0] } ?? [] })
            tiles.append(Tile(id: "netlist", title: "Netlist", detail: "KiCad s-expression",
                              icon: "point.3.connected.trianglepath.dotted") { schematic.exportNetlist().map { [$0] } ?? [] })
        }
        return tiles
    }

    private var enclosureTiles: [Tile] {
        var tiles = [
            Tile(id: "traySTL", title: "Tray STL", detail: "Mesh", icon: "cube") {
                design.exportSTL(); return design.exportURL.map { [$0] } ?? []
            },
            Tile(id: "lidSTL", title: "Lid STL", detail: "Mesh", icon: "square.stack.3d.up") {
                design.exportLidSTL(); return design.lidExportURL.map { [$0] } ?? []
            },
            Tile(id: "obj", title: "OBJ", detail: "Tray and lid, one file", icon: "cube.transparent") {
                guard let parts = EnclosureParts.measure(design) else { throw DesignError.message("The enclosure has no mesh.") }
                return [try parts.writeOBJ(named: projectFolder?.lastPathComponent ?? "CorvCAD")]
            },
        ]
        if MechanicalSession.enabled {
            tiles.insert(Tile(id: "step", title: "STEP", detail: "Exact solids · tray and lid", icon: "cube.fill") {
                guard design.mechanicalReady, let mechanical = design.mechanical else {
                    throw DesignError.message("The enclosure is unavailable.")
                }
                return [try mechanical.exportSTEP(part: 0), try mechanical.exportSTEP(part: 1)]
            }, at: 0)
            tiles.append(Tile(id: "fcstd", title: "FreeCAD document", detail: "Saved .FCStd", icon: "doc.badge.gearshape") {
                if design.mechanicalSavedURL == nil { design.saveMechanical() }
                return design.mechanicalSavedURL.map { [$0] } ?? []
            })
        }
        return tiles
    }

    private func outputs(_ title: String, tiles: [Tile]) -> some View {
        InspectorSection("\(title) outputs") {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 200), spacing: 14)], spacing: 14) {
                ForEach(tiles) { tile in
                    OutputTile(tile: tile, files: files[tile.id] ?? [], busy: working == tile.id,
                               disabled: working != nil) { run(tile) }
                }
            }
        }
    }

    private func run(_ tile: Tile) {
        guard let action = tile.action else { return }
        working = tile.id
        failure = nil
        Task {
            do {
                let urls = try await action()
                files[tile.id] = urls
                if urls.isEmpty { failure = design.drcError ?? design.error ?? schematic.error ?? "\(tile.title) wrote nothing." }
            } catch {
                failure = error.localizedDescription
            }
            working = nil
        }
    }

    // MARK: Helpers

    private var enclosureKey: String {
        let s = design.settings
        return "\(design.boardSource.count) \(design.mechanicalReady) \(s.margin) \(s.wall) \(s.floor) \(s.board_clearance) "
            + "\(s.standoff_height) \(s.standoff_diameter) \(s.lid_thickness) \(s.lid_gap)"
    }

    private func card<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
        InspectorSection(title) { content() }
            .padding(20)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .corvPanel(radius: 22)
    }

    private func row(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel(title, color: Corv.textFaint, small: true)
            Text(value).font(.corvReadout).foregroundStyle(Corv.cyan)
        }
        .accessibilityElement(children: .combine)
    }
}

/// One output: an action that writes files.
private struct Tile: Identifiable {
    let id: String
    let title: String
    let detail: String
    let icon: String
    var action: (() async throws -> [URL])?
}

private struct OutputTile: View {
    let tile: Tile
    let files: [URL]
    let busy: Bool
    let disabled: Bool
    let export: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Image(systemName: tile.icon).font(.system(size: 18, weight: .medium))
                    .foregroundStyle(Corv.gold)
                Spacer()
                if busy {
                    ProgressView().tint(Corv.gold)
                } else if !files.isEmpty {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(Corv.cyan)
                }
            }
            Text(tile.title).font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Corv.text)
            MonoLabel(tile.detail, color: Corv.textFaint, small: true)
            Spacer(minLength: 4)
            if files.isEmpty {
                Button(busy ? "Exporting…" : "Export", action: export)
                    .buttonStyle(.corvGhost)
                    .disabled(disabled)
                    .accessibilityIdentifier("export-\(tile.id)")
            } else {
                ShareLink(items: files) {
                    Label(files.count == 1 ? files[0].lastPathComponent : "\(files.count) files",
                          systemImage: "square.and.arrow.up")
                        .font(.caption.weight(.semibold)).lineLimit(1)
                }
                .foregroundStyle(Corv.gold)
                .accessibilityIdentifier("share-\(tile.id)")
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, minHeight: 168, alignment: .topLeading)
        .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 16))
        .overlay(RoundedRectangle(cornerRadius: 16).strokeBorder(Corv.lineStrong))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(tile.title)
    }
}

/// Densities for the mass estimate, g/cm³ (typical datasheet values).
enum PrintMaterial: String, CaseIterable, Identifiable {
    case pla = "PLA", petg = "PETG", abs = "ABS", asa = "ASA", pc = "PC", aluminium = "Aluminium"
    var id: Self { self }
    var density: Double {
        switch self {
        case .pla: return 1.24
        case .petg: return 1.27
        case .abs: return 1.04
        case .asa: return 1.07
        case .pc: return 1.20
        case .aluminium: return 2.70
        }
    }
    var printed: Bool { self != .aluminium }
}

/// A triangle mesh read from an STL file (binary or ASCII), three vertices per triangle.
struct STLMesh {
    let vertices: [SIMD3<Double>]

    init?(contentsOf url: URL) {
        guard let data = try? Data(contentsOf: url) else { return nil }
        var points: [SIMD3<Double>] = []
        let binaryCount = data.count >= 84
            ? Int(data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 80, as: UInt32.self).littleEndian }) : -1
        if binaryCount >= 0, data.count == 84 + binaryCount * 50 {
            points.reserveCapacity(binaryCount * 3)
            data.withUnsafeBytes { bytes in
                for triangle in 0..<binaryCount {
                    for corner in 0..<3 {
                        let offset = 84 + triangle * 50 + 12 + corner * 12
                        let xyz = (0..<3).map { Double(Float(bitPattern: bytes.loadUnaligned(fromByteOffset: offset + $0 * 4,
                                                                                              as: UInt32.self).littleEndian)) }
                        points.append(SIMD3(xyz[0], xyz[1], xyz[2]))
                    }
                }
            }
        } else {
            let words = String(decoding: data, as: UTF8.self).split(whereSeparator: \.isWhitespace)
            var index = 0
            while index + 3 < words.count {
                if words[index] == "vertex", let x = Double(words[index + 1]), let y = Double(words[index + 2]),
                   let z = Double(words[index + 3]) {
                    points.append(SIMD3(x, y, z))
                    index += 4
                } else {
                    index += 1
                }
            }
        }
        guard !points.isEmpty, points.count % 3 == 0 else { return nil }
        vertices = points
    }

    var size: SIMD3<Double> {
        let lower = vertices.reduce(vertices[0]) { pointwiseMin($0, $1) }
        let upper = vertices.reduce(vertices[0]) { pointwiseMax($0, $1) }
        return upper - lower
    }

    /// Enclosed volume, mm³ (divergence theorem; exact for the closed mesh).
    var volume: Double {
        var sum = 0.0
        for index in stride(from: 0, to: vertices.count, by: 3) {
            let a = vertices[index], b = vertices[index + 1], c = vertices[index + 2]
            sum += a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x)
        }
        return abs(sum) / 6
    }

    var label: String { String(format: "%.0f×%.0f×%.0f", size.x, size.y, size.z) }
}

/// The enclosure's tray and lid as exported (the STL files the app shares).
struct EnclosureParts {
    let tray: STLMesh
    let lid: STLMesh?
    var volume: Double { tray.volume + (lid?.volume ?? 0) }
    /// Closed height: tray plus lid thickness.
    var height: Double { tray.size.z + (lid?.size.z ?? 0) }

    @MainActor
    static func measure(_ design: Design) -> EnclosureParts? {
        guard !MechanicalSession.enabled || design.mechanicalReady else { return nil }
        design.exportSTL()
        design.exportLidSTL()
        guard let tray = design.exportURL.flatMap(STLMesh.init(contentsOf:)) else { return nil }
        return EnclosureParts(tray: tray, lid: design.lidExportURL.flatMap(STLMesh.init(contentsOf:)))
    }

    /// Both parts in one OBJ, as objects "tray" and "lid", millimetres.
    func writeOBJ(named name: String) throws -> URL {
        var text = "# CorvCAD enclosure, millimetres\n"
        var offset = 0
        for (object, mesh) in [("tray", tray), ("lid", lid)] {
            guard let mesh else { continue }
            var index: [SIMD3<Double>: Int] = [:]
            var faces = ""
            text += "o \(object)\n"
            for start in stride(from: 0, to: mesh.vertices.count, by: 3) {
                let corners = mesh.vertices[start..<start + 3].map { point -> Int in
                    if let known = index[point] { return known }
                    index[point] = offset + index.count + 1
                    text += String(format: "v %.5f %.5f %.5f\n", point.x, point.y, point.z)
                    return offset + index.count
                }
                faces += "f \(corners[0]) \(corners[1]) \(corners[2])\n"
            }
            text += faces
            offset += index.count
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(name)-enclosure.obj")
        try Data(text.utf8).write(to: url, options: .atomic)
        return url
    }
}

enum FileZip {
    /// A folder as <name>.zip in the temporary directory (the system's zip, as Files shares it).
    static func zip(_ folder: URL, name: String) throws -> URL {
        let zip = FileManager.default.temporaryDirectory.appendingPathComponent("\(name).zip")
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
        if let failure = coordinationError ?? copyError { throw failure }
        return zip
    }
}

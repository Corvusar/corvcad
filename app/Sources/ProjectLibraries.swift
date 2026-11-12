import SwiftUI
import UniformTypeIdentifiers

/// A pad of a library footprint, in footprint millimetres.
struct LibraryPad {
    let center: CGPoint
    let size: CGSize
    let rotation: Double
    let shape: String
    let number: String
    let type: String
}

/// A symbol or footprint of a project library, as the engine lists it
/// (corvcad::project_libraries_json): fields, pins and a drawing.
struct LibraryItem: Identifiable {
    var id: String { name }
    var name = ""
    var reference = "", value = "", footprint = "", description = "", filters = ""
    /// Derived (extends) symbols are listed but not edited here.
    var derived = false
    /// Number, name, KiCad electrical type, side ("left", "right", "top", "bottom").
    var pins: [[String]] = []
    /// Symbol drawing in sheet millimetres (y down): body polylines and pin lines.
    var graphics: [(closed: Bool, fill: Int, points: [CGPoint])] = []
    var pinLines: [(end: CGPoint, body: CGPoint, number: String, name: String)] = []
    var smd = false
    var pads: [LibraryPad] = []

    init() {}

    init(_ row: [String: Any]) {
        name = row["name"] as? String ?? ""
        reference = row["reference"] as? String ?? ""
        value = row["value"] as? String ?? ""
        footprint = row["footprint"] as? String ?? ""
        description = row["description"] as? String ?? ""
        filters = row["filters"] as? String ?? ""
        derived = row["derived"] as? Bool ?? false
        smd = row["smd"] as? Bool ?? false
        pins = (row["pins"] as? [[String]]) ?? []
        let number = { (value: Any) in CGFloat((value as? NSNumber)?.doubleValue ?? 0) }
        if let shape = row["shape"] as? [String: Any] {
            for case let graphic as [Any] in shape["graphics"] as? [Any] ?? [] where graphic.count >= 2 {
                let coordinates = graphic.dropFirst(2).map(number)
                let points = stride(from: 0, to: coordinates.count - 1, by: 2).map {
                    CGPoint(x: coordinates[$0], y: coordinates[$0 + 1])
                }
                graphics.append(((graphic[0] as? NSNumber)?.intValue == 1, (graphic[1] as? NSNumber)?.intValue ?? 0, points))
            }
            for case let pin as [Any] in shape["pins"] as? [Any] ?? [] where pin.count == 6 {
                pinLines.append((CGPoint(x: number(pin[0]), y: number(pin[1])), CGPoint(x: number(pin[2]), y: number(pin[3])),
                                 pin[4] as? String ?? "", pin[5] as? String ?? ""))
            }
        }
        for case let pad as [Any] in row["pads"] as? [Any] ?? [] where pad.count == 8 {
            pads.append(LibraryPad(center: CGPoint(x: number(pad[0]), y: number(pad[1])),
                                   size: CGSize(width: number(pad[2]), height: number(pad[3])),
                                   rotation: Double(number(pad[4])), shape: pad[5] as? String ?? "",
                                   number: pad[6] as? String ?? "", type: pad[7] as? String ?? ""))
        }
    }

    /// Distinct pad numbers, as KiCad counts a footprint's pads.
    var padCount: Int { Set(pads.map(\.number).filter { !$0.isEmpty }).count }
}

/// A row of the project's sym-lib-table or fp-lib-table.
struct ProjectLibrary: Identifiable {
    let name: String
    let uri: String
    let editable: Bool
    let error: String
    let footprint: Bool
    let items: [LibraryItem]
    var id: String { (footprint ? "fp:" : "sym:") + name }
}

/// The project's libraries, read through the engine (both editions).
struct ProjectLibraries {
    var symbols: [ProjectLibrary] = []
    var footprints: [ProjectLibrary] = []

    static func load(_ folder: URL) -> (libraries: ProjectLibraries, error: String?) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let text = cc_project_libraries_json(folder.path, &buffer, capacity) else {
            return (ProjectLibraries(), String(cString: buffer))
        }
        defer { cc_free_text(text) }
        guard let root = try? JSONSerialization.jsonObject(with: Data(String(cString: text).utf8)) as? [String: Any] else {
            return (ProjectLibraries(), "Cannot read the project's libraries")
        }
        func libraries(_ key: String) -> [ProjectLibrary] {
            (root[key] as? [[String: Any]] ?? []).map { row in
                ProjectLibrary(name: row["name"] as? String ?? "", uri: row["uri"] as? String ?? "",
                               editable: row["editable"] as? Bool ?? false, error: row["error"] as? String ?? "",
                               footprint: key == "footprints",
                               items: (row["items"] as? [[String: Any]] ?? []).map(LibraryItem.init))
            }
        }
        return (ProjectLibraries(symbols: libraries("symbols"), footprints: libraries("footprints")), nil)
    }

    /// The project's symbols for the part picker (NATIVE_KICAD edition).
    var parts: [PartLibrary.Part] {
        symbols.flatMap { library in
            library.items.map { item in
                PartLibrary.Part(library: library.name, name: item.name, prefix: item.reference,
                                 description: item.description, keywords: "", footprint: item.footprint,
                                 pins: Set(item.pins.map { $0.first ?? "" }).count,
                                 filters: item.filters.split(separator: " ").map(String.init),
                                 haystack: "\(item.name) \(item.description) \(library.name)".lowercased())
            }
        }
    }

    var footprintEntries: [FootprintLibrary.Footprint] {
        footprints.flatMap { library in
            library.items.map {
                FootprintLibrary.Footprint(library: library.name, name: $0.name, pads: $0.padCount, description: $0.description)
            }
        }
    }

    /// Runs an engine library call; nil on success, else its message.
    static func run(_ call: (UnsafeMutablePointer<CChar>?, Int) -> Int32) -> String? {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        return call(&buffer, capacity) == 1 ? nil : String(cString: buffer)
    }

    /// Runs an engine call that returns text; the text, or nil with `error` set.
    static func text(_ error: inout String?, _ call: (UnsafeMutablePointer<CChar>?, Int) -> UnsafeMutablePointer<CChar>?) -> String? {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        guard let result = call(&buffer, capacity) else { error = String(cString: buffer); return nil }
        defer { cc_free_text(result) }
        error = nil
        return String(cString: result)
    }

    /// The files a library change can touch: both tables, the .kicad_sym files beside them and
    /// each .pretty folder with its .kicad_mod files, by path.
    struct Snapshot {
        var files: [String: Data] = [:]
        var folders: Set<String> = []
    }

    static func snapshot(_ folder: URL) -> Snapshot {
        var snapshot = Snapshot()
        let manager = FileManager.default
        for name in ["sym-lib-table", "fp-lib-table"] {
            let url = folder.appendingPathComponent(name)
            if let data = try? Data(contentsOf: url) { snapshot.files[url.path] = data }
        }
        for url in (try? manager.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [] {
            if url.pathExtension == "kicad_sym", let data = try? Data(contentsOf: url) { snapshot.files[url.path] = data }
            if url.pathExtension == "pretty" {
                snapshot.folders.insert(url.path)
                for file in (try? manager.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? []
                where file.pathExtension == "kicad_mod" {
                    if let data = try? Data(contentsOf: file) { snapshot.files[file.path] = data }
                }
            }
        }
        return snapshot
    }

    static func changes(_ folder: URL, from before: Snapshot, to after: Snapshot) -> [EditHistory.LibraryFile] {
        let folders = before.folders.symmetricDifference(after.folders).map { path in
            EditHistory.LibraryFile(url: URL(fileURLWithPath: path, isDirectory: true),
                                    before: before.folders.contains(path) ? Data() : nil,
                                    after: after.folders.contains(path) ? Data() : nil, folder: true)
        }
        let files = Set(before.files.keys).union(after.files.keys).filter { before.files[$0] != after.files[$0] }.map { path in
            EditHistory.LibraryFile(url: URL(fileURLWithPath: path), before: before.files[path], after: after.files[path],
                                    folder: false)
        }
        return folders + files
    }
}

/// The editor form of one symbol or footprint (corvcad::library_item_json): symbol coordinates
/// are KiCad's library millimetres (y up), footprint coordinates y down.
struct EditorForm {
    struct Shape {
        var i = 0, unit = 0, fill = 0
        var kind = "", layer = "", text = ""
        var closed = false
        var width = 0.0
        var points: [CGPoint] = []
    }
    struct Pin {
        var i = 0, unit = 0
        var number = "", name = "", type = ""
        var x = 0.0, y = 0.0, angle = 0.0, length = 0.0
        var hidden = false
        var end: CGPoint { CGPoint(x: x, y: y) }
        /// The body end: KiCad's pin points from its connection end along its angle.
        var body: CGPoint {
            CGPoint(x: x + length * cos(angle * .pi / 180), y: y + length * sin(angle * .pi / 180))
        }
    }
    struct Pad {
        var i = 0
        var number = "", type = "", shape = ""
        var x = 0.0, y = 0.0, rotation = 0.0, width = 0.0, height = 0.0, drill = 0.0
    }
    var name = "", description = "", attr = "", base = ""
    var derived = false
    var units = 1
    var fields: [String: String] = [:]
    var shapes: [Shape] = []
    var pins: [Pin] = []
    var pads: [Pad] = []

    init() {}

    init(json: String) {
        guard let row = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any] else { return }
        func number(_ value: Any?) -> Double { (value as? NSNumber)?.doubleValue ?? 0 }
        name = row["name"] as? String ?? ""
        description = row["description"] as? String ?? ""
        attr = row["attr"] as? String ?? ""
        base = row["base"] as? String ?? ""
        derived = row["derived"] as? Bool ?? false
        units = (row["units"] as? NSNumber)?.intValue ?? 1
        fields = row["fields"] as? [String: String] ?? [:]
        shapes = (row["shapes"] as? [[String: Any]] ?? []).map { s in
            let values = (s["points"] as? [Any] ?? []).map { CGFloat(number($0)) }
            return Shape(i: Int(number(s["i"])), unit: Int(number(s["unit"])), fill: Int(number(s["fill"])),
                         kind: s["kind"] as? String ?? "", layer: s["layer"] as? String ?? "", text: s["text"] as? String ?? "",
                         closed: s["closed"] as? Bool ?? false, width: number(s["width"]),
                         points: stride(from: 0, to: values.count - 1, by: 2).map { CGPoint(x: values[$0], y: values[$0 + 1]) })
        }
        pins = (row["pins"] as? [[String: Any]] ?? []).map { p in
            Pin(i: Int(number(p["i"])), unit: Int(number(p["unit"])), number: p["number"] as? String ?? "",
                name: p["name"] as? String ?? "", type: p["type"] as? String ?? "", x: number(p["x"]), y: number(p["y"]),
                angle: number(p["angle"]), length: number(p["length"]), hidden: p["hidden"] as? Bool ?? false)
        }
        pads = (row["pads"] as? [[String: Any]] ?? []).map { p in
            Pad(i: Int(number(p["i"])), number: p["number"] as? String ?? "", type: p["type"] as? String ?? "",
                shape: p["shape"] as? String ?? "", x: number(p["x"]), y: number(p["y"]), rotation: number(p["rot"]),
                width: number(p["w"]), height: number(p["h"]), drill: number(p["drill"]))
        }
    }
}

/// One item's KiCad text under edit, with the editor's own undo. Every change is one engine
/// edit (library.hpp), so whatever the editor does not model stays in the text.
@Observable
final class LibraryItemEditor {
    let footprint: Bool
    private(set) var text = ""
    /// A derived symbol's base, for drawing.
    private(set) var baseText: String?
    private(set) var form = EditorForm()
    var error: String?
    private var past: [String] = []
    private var future: [String] = []

    init(footprint: Bool) { self.footprint = footprint }

    var canUndo: Bool { !past.isEmpty }
    var canRedo: Bool { !future.isEmpty }

    /// Starts from a text (an item or a template); clears the editor's history unless kept.
    func load(_ text: String, base: String? = nil, keepHistory: Bool = false) {
        if keepHistory && !self.text.isEmpty { past.append(self.text); future.removeAll() }
        else { past.removeAll(); future.removeAll() }
        self.text = text
        baseText = base
        refresh()
    }

    func edit(_ fields: [String]) {
        var failure: String?
        let op = fields.joined(separator: "\t")
        guard let edited = ProjectLibraries.text(&failure, { cc_library_edit(footprint ? 1 : 0, text, op, $0, $1) }) else {
            error = failure
            return
        }
        past.append(text)
        future.removeAll()
        text = edited
        refresh()
    }

    func undo() { guard let last = past.popLast() else { return }; future.append(text); text = last; refresh() }
    func redo() { guard let next = future.popLast() else { return }; past.append(text); text = next; refresh() }

    private func refresh() {
        var failure: String?
        let json = ProjectLibraries.text(&failure) { cc_library_item_json(footprint ? 1 : 0, text, baseText, $0, $1) }
        form = json.map { EditorForm(json: $0) } ?? EditorForm()
        error = failure
    }
}

// MARK: - Drawings

/// A library symbol drawn to fit: body shapes, pin lines and (when large) pin numbers.
struct SymbolShapeView: View {
    let item: LibraryItem
    var detailed = false

    var body: some View {
        Canvas { context, size in
            var points = item.graphics.flatMap(\.points)
            points += item.pinLines.flatMap { [$0.end, $0.body] }
            guard let fit = Fit(points, in: size) else { return }
            for graphic in item.graphics where graphic.points.count > 1 {
                var path = Path()
                path.addLines(graphic.points.map(fit.map))
                if graphic.closed { path.closeSubpath() }
                if graphic.fill == 2 { context.fill(path, with: .color(Corv.gold.opacity(0.08))) }
                if graphic.fill == 1 { context.fill(path, with: .color(.white.opacity(0.7))) }
                context.stroke(path, with: .color(.white.opacity(0.9)), lineWidth: 1.2)
            }
            for pin in item.pinLines {
                var path = Path()
                path.move(to: fit.map(pin.end))
                path.addLine(to: fit.map(pin.body))
                context.stroke(path, with: .color(Corv.cyan.opacity(0.9)), lineWidth: 1.2)
                if detailed {
                    context.draw(Text(pin.number).font(.corvMonoSmall).foregroundStyle(Corv.textFaint),
                                 at: fit.map(CGPoint(x: (pin.end.x + pin.body.x) / 2, y: (pin.end.y + pin.body.y) / 2 - 0.9)))
                    if !pin.name.isEmpty && pin.name != "~" {
                        let inward = CGPoint(x: pin.body.x + (pin.body.x - pin.end.x) * 0.4, y: pin.body.y + (pin.body.y - pin.end.y) * 0.4)
                        context.draw(Text(pin.name).font(.corvMonoSmall).foregroundStyle(Corv.text), at: fit.map(inward))
                    }
                }
            }
        }
        .accessibilityLabel("\(item.name), \(item.pins.count) pins")
    }
}

/// A library footprint drawn to fit: pads, SMD gold and through-hole pale gold.
struct FootprintShapeView: View {
    let item: LibraryItem
    var detailed = false

    var body: some View {
        Canvas { context, size in
            let corners = item.pads.flatMap { pad -> [CGPoint] in
                let r = hypot(pad.size.width, pad.size.height) / 2
                return [CGPoint(x: pad.center.x - r, y: pad.center.y - r), CGPoint(x: pad.center.x + r, y: pad.center.y + r)]
            }
            guard let fit = Fit(corners, in: size) else { return }
            for pad in item.pads {
                let center = fit.map(pad.center)
                let width = pad.size.width * fit.scale, height = pad.size.height * fit.scale
                let rect = CGRect(x: -width / 2, y: -height / 2, width: width, height: height)
                var shape = Path(roundedRect: rect, cornerRadius: pad.shape == "roundrect" ? min(width, height) * 0.25 : 0)
                if pad.shape == "circle" || pad.shape == "oval" {
                    shape = Path(roundedRect: rect, cornerRadius: min(width, height) / 2)
                }
                var padContext = context
                padContext.translateBy(x: center.x, y: center.y)
                padContext.rotate(by: .degrees(-pad.rotation))
                padContext.fill(shape, with: .color(pad.type == "smd" ? Corv.gold : Corv.goldSoft))
                if detailed {
                    context.draw(Text(pad.number).font(.corvMonoSmall).foregroundStyle(Corv.void), at: center)
                }
            }
        }
        .accessibilityLabel("\(item.name), \(item.padCount) pads")
    }
}

/// Maps millimetres into a view, preserving aspect, with a small margin.
private struct Fit {
    let scale: CGFloat
    let origin: CGPoint
    let offset: CGPoint

    init(scale: CGFloat, origin: CGPoint, offset: CGPoint) {
        self.scale = scale
        self.origin = origin
        self.offset = offset
    }

    init?(_ points: [CGPoint], in size: CGSize) {
        guard let minX = points.map(\.x).min(), let maxX = points.map(\.x).max(),
              let minY = points.map(\.y).min(), let maxY = points.map(\.y).max(), size.width > 8, size.height > 8 else { return nil }
        let width = max(maxX - minX, 1), height = max(maxY - minY, 1)
        scale = min((size.width - 8) / width, (size.height - 8) / height)
        origin = CGPoint(x: minX, y: minY)
        offset = CGPoint(x: (size.width - width * scale) / 2, y: (size.height - height * scale) / 2)
    }

    func map(_ point: CGPoint) -> CGPoint {
        CGPoint(x: offset.x + (point.x - origin.x) * scale, y: offset.y + (point.y - origin.y) * scale)
    }

    func unmap(_ point: CGPoint) -> CGPoint {
        CGPoint(x: (point.x - offset.x) / scale + origin.x, y: (point.y - offset.y) / scale + origin.y)
    }
}

// MARK: - Library browser

/// The project's symbol and footprint libraries: import from Files (a .kicad_sym, a .pretty or
/// one .kicad_mod), new libraries, the item editor, deletion. Each change is one undo step.
struct LibrariesSheet: View {
    let folder: URL
    let libraries: ProjectLibraries
    /// The pickers list the project's parts (NATIVE_KICAD edition).
    let pickersListProjectParts: Bool
    /// Runs one library change (returning its error) as one undo step.
    let perform: (() -> String?) -> String?
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    /// The footprint library a single .kicad_mod goes into; nil imports a whole library.
    @State private var importInto: String?
    @State private var error: String?
    @State private var newLibrary: Bool?
    @State private var newName = ""
    @State private var editing: Editing?
    @State private var removing: ProjectLibrary?

    struct Editing: Identifiable {
        let library: String
        let footprint: Bool
        let item: String?
        var id: String { "\(footprint ? "f" : "s"):\(library):\(item ?? "+")" }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                MonoLabel("Project libraries", color: Corv.gold)
                Spacer()
                Button("Close") { dismiss() }.buttonStyle(.corvGhost)
            }
            HStack(spacing: 8) {
                Button { importInto = nil; importing = true } label: { Label("Import", systemImage: "square.and.arrow.down") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("importLibrary")
                Button { newName = ""; newLibrary = false } label: { Label("Symbol library", systemImage: "plus") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("newSymbolLibrary")
                Button { newName = ""; newLibrary = true } label: { Label("Footprint library", systemImage: "plus") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("newFootprintLibrary")
            }
            if let error {
                Text(error).font(.caption).foregroundStyle(Corv.red)
            }
            CautionNote(text: pickersListProjectParts
                ? "KiCad 10 library files in the project folder. The Part tool's picker and the footprint chooser list their parts. Every change is saved at once and can be undone from the workspace."
                : "KiCad 10 library files in the project folder, for KiCad. This edition's Part tool places CorvCAD's two-pin part only. Every change is saved at once and can be undone from the workspace.")
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    group("Symbols", libraries.symbols)
                    group("Footprints", libraries.footprints)
                }
            }
        }
        .padding(20)
        .background(Corv.void)
        .fileImporter(isPresented: $importing, allowedContentTypes: importInto == nil ? [.item, .folder] : [.item]) { result in
            switch result {
            case .success(let url):
                let access = url.startAccessingSecurityScopedResource()
                defer { if access { url.stopAccessingSecurityScopedResource() } }
                if let library = importInto {
                    change { cc_import_library_footprint(folder.path, library, url.path, $0, $1) }
                } else {
                    change { cc_import_project_library(folder.path, url.path, $0, $1) }
                }
            case .failure(let failure): error = failure.localizedDescription
            }
            importInto = nil
        }
        .alert(newLibrary == true ? "New footprint library" : "New symbol library", isPresented: Binding(
            get: { newLibrary != nil }, set: { if !$0 { newLibrary = nil } }
        )) {
            TextField("Name", text: $newName).textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Create") {
                let footprint = newLibrary == true
                change { cc_create_project_library(folder.path, footprint ? 1 : 0, newName, $0, $1) }
                newLibrary = nil
            }
            Button("Cancel", role: .cancel) { newLibrary = nil }
        } message: { Text("Letters, digits, _ - . +") }
        .confirmationDialog("Remove \(removing?.name ?? "")?", isPresented: Binding(
            get: { removing != nil }, set: { if !$0 { removing = nil } }
        ), titleVisibility: .visible) {
            Button("Remove library and its file", role: .destructive) {
                if let library = removing {
                    change { cc_remove_project_library(folder.path, library.footprint ? 1 : 0, library.name, $0, $1) }
                }
                removing = nil
            }
        } message: { Text("Parts already placed keep their copy in the schematic; the board's footprints stay. Undo brings the library back.") }
        .fullScreenCover(item: $editing) { target in
            LibraryItemEditorView(folder: folder, library: target.library, footprint: target.footprint, original: target.item,
                                  footprints: libraries.footprintEntries.map(\.id), perform: perform)
        }
    }

    private func change(_ call: (UnsafeMutablePointer<CChar>?, Int) -> Int32) {
        error = perform { ProjectLibraries.run(call) }
    }

    @ViewBuilder private func group(_ title: String, _ libraries: [ProjectLibrary]) -> some View {
        InspectorSection(title) {
            if libraries.isEmpty {
                Text("None in the project's library table.").font(.caption).foregroundStyle(Corv.textFaint)
            }
            ForEach(libraries) { library in libraryCard(library) }
        }
    }

    private func libraryCard(_ library: ProjectLibrary) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text(library.name).font(.corvReadout).foregroundStyle(Corv.text)
                MonoLabel("\(library.items.count)", color: Corv.cyan, small: true)
                if !library.editable { MonoLabel("read-only", color: Corv.textFaint, small: true) }
                Spacer()
                if library.editable {
                    if library.footprint {
                        Button { importInto = library.name; importing = true } label: { Image(systemName: "square.and.arrow.down") }
                            .buttonStyle(.corvGhost)
                            .accessibilityLabel("Import a .kicad_mod into \(library.name)")
                    }
                    Button { editing = Editing(library: library.name, footprint: library.footprint, item: nil) }
                        label: { Image(systemName: "plus") }
                        .buttonStyle(.corvGhost)
                        .accessibilityLabel(library.footprint ? "New footprint in \(library.name)" : "New symbol in \(library.name)")
                    Button { removing = library } label: { Image(systemName: "trash") }
                        .buttonStyle(.corvGhost)
                        .foregroundStyle(Corv.red)
                        .accessibilityLabel("Remove \(library.name)")
                }
            }
            if !library.error.isEmpty {
                Text(library.error).font(.caption).foregroundStyle(Corv.red)
            }
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 8)], spacing: 8) {
                ForEach(library.items) { item in itemCard(item, in: library) }
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Corv.line))
    }

    private func itemCard(_ item: LibraryItem, in library: ProjectLibrary) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Group {
                if library.footprint { FootprintShapeView(item: item) } else { SymbolShapeView(item: item) }
            }
            .frame(height: 70)
            HStack(spacing: 4) {
                Text(item.name).font(.corvMonoSmall).foregroundStyle(Corv.text).lineLimit(1)
                Spacer(minLength: 0)
                MonoLabel(library.footprint ? "\(item.padCount) pads" : item.derived ? "derived" : "\(item.pins.count) pins",
                          color: Corv.cyan, small: true)
            }
            if library.editable {
                HStack(spacing: 6) {
                    Button("Edit") { editing = Editing(library: library.name, footprint: library.footprint, item: item.name) }
                        .buttonStyle(.corvGhost)
                        .accessibilityLabel("Edit \(item.name)")
                    Button { change { cc_delete_library_item(folder.path, library.footprint ? 1 : 0, library.name, item.name, $0, $1) } }
                        label: { Image(systemName: "trash") }
                        .buttonStyle(.corvGhost)
                        .foregroundStyle(Corv.red)
                        .accessibilityLabel("Delete \(item.name)")
                }
            }
        }
        .padding(8)
        .background(Corv.void.opacity(0.6), in: .rect(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
    }
}

// MARK: - Item editor

/// Draws an editor form: symbol shapes and pins (library y up, drawn y down) or footprint pads
/// and shapes. Items of other units are dimmed; the selected item glows gold.
private struct EditorDrawing {
    let form: EditorForm
    let footprint: Bool
    let unit: Int

    /// Item millimetres as drawn (y down).
    func shown(_ p: CGPoint) -> CGPoint { footprint ? p : CGPoint(x: p.x, y: -p.y) }
    func stored(_ p: CGPoint) -> CGPoint { shown(p) }

    func visible(_ itemUnit: Int) -> Bool { footprint || itemUnit == 0 || itemUnit == unit }

    func bounds() -> [CGPoint] {
        var points = form.shapes.flatMap { $0.points.map(shown) }
        points += form.pins.flatMap { [shown($0.end), shown($0.body)] }
        points += form.pads.flatMap { pad in
            [CGPoint(x: pad.x - pad.width / 2, y: pad.y - pad.height / 2), CGPoint(x: pad.x + pad.width / 2, y: pad.y + pad.height / 2)]
        }
        // At least a few millimetres of view around the origin.
        let span: CGFloat = footprint ? 3 : 7.62
        return points + [CGPoint(x: -span, y: -span), CGPoint(x: span, y: span)]
    }

    func fit(in size: CGSize) -> Fit? { Fit(bounds(), in: CGSize(width: size.width - 40, height: size.height - 40)).map {
        Fit(scale: $0.scale, origin: $0.origin, offset: CGPoint(x: $0.offset.x + 20, y: $0.offset.y + 20))
    } }

    func draw(_ context: inout GraphicsContext, fit: Fit, selection: LibraryItemEditorView.Selection?) {
        // Origin cross.
        var origin = Path()
        let o = fit.map(.zero)
        origin.move(to: CGPoint(x: o.x - 8, y: o.y)); origin.addLine(to: CGPoint(x: o.x + 8, y: o.y))
        origin.move(to: CGPoint(x: o.x, y: o.y - 8)); origin.addLine(to: CGPoint(x: o.x, y: o.y + 8))
        context.stroke(origin, with: .color(Corv.cyan.opacity(0.5)), lineWidth: 1)
        for shape in form.shapes {
            let active = visible(shape.unit)
            let picked = selection == .shape(shape.i)
            if shape.points.count == 1 || !shape.text.isEmpty, let at = shape.points.first {
                context.draw(Text(shape.text.isEmpty ? shape.kind : shape.text).font(.corvMonoSmall)
                                .foregroundStyle(picked ? Corv.gold : Color.white.opacity(active ? 0.8 : 0.2)), at: fit.map(shown(at)))
                continue
            }
            var path = Path()
            path.addLines(shape.points.map { fit.map(shown($0)) })
            if shape.closed { path.closeSubpath() }
            if shape.fill == 2 && active { context.fill(path, with: .color(Corv.gold.opacity(0.08))) }
            if shape.fill == 1 && active { context.fill(path, with: .color(.white.opacity(0.6))) }
            let color: Color = picked ? Corv.gold : footprint ? layerColor(shape.layer) : .white
            context.stroke(path, with: .color(color.opacity(active ? 0.95 : 0.2)),
                           lineWidth: footprint ? max(1, shape.width * fit.scale) : 1.5)
        }
        for pad in form.pads {
            let center = fit.map(CGPoint(x: pad.x, y: pad.y))
            let width = pad.width * fit.scale, height = pad.height * fit.scale
            let rect = CGRect(x: -width / 2, y: -height / 2, width: width, height: height)
            let radius = pad.shape == "circle" || pad.shape == "oval" ? min(width, height) / 2
                : pad.shape == "roundrect" ? min(width, height) * 0.25 : 0
            var padContext = context
            padContext.translateBy(x: center.x, y: center.y)
            padContext.rotate(by: .degrees(-pad.rotation))
            let picked = selection == .pad(pad.i)
            padContext.fill(Path(roundedRect: rect, cornerRadius: radius),
                            with: .color(picked ? Corv.gold : pad.type == "smd" ? Corv.gold.opacity(0.7) : Corv.goldSoft))
            if pad.drill > 0 {
                let hole = pad.drill * fit.scale
                padContext.fill(Path(ellipseIn: CGRect(x: -hole / 2, y: -hole / 2, width: hole, height: hole)), with: .color(Corv.void))
            }
            context.draw(Text(pad.number).font(.corvMonoSmall).foregroundStyle(Corv.void), at: center)
        }
        for pin in form.pins {
            let active = visible(pin.unit)
            let picked = selection == .pin(pin.i)
            var path = Path()
            let end = fit.map(shown(pin.end)), body = fit.map(shown(pin.body))
            path.move(to: end)
            path.addLine(to: body)
            let color = picked ? Corv.gold : Corv.cyan
            context.stroke(path, with: .color(color.opacity(active ? 0.95 : 0.2)), lineWidth: picked ? 2.5 : 1.5)
            context.stroke(Path(ellipseIn: CGRect(x: end.x - 3, y: end.y - 3, width: 6, height: 6)),
                           with: .color(color.opacity(active ? 0.9 : 0.2)), lineWidth: 1)
            guard active else { continue }
            let mid = CGPoint(x: (end.x + body.x) / 2, y: (end.y + body.y) / 2 - 8)
            context.draw(Text(pin.number).font(.corvMonoSmall).foregroundStyle(Corv.textFaint), at: mid)
            if !pin.name.isEmpty && pin.name != "~" {
                let dx = body.x - end.x, dy = body.y - end.y, length = max(hypot(dx, dy), 1)
                let inward = CGPoint(x: body.x + dx / length * 6, y: body.y + dy / length * 6)
                context.draw(Text(pin.name).font(.corvMonoSmall).foregroundStyle(Corv.text), at: inward,
                             anchor: dx > 0.5 ? .leading : dx < -0.5 ? .trailing : .center)
            }
        }
    }

    private func layerColor(_ layer: String) -> Color {
        layer.hasSuffix("SilkS") ? .white : layer.hasSuffix("CrtYd") ? Corv.cyan : Corv.textFaint
    }
}

/// Edits one symbol or footprint on a canvas: select and drag pins, pads and shapes, add pins or
/// pads, rectangles, lines and circles, delete; fields, units, pin and pad properties alongside;
/// templates (box symbol, pad rows) replace the drawing. Saved into the library in one step.
struct LibraryItemEditorView: View {
    let folder: URL
    let library: String
    let footprint: Bool
    /// The item being edited; nil for a new one.
    let original: String?
    /// The project's footprints ("Library:Name"), offered for a symbol's Footprint field.
    let footprints: [String]
    let perform: (() -> String?) -> String?
    @Environment(\.dismiss) private var dismiss
    @State private var editor: LibraryItemEditor
    @State private var tool = Tool.select
    @State private var unit = 1
    @State private var shared = false
    @State private var selection: Selection?
    /// The line being drawn, in item millimetres.
    @State private var linePoints: [CGPoint] = []
    @State private var drag: (start: CGPoint, now: CGPoint)?
    @State private var showTemplate = false
    @State private var saveError: String?
    @State private var drafts: [String: String] = [:]
    @State private var fill = 2
    @State private var layer = "F.SilkS"
    @State private var padType = "smd"
    @State private var padShape = "roundrect"
    @State private var padWidth = 1.5
    @State private var padHeight = 1.0
    @State private var padDrill = 0.8

    enum Tool: Hashable { case select, pin, rect, line, circle, delete }
    enum Selection: Equatable { case pin(Int), shape(Int), pad(Int) }

    init(folder: URL, library: String, footprint: Bool, original: String?, footprints: [String],
         perform: @escaping (() -> String?) -> String?) {
        self.folder = folder
        self.library = library
        self.footprint = footprint
        self.original = original
        self.footprints = footprints
        self.perform = perform
        _editor = State(initialValue: LibraryItemEditor(footprint: footprint))
    }

    private var toolItems: [ToolRail<Tool>.Item] {
        [.init(tool: .select, title: "Select", icon: "hand.point.up.left"),
         .init(tool: .pin, title: footprint ? "Pad" : "Pin", icon: footprint ? "square.fill" : "line.diagonal.arrow"),
         .init(tool: .rect, title: "Rect", icon: "rectangle"),
         .init(tool: .line, title: "Line", icon: "scribble"),
         .init(tool: .circle, title: "Circle", icon: "circle"),
         .init(tool: .delete, title: "Delete", icon: "trash")]
    }

    private var grid: Double { footprint ? 0.05 : 1.27 }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if let message = saveError ?? editor.error {
                Text(message).font(.caption).foregroundStyle(Corv.red)
            }
            HStack(alignment: .top, spacing: 12) {
                ToolRail(items: toolItems, selection: $tool, disabled: editor.form.derived)
                canvas
                    .background(Color.white.opacity(0.02), in: .rect(cornerRadius: 12))
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Corv.line))
                ScrollView { sidebar.frame(width: 290) }
            }
        }
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .padding(20)
        .background(Corv.void)
        .onAppear(perform: load)
        .onChange(of: editor.text) { syncDrafts() }
        .onChange(of: tool) { linePoints = []; if tool != .select { selection = nil } }
        .fullScreenCover(isPresented: $showTemplate) {
            TemplateForm(footprint: footprint, footprints: footprints) { editor.load($0, keepHistory: true) }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            MonoLabel("\(original == nil ? "New" : "Edit") \(footprint ? "footprint" : "symbol") · \(library)", color: Corv.gold)
            Spacer()
            Button { editor.undo() } label: { Image(systemName: "arrow.uturn.backward") }
                .buttonStyle(.corvGhost).disabled(!editor.canUndo).accessibilityLabel("Undo")
            Button { editor.redo() } label: { Image(systemName: "arrow.uturn.forward") }
                .buttonStyle(.corvGhost).disabled(!editor.canRedo).accessibilityLabel("Redo")
            Button("Cancel") { dismiss() }.buttonStyle(.corvGhost)
            Button("Save") { save() }.buttonStyle(.corvPrimary)
                .accessibilityIdentifier("saveItem")
        }
    }

    // MARK: Canvas

    private var canvas: some View {
        GeometryReader { geometry in
            let drawing = EditorDrawing(form: editor.form, footprint: footprint, unit: unit)
            Canvas { context, size in
                guard let fit = drawing.fit(in: size) else { return }
                drawGrid(&context, fit: fit, size: size)
                drawing.draw(&context, fit: fit, selection: selection)
                drawPreview(&context, fit: fit)
            }
            .contentShape(.rect)
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { value in drag = (value.startLocation, value.location) }
                .onEnded { value in
                    drag = nil
                    guard let fit = drawing.fit(in: geometry.size) else { return }
                    finish(from: value.startLocation, to: value.location, fit: fit, drawing: drawing)
                })
            .accessibilityElement()
            .accessibilityLabel("\(footprint ? "Footprint" : "Symbol") canvas")
            .accessibilityIdentifier("itemCanvas")
            .accessibilityAddTraits(.allowsDirectInteraction)
        }
        .frame(minWidth: 300, minHeight: 300)
    }

    private func drawGrid(_ context: inout GraphicsContext, fit: Fit, size: CGSize) {
        let step = grid * (footprint ? 10 : 2) // 0.5 mm or 2.54 mm lines
        guard step * fit.scale > 6 else { return }
        let first = fit.unmap(.zero), last = fit.unmap(CGPoint(x: size.width, y: size.height))
        var lines = Path()
        for x in stride(from: (first.x / step).rounded(.down) * step, through: last.x, by: step) {
            lines.move(to: fit.map(CGPoint(x: x, y: first.y))); lines.addLine(to: fit.map(CGPoint(x: x, y: last.y)))
        }
        for y in stride(from: (first.y / step).rounded(.down) * step, through: last.y, by: step) {
            lines.move(to: fit.map(CGPoint(x: first.x, y: y))); lines.addLine(to: fit.map(CGPoint(x: last.x, y: y)))
        }
        context.stroke(lines, with: .color(.white.opacity(0.05)), lineWidth: 0.6)
    }

    private func drawPreview(_ context: inout GraphicsContext, fit: Fit) {
        if !linePoints.isEmpty {
            var path = Path()
            path.addLines(linePoints.map { fit.map(EditorDrawing(form: editor.form, footprint: footprint, unit: unit).shown($0)) })
            if let drag { path.addLine(to: fit.map(snap(fit.unmap(drag.now)))) }
            context.stroke(path, with: .color(Corv.gold), style: StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
        }
        guard let drag, hypot(drag.now.x - drag.start.x, drag.now.y - drag.start.y) > 6 else { return }
        let a = fit.map(snap(fit.unmap(drag.start))), b = fit.map(snap(fit.unmap(drag.now)))
        var path = Path()
        switch tool {
        case .rect:
            path.addRect(CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y)))
        case .circle:
            let r = hypot(b.x - a.x, b.y - a.y)
            path.addEllipse(in: CGRect(x: a.x - r, y: a.y - r, width: r * 2, height: r * 2))
        case .select where selection != nil:
            path.move(to: a); path.addLine(to: b)
        default:
            return
        }
        context.stroke(path, with: .color(Corv.gold), style: StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
    }

    /// A drawn point on the editor grid.
    private func snap(_ point: CGPoint) -> CGPoint {
        CGPoint(x: (point.x / grid).rounded() * grid, y: (point.y / grid).rounded() * grid)
    }

    private func number(_ value: Double) -> String { String(format: "%.4f", value) }

    /// The pin, pad or shape under a view point (pins and pads first).
    private func hit(_ location: CGPoint, fit: Fit, drawing: EditorDrawing) -> Selection? {
        func distance(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
            let dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy
            let t = length > 0 ? min(1, max(0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / length)) : 0
            return hypot(a.x + t * dx - p.x, a.y + t * dy - p.y)
        }
        for pin in editor.form.pins where drawing.visible(pin.unit) {
            if distance(location, fit.map(drawing.shown(pin.end)), fit.map(drawing.shown(pin.body))) <= 12 { return .pin(pin.i) }
        }
        for pad in editor.form.pads {
            let c = fit.map(CGPoint(x: pad.x, y: pad.y))
            if abs(location.x - c.x) <= pad.width * fit.scale / 2 + 6 && abs(location.y - c.y) <= pad.height * fit.scale / 2 + 6 {
                return .pad(pad.i)
            }
        }
        for shape in editor.form.shapes where drawing.visible(shape.unit) {
            let points = shape.points.map { fit.map(drawing.shown($0)) }
            if points.count == 1, hypot(points[0].x - location.x, points[0].y - location.y) <= 16 { return .shape(shape.i) }
            let segments = shape.closed ? points + points.prefix(1) : points
            if zip(segments, segments.dropFirst()).contains(where: { pair in distance(location, pair.0, pair.1) <= 10 }) {
                return .shape(shape.i)
            }
        }
        return nil
    }

    private func finish(from start: CGPoint, to end: CGPoint, fit: Fit, drawing: EditorDrawing) {
        let moved = hypot(end.x - start.x, end.y - start.y) > 6
        // Item millimetres (symbols: y up again).
        let a = drawing.stored(snap(fit.unmap(start))), b = drawing.stored(snap(fit.unmap(end)))
        let addUnit = shared ? 0 : unit
        switch tool {
        case .select:
            if !moved { selection = hit(end, fit: fit, drawing: drawing); return }
            guard let picked = hit(start, fit: fit, drawing: drawing) else { return }
            selection = picked
            let dx = b.x - a.x, dy = b.y - a.y
            guard dx != 0 || dy != 0 else { return }
            switch picked {
            case .pin(let i):
                if let pin = editor.form.pins.first(where: { $0.i == i }) {
                    editor.edit(["pin", "move", "\(i)", number(pin.x + dx), number(pin.y + dy), number(pin.angle)])
                }
            case .pad(let i):
                if let pad = editor.form.pads.first(where: { $0.i == i }) {
                    editor.edit(["pad", "move", "\(i)", number(pad.x + dx), number(pad.y + dy)])
                }
            case .shape(let i):
                editor.edit(["shape", "move", "\(i)", number(dx), number(dy)])
            }
        case .pin:
            if footprint {
                editor.edit(["pad", "add", nextNumber(editor.form.pads.map(\.number)), padType, padShape,
                             number(b.x), number(b.y), number(padWidth), number(padHeight), number(padDrill)])
            } else {
                // Pointing into the body: from the left of the drawing's centre, towards +x.
                let xs = editor.form.shapes.flatMap { $0.points.map(\.x) }
                let centre = xs.isEmpty ? 0 : (xs.min()! + xs.max()!) / 2
                editor.edit(["pin", "add", "\(addUnit)", nextNumber(editor.form.pins.map(\.number)), "", "passive",
                             number(b.x), number(b.y), b.x < centre ? "0" : "180", "2.54"])
            }
            selection = nil
        case .rect:
            guard moved else { return }
            if footprint {
                editor.edit(["shape", "add", "rect", layer, number(a.x), number(a.y), number(b.x), number(b.y), lineWidth])
            } else {
                editor.edit(["shape", "add", "\(addUnit)", "rect", "\(fill)", number(a.x), number(a.y), number(b.x), number(b.y)])
            }
        case .circle:
            guard moved else { return }
            let r = hypot(b.x - a.x, b.y - a.y)
            if footprint {
                editor.edit(["shape", "add", "circle", layer, number(a.x), number(a.y), number(r), lineWidth])
            } else {
                editor.edit(["shape", "add", "\(addUnit)", "circle", "\(fill)", number(a.x), number(a.y), number(r)])
            }
        case .line:
            // A tap on the last point again ends the line.
            if let last = linePoints.last, last == b { finishLine(); return }
            linePoints.append(b)
        case .delete:
            switch hit(end, fit: fit, drawing: drawing) {
            case .pin(let i)?: editor.edit(["pin", "delete", "\(i)"])
            case .pad(let i)?: editor.edit(["pad", "delete", "\(i)"])
            case .shape(let i)?: editor.edit(["shape", "delete", "\(i)"])
            case nil: break
            }
            selection = nil
        }
    }

    private var lineWidth: String { layer.hasSuffix("CrtYd") ? "0.05" : layer.hasSuffix("Fab") ? "0.1" : "0.12" }

    private func finishLine() {
        defer { linePoints = [] }
        guard linePoints.count > 1 else { return }
        if footprint {
            for (a, b) in zip(linePoints, linePoints.dropFirst()) {
                editor.edit(["shape", "add", "line", layer, number(a.x), number(a.y), number(b.x), number(b.y), lineWidth])
            }
        } else {
            editor.edit(["shape", "add", "\(shared ? 0 : unit)", "poly", "0"] + linePoints.flatMap { [number($0.x), number($0.y)] })
        }
    }

    private func nextNumber(_ used: [String]) -> String {
        let taken = Set(used)
        return "\((1...9999).first { !taken.contains("\($0)") } ?? used.count + 1)"
    }

    // MARK: Sidebar

    @ViewBuilder private var sidebar: some View {
        VStack(alignment: .leading, spacing: 16) {
            InspectorSection(footprint ? "Footprint" : "Symbol") {
                draftField("Name", key: "name") { editor.edit(["name", $0]) }
                if footprint {
                    draftField("Description", key: "descr") { editor.edit(["field", "descr", $0]) }
                    Picker("Type", selection: Binding(get: { editor.form.attr == "through_hole" ? "through_hole" : "smd" },
                                                      set: { editor.edit(["attr", $0]) })) {
                        Text("SMD").tag("smd")
                        Text("Through-hole").tag("through_hole")
                    }
                    .pickerStyle(.segmented)
                } else {
                    ForEach(["Reference", "Value", "Footprint", "Description"], id: \.self) { key in
                        HStack(alignment: .bottom, spacing: 6) {
                            draftField(key, key: key) { editor.edit(["field", key, $0]) }
                            if key == "Footprint" && !footprints.isEmpty {
                                Menu {
                                    Button("None") { editor.edit(["field", "Footprint", ""]) }
                                    ForEach(footprints, id: \.self) { id in Button(id) { editor.edit(["field", "Footprint", id]) } }
                                } label: { Image(systemName: "chevron.down.circle").foregroundStyle(Corv.gold) }
                                    .accessibilityLabel("Project footprints")
                            }
                        }
                    }
                }
            }
            if editor.form.derived {
                CautionNote(text: "Derived from \(editor.form.base): its pins and drawing come from the base symbol. Edit the base for those.")
            } else if !footprint {
                unitsSection
            }
            selectionSection
            if !editor.form.derived {
                if footprint { newItemSettings }
                Button { showTemplate = true } label: {
                    Label(footprint ? "Pad rows…" : "Box layout…", systemImage: "square.grid.3x3")
                }
                .buttonStyle(.corvGhost)
                .accessibilityIdentifier("template")
                if tool == .line && linePoints.count > 1 {
                    Button("Finish line") { finishLine() }.buttonStyle(.corvGhost)
                }
            }
        }
    }

    private var unitsSection: some View {
        InspectorSection("Units") {
            Stepper("Units \(editor.form.units)", value: Binding(get: { editor.form.units },
                                                                 set: { editor.edit(["units", "\($0)"]); unit = min(unit, $0) }),
                    in: 1...26)
                .font(.corvReadout).foregroundStyle(Corv.text)
            if editor.form.units > 1 {
                Picker("Unit", selection: $unit) {
                    ForEach(1...editor.form.units, id: \.self) { Text(unitName($0)).tag($0) }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                Toggle("New items shared by all units", isOn: $shared).font(.caption).tint(Corv.gold)
            }
            Picker("Fill", selection: $fill) {
                Text("No fill").tag(0)
                Text("Outline").tag(1)
                Text("Body").tag(2)
            }
            .pickerStyle(.segmented)
        }
    }

    private func unitName(_ unit: Int) -> String {
        "Unit " + String(UnicodeScalar(UInt8(64 + min(unit, 26))))
    }

    private var newItemSettings: some View {
        InspectorSection("New pads and lines") {
            Picker("Pad", selection: $padType) {
                Text("SMD").tag("smd")
                Text("THT").tag("thru_hole")
                Text("Hole").tag("np_thru_hole")
            }
            .pickerStyle(.segmented)
            Picker("Shape", selection: $padShape) {
                ForEach(["roundrect", "rect", "circle", "oval"], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.segmented)
            HStack(spacing: 8) {
                NumberField(title: "W mm", value: $padWidth)
                NumberField(title: "H mm", value: $padHeight)
                if padType != "smd" { NumberField(title: "Drill", value: $padDrill) }
            }
            Picker("Layer", selection: $layer) {
                ForEach(["F.SilkS", "F.Fab", "F.CrtYd", "B.SilkS", "B.Fab", "B.CrtYd"], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.menu).tint(Corv.gold)
        }
    }

    @ViewBuilder private var selectionSection: some View {
        switch selection {
        case .pin(let i)?:
            if let pin = editor.form.pins.first(where: { $0.i == i }) { PinInspector(pin: pin, editor: editor) }
        case .pad(let i)?:
            if let pad = editor.form.pads.first(where: { $0.i == i }) { PadInspector(pad: pad, editor: editor) }
        case .shape(let i)?:
            if let shape = editor.form.shapes.first(where: { $0.i == i }) {
                InspectorSection("Shape · \(shape.kind)") {
                    if !footprint {
                        Picker("Fill", selection: Binding(get: { shape.fill }, set: { editor.edit(["shape", "fill", "\(i)", "\($0)"]) })) {
                            Text("No fill").tag(0)
                            Text("Outline").tag(1)
                            Text("Body").tag(2)
                        }
                        .pickerStyle(.segmented)
                    }
                    Button("Delete shape", role: .destructive) { editor.edit(["shape", "delete", "\(i)"]); selection = nil }
                        .buttonStyle(.corvGhost)
                }
            }
        case nil:
            EmptyView()
        }
    }

    private func draftField(_ title: String, key: String, commit: @escaping (String) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            MonoLabel(title, color: Corv.textFaint, small: true)
            TextField(title, text: Binding(get: { drafts[key] ?? "" }, set: { drafts[key] = $0 }))
                .corvField()
                .onSubmit { commit(drafts[key] ?? "") }
                .accessibilityIdentifier("field-\(key)")
        }
    }

    private func syncDrafts() {
        let form = editor.form
        drafts = ["name": form.name, "descr": form.description]
        for key in ["Reference", "Value", "Footprint", "Description"] { drafts[key] = form.fields[key] ?? "" }
        if selection.map({ item -> Bool in
            switch item {
            case .pin(let i): return !form.pins.contains { $0.i == i }
            case .pad(let i): return !form.pads.contains { $0.i == i }
            case .shape(let i): return !form.shapes.contains { $0.i == i }
            }
        }) == true { selection = nil }
    }

    private func load() {
        var failure: String?
        if let original {
            guard let text = ProjectLibraries.text(&failure, {
                cc_library_item_text(folder.path, footprint ? 1 : 0, library, original, $0, $1)
            }) else { saveError = failure; return }
            editor.load(text)
            if editor.form.derived, !editor.form.base.isEmpty,
               let base = ProjectLibraries.text(&failure, { cc_library_item_text(folder.path, 0, library, editor.form.base, $0, $1) }) {
                editor.load(text, base: base)
            }
        } else {
            let spec = footprint ? "name\tNewFootprint\ntype\tsmd\npads\t2\npitch\t2.54\npad\t1.5\t1\n"
                                 : "name\tNewSymbol\nreference\tU\npin\t1\t\tpassive\tleft\npin\t2\t\tpassive\tright\n"
            guard let text = ProjectLibraries.text(&failure, { cc_library_generate(footprint ? 1 : 0, spec, $0, $1) }) else {
                saveError = failure; return
            }
            editor.load(text)
        }
        syncDrafts()
    }

    private func save() {
        let text = editor.text
        saveError = perform {
            ProjectLibraries.run { cc_save_library_item_text(folder.path, footprint ? 1 : 0, library, original ?? "", text, 0, $0, $1) }
        }
        if saveError == nil { dismiss() }
    }
}

/// A selected pin: number, name, type, side and length.
private struct PinInspector: View {
    let pin: EditorForm.Pin
    let editor: LibraryItemEditor
    @State private var number = ""
    @State private var name = ""
    @State private var length = 2.54
    private static let types = ["input", "output", "bidirectional", "tri_state", "passive", "power_in", "power_out",
                                "open_collector", "open_emitter", "unspecified", "free", "no_connect"]

    var body: some View {
        InspectorSection("Pin \(pin.number)") {
            HStack(spacing: 8) {
                TextField("No.", text: $number).frame(width: 70).corvField().accessibilityIdentifier("pinNumber")
                TextField("Name", text: $name).corvField().accessibilityIdentifier("pinName")
            }
            Picker("Type", selection: Binding(get: { pin.type }, set: { set(type: $0) })) {
                ForEach(Self.types, id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.menu).tint(Corv.gold)
            // KiCad's angle runs from the connection end into the body: 0 is a pin on the left.
            Picker("Side", selection: Binding(get: { Int(pin.angle) }, set: { move(angle: $0) })) {
                Text("Left").tag(0)
                Text("Right").tag(180)
                Text("Top").tag(270)
                Text("Bottom").tag(90)
            }
            .pickerStyle(.segmented)
            HStack(spacing: 8) {
                NumberField(title: "Length mm", value: $length)
                Button("Apply") { set(type: pin.type) }.buttonStyle(.corvGhost)
            }
            Button("Delete pin", role: .destructive) { editor.edit(["pin", "delete", "\(pin.i)"]) }.buttonStyle(.corvGhost)
        }
        .onAppear { number = pin.number; name = pin.name == "~" ? "" : pin.name; length = pin.length }
        .onChange(of: pin.i) { number = pin.number; name = pin.name == "~" ? "" : pin.name; length = pin.length }
    }

    private func set(type: String) {
        editor.edit(["pin", "set", "\(pin.i)", number, name, type, String(format: "%.4f", length)])
    }

    private func move(angle: Int) {
        editor.edit(["pin", "move", "\(pin.i)", String(format: "%.4f", pin.x), String(format: "%.4f", pin.y), "\(angle)"])
    }
}

/// A selected pad: number, type, shape, size and drill.
private struct PadInspector: View {
    let pad: EditorForm.Pad
    let editor: LibraryItemEditor
    @State private var number = ""
    @State private var type = "smd"
    @State private var shape = "rect"
    @State private var width = 1.0
    @State private var height = 1.0
    @State private var drill = 0.8

    var body: some View {
        InspectorSection("Pad \(pad.number)") {
            TextField("Number", text: $number).corvField().accessibilityIdentifier("padNumber")
            Picker("Type", selection: $type) {
                Text("SMD").tag("smd")
                Text("THT").tag("thru_hole")
                Text("Hole").tag("np_thru_hole")
            }
            .pickerStyle(.segmented)
            Picker("Shape", selection: $shape) {
                ForEach(["roundrect", "rect", "circle", "oval"], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.segmented)
            HStack(spacing: 8) {
                NumberField(title: "W mm", value: $width)
                NumberField(title: "H mm", value: $height)
                if type != "smd" { NumberField(title: "Drill", value: $drill) }
            }
            HStack(spacing: 8) {
                Button("Apply") {
                    editor.edit(["pad", "set", "\(pad.i)", number, type, shape, String(format: "%.4f", width),
                                 String(format: "%.4f", height), String(format: "%.4f", drill)])
                }
                .buttonStyle(.corvGhost)
                Button("Delete pad", role: .destructive) { editor.edit(["pad", "delete", "\(pad.i)"]) }.buttonStyle(.corvGhost)
            }
        }
        .onAppear(perform: load)
        .onChange(of: pad.i) { load() }
    }

    private func load() {
        number = pad.number
        type = pad.type
        shape = pad.shape
        width = pad.width
        height = pad.height
        drill = pad.drill > 0 ? pad.drill : 0.8
    }
}

/// Templates that replace the drawing: a box symbol from a pin list, or rows of pads.
private struct TemplateForm: View {
    let footprint: Bool
    let footprints: [String]
    let onGenerate: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var reference = "U"
    @State private var footprintID = ""
    @State private var pins: [Pin] = [Pin(number: "1"), Pin(number: "2", right: true)]
    @State private var smd = true
    @State private var pads = 8
    @State private var rows = 2
    @State private var pitch = 1.27
    @State private var span = 5.4
    @State private var padWidth = 1.55
    @State private var padHeight = 0.6
    @State private var drill = 1.0

    struct Pin: Identifiable {
        let id = UUID()
        var number: String
        var name = ""
        var type = "passive"
        var right = false
    }

    private var spec: String {
        if footprint {
            return ["name\t\(name)", "type\t\(smd ? "smd" : "tht")", "pads\t\(pads)", "rows\t\(rows)", "pitch\t\(pitch)",
                    "span\t\(span)", "pad\t\(padWidth)\t\(padHeight)", "drill\t\(drill)"].joined(separator: "\n")
        }
        return (["name\t\(name)", "reference\t\(reference)", "footprint\t\(footprintID)"] +
                pins.map { "pin\t\($0.number)\t\($0.name)\t\($0.type)\t\($0.right ? "right" : "left")" }).joined(separator: "\n")
    }

    var body: some View {
        var failure: String?
        let text = ProjectLibraries.text(&failure) { cc_library_generate(footprint ? 1 : 0, spec, $0, $1) }
        let form = text.flatMap { generated in
            ProjectLibraries.text(&failure) { cc_library_item_json(footprint ? 1 : 0, generated, nil, $0, $1) }
        }.map { EditorForm(json: $0) }
        return VStack(alignment: .leading, spacing: 12) {
            HStack {
                MonoLabel(footprint ? "Pad rows" : "Box layout", color: Corv.gold)
                Spacer()
                Button("Cancel") { dismiss() }.buttonStyle(.corvGhost)
                Button("Use") { if let text { onGenerate(text); dismiss() } }
                    .buttonStyle(.corvPrimary).disabled(text == nil)
                    .accessibilityIdentifier("useTemplate")
            }
            CautionNote(text: "Replaces the drawing (pins or pads and shapes). Undo in the editor brings it back.")
            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 4) {
                    MonoLabel("Name", color: Corv.textFaint, small: true)
                    TextField("Name", text: $name).corvField()
                }
                if !footprint {
                    VStack(alignment: .leading, spacing: 4) {
                        MonoLabel("Ref", color: Corv.textFaint, small: true)
                        TextField("Ref", text: $reference).frame(width: 70).corvField()
                    }
                    Menu {
                        Button("None") { footprintID = "" }
                        ForEach(footprints, id: \.self) { id in Button(id) { footprintID = id } }
                    } label: { Text(footprintID.isEmpty ? "No footprint" : footprintID).font(.corvReadout).foregroundStyle(Corv.gold) }
                }
            }
            if let form {
                Canvas { context, size in
                    let drawing = EditorDrawing(form: form, footprint: footprint, unit: 1)
                    if let fit = drawing.fit(in: size) { drawing.draw(&context, fit: fit, selection: nil) }
                }
                .frame(height: 200)
                .background(Color.white.opacity(0.02), in: .rect(cornerRadius: 12))
            } else if let failure {
                Text(failure).font(.caption).foregroundStyle(Corv.red).frame(height: 200)
            }
            if footprint { rowFields } else { pinList }
        }
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .padding(20)
        .background(Corv.void)
    }

    private var rowFields: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Picker("Type", selection: $smd) {
                    Text("SMD").tag(true)
                    Text("Through-hole").tag(false)
                }
                .pickerStyle(.segmented)
                Picker("Rows", selection: $rows) {
                    Text("1 row").tag(1)
                    Text("2 rows").tag(2)
                }
                .pickerStyle(.segmented)
                Stepper("Pads \(pads)", value: $pads, in: 1...256, step: rows).font(.corvReadout).foregroundStyle(Corv.text)
            }
            HStack(spacing: 10) {
                NumberField(title: "Pitch mm", value: $pitch)
                if rows == 2 { NumberField(title: "Row spacing mm", value: $span) }
                NumberField(title: "Pad W mm", value: $padWidth)
                NumberField(title: "Pad H mm", value: $padHeight)
                if !smd { NumberField(title: "Drill mm", value: $drill) }
            }
        }
        .onChange(of: rows) { _, value in if value == 2 && pads % 2 == 1 { pads += 1 } }
    }

    private var pinList: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                MonoLabel("Pins · \(pins.count)", color: Corv.textFaint, small: true)
                Spacer()
                Button {
                    let taken = Set(pins.map(\.number))
                    let next = (1...999).first { !taken.contains("\($0)") } ?? pins.count + 1
                    pins.append(Pin(number: "\(next)", right: pins.filter(\.right).count < pins.filter { !$0.right }.count))
                } label: { Label("Pin", systemImage: "plus") }
                    .buttonStyle(.corvGhost)
            }
            ScrollView {
                VStack(spacing: 6) {
                    ForEach($pins) { $pin in
                        HStack(spacing: 8) {
                            TextField("No.", text: $pin.number).frame(width: 60).corvField()
                            TextField("Name", text: $pin.name).corvField()
                            Picker("Type", selection: $pin.type) {
                                ForEach(["input", "output", "bidirectional", "tri_state", "passive", "power_in", "power_out",
                                         "open_collector", "open_emitter", "unspecified", "free", "no_connect"], id: \.self) {
                                    Text($0).tag($0)
                                }
                            }
                            .pickerStyle(.menu).tint(Corv.gold)
                            Picker("Side", selection: $pin.right) {
                                Text("Left").tag(false)
                                Text("Right").tag(true)
                            }
                            .pickerStyle(.segmented).frame(width: 120)
                            Button { pins.removeAll { $0.id == pin.id } } label: { Image(systemName: "minus.circle") }
                                .buttonStyle(.plain).foregroundStyle(Corv.red)
                                .accessibilityLabel("Remove pin \(pin.number)")
                        }
                    }
                }
            }
        }
    }
}


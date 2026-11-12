import SwiftUI
import UniformTypeIdentifiers

/// One Gerber or Excellon file as read by the engine (corvcad/gerber.hpp): its shapes as paths in
/// millimetres with the files' y axis (up), in file order.
struct GerberLayerModel: Identifiable {
    struct Shape {
        let path: Path
        /// Stroke width in millimetres for a draw; nil for a filled flash or region.
        let stroke: Double?
        /// Clear polarity erases what the layer drew before it.
        let dark: Bool
    }
    let id = UUID()
    let name: String
    let function: String
    let drill: Bool
    let bounds: CGRect
    let flashes: Int, draws: Int, regions: Int, holes: Int, slots: Int
    let warnings: [String]
    let shapes: [Shape]
    var visible = true

    /// Fab, courtyard, user and paste layers start hidden: they cover the fabrication layers.
    var shownByDefault: Bool {
        let k = key
        return !["assembly", "fab", "courtyard", "crtyd", "user", "other", "paste", "margin"].contains { k.contains($0) }
    }

    /// Side and kind from the X2 file function, else the file name (KiCad's layer suffixes).
    private var key: String { (function.isEmpty ? name : function).lowercased() }
    private var bottom: Bool { key.contains("bot") || key.contains("b_") || key.contains("-b.") || key.hasSuffix(".gbl") || key.hasSuffix(".gbs") || key.hasSuffix(".gbo") || key.hasSuffix(".gbp") }

    var title: String {
        let parts = function.split(separator: ",").map(String.init)
        guard let kind = parts.first else { return name }
        let side = bottom ? "Bottom" : "Top"
        switch kind {
        case "Copper": return parts.count > 2 && parts[2] == "Inr" ? "Inner copper \(parts[1])" : "\(side) copper"
        case "Soldermask": return "\(side) solder mask"
        case "Legend": return "\(side) silkscreen"
        case "Paste": return "\(side) paste"
        case "Profile": return "Board outline"
        case "AssemblyDrawing": return "\(side) fab"
        case "Plated": return "Plated holes"
        case "NonPlated": return "Non-plated holes"
        default: return name
        }
    }

    /// Bottom layers first, then top, outline and holes on top of everything.
    var order: Int {
        if drill { return 20 }
        let k = key
        if k.contains("profile") || k.contains("edge") { return 19 }
        if k.contains("inr") { return 5 }
        // Top: copper, mask, paste, silkscreen, others upwards; the bottom side the other way round.
        let kind = k.contains("copper") || k.contains("_cu") ? 0 : k.contains("mask") ? 1 : k.contains("paste") ? 2
            : k.contains("legend") || k.contains("silk") ? 3 : 4
        return bottom ? 3 - min(kind, 3) : 6 + kind
    }

    var color: Color {
        if drill { return Corv.void }
        let kind = key
        if kind.contains("profile") || kind.contains("edge") { return .white }
        if kind.contains("inr") { return Corv.goldSoft }
        if kind.contains("copper") || kind.contains("_cu") { return bottom ? Corv.cyan : Corv.gold }
        if kind.contains("legend") || kind.contains("silk") { return .white }
        if kind.contains("mask") { return Corv.red }
        return Corv.textDim
    }

    var opacity: Double {
        let kind = key
        return kind.contains("mask") ? 0.35 : kind.contains("paste") ? 0.5 : drill ? 1 : 0.85
    }
}

enum GerberFiles {
    /// Files in a fabrication folder that are not Gerber or drill data.
    static let ignored: Set<String> = ["gbrjob", "zip", "pdf", "svg", "csv", "pos", "json", "png", "jpg", "md", "rpt", "html"]

    /// Reads files, or every file of a folder, through the engine. Unreadable files are reported.
    static func load(_ urls: [URL]) -> (layers: [GerberLayerModel], errors: [String]) {
        var files: [URL] = []
        for url in urls {
            if (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                let contents = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? []
                files += contents.sorted { $0.lastPathComponent < $1.lastPathComponent }
            } else {
                files.append(url)
            }
        }
        var layers: [GerberLayerModel] = []
        var errors: [String] = []
        for file in files where !ignored.contains(file.pathExtension.lowercased()) {
            guard let data = try? Data(contentsOf: file),
                  let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else {
                errors.append("\(file.lastPathComponent): unreadable")
                continue
            }
            var failure: String?
            guard let json = ProjectLibraries.text(&failure, { cc_gerber_json(text, file.lastPathComponent, $0, $1) }),
                  let layer = layer(json) else {
                errors.append("\(file.lastPathComponent): \(failure ?? "not a Gerber or drill file")")
                continue
            }
            if layer.shapes.isEmpty { errors.append("\(file.lastPathComponent): nothing to draw"); continue }
            var shown = layer
            shown.visible = layer.shownByDefault
            layers.append(shown)
        }
        return (layers.sorted { $0.order < $1.order }, errors)
    }

    private static func layer(_ json: String) -> GerberLayerModel? {
        guard let row = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any] else { return nil }
        func points(_ any: Any?) -> [CGPoint] {
            let values = (any as? [NSNumber] ?? []).map { CGFloat($0.doubleValue) }
            return stride(from: 0, to: values.count - 1, by: 2).map { CGPoint(x: values[$0], y: values[$0 + 1]) }
        }
        var shapes: [GerberLayerModel.Shape] = []
        for item in row["items"] as? [[String: Any]] ?? [] {
            let dark = item["dark"] as? Bool ?? true
            let line = points(item["path"])
            if line.count > 1 {
                var path = Path()
                path.addLines(line)
                shapes.append(.init(path: path, stroke: (item["width"] as? NSNumber)?.doubleValue ?? 0, dark: dark))
                continue
            }
            var path = Path()
            for polygon in item["polygons"] as? [[String: Any]] ?? [] {
                let corners = points(polygon["points"])
                guard corners.count > 2 else { continue }
                path.addLines(corners)
                path.closeSubpath()
            }
            if !path.isEmpty { shapes.append(.init(path: path, stroke: nil, dark: dark)) }
        }
        let b = (row["bounds"] as? [NSNumber] ?? []).map { CGFloat($0.doubleValue) }
        let counts = row["counts"] as? [String: Int] ?? [:]
        return GerberLayerModel(name: row["name"] as? String ?? "", function: row["function"] as? String ?? "",
                                drill: row["drill"] as? Bool ?? false,
                                bounds: b.count == 4 ? CGRect(x: b[0], y: b[1], width: b[2] - b[0], height: b[3] - b[1]) : .zero,
                                flashes: counts["flashes"] ?? 0, draws: counts["draws"] ?? 0, regions: counts["regions"] ?? 0,
                                holes: counts["holes"] ?? 0, slots: counts["slots"] ?? 0,
                                warnings: row["warnings"] as? [String] ?? [], shapes: shapes)
    }
}

/// Presents the viewer on these files or folders.
struct GerberRequest: Identifiable {
    let id = UUID()
    let files: [URL]
}

/// Views Gerber and Excellon files: the board's fabrication export, or files and folders from
/// Files. Layers stack bottom to top with their own colours; pinch and drag to zoom and pan.
struct GerberViewer: View {
    /// Files or folders to open at once (the fabrication export's folder).
    var initial: [URL] = []
    @Environment(\.dismiss) private var dismiss
    @State private var layers: [GerberLayerModel] = []
    @State private var errors: [String] = []
    @State private var importing = false
    @State private var zoom: CGFloat = 1
    @State private var pan = CGSize.zero
    @State private var pinch: CGFloat = 1
    @State private var drag = CGSize.zero

    private var bounds: CGRect? {
        layers.filter(\.visible).map(\.bounds).filter { $0.width > 0 || $0.height > 0 }
            .reduce(CGRect?.none) { $0?.union($1) ?? $1 }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                MonoLabel("Gerber viewer", color: Corv.gold)
                if let bounds {
                    MonoLabel(String(format: "%.2f × %.2f mm", bounds.width, bounds.height), color: Corv.cyan, small: true)
                }
                Spacer()
                Button { importing = true } label: { Label("Open", systemImage: "folder") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("openGerberFiles")
                Button { withAnimation(Corv.spring) { zoom = 1; pan = .zero } } label: { Image(systemName: "arrow.up.left.and.arrow.down.right") }
                    .buttonStyle(.corvGhost)
                    .accessibilityLabel("Fit")
                Button("Close") { dismiss() }.buttonStyle(.corvGhost)
            }
            HStack(alignment: .top, spacing: 12) {
                layerList.frame(width: 250)
                canvas
            }
        }
        .padding(20)
        .background(Corv.void)
        .onAppear { if !initial.isEmpty { open(initial) } }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item, .folder], allowsMultipleSelection: true) { result in
            guard case .success(let urls) = result else { return }
            let access = urls.map { $0.startAccessingSecurityScopedResource() }
            defer { for (url, granted) in zip(urls, access) where granted { url.stopAccessingSecurityScopedResource() } }
            open(urls)
        }
    }

    private func open(_ urls: [URL]) {
        let loaded = GerberFiles.load(urls)
        layers = loaded.layers
        errors = loaded.errors
        zoom = 1
        pan = .zero
    }

    private var layerList: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                if layers.isEmpty {
                    Text("Open Gerber (.gbr, .gtl, …) and drill (.drl) files, or the folder that holds them.")
                        .font(.caption).foregroundStyle(Corv.textFaint)
                }
                ForEach($layers) { $layer in
                    Button { layer.visible.toggle() } label: {
                        HStack(alignment: .top, spacing: 8) {
                            Circle().fill(layer.drill ? Color.white : layer.color).frame(width: 9, height: 9)
                                .opacity(layer.visible ? 1 : 0.25).padding(.top, 4)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(layer.title).font(.corvReadout).foregroundStyle(layer.visible ? Corv.text : Corv.textFaint)
                                    .lineLimit(1)
                                MonoLabel(layer.drill ? "\(layer.holes) holes · \(layer.slots) slots"
                                                      : "\(layer.flashes) flashes · \(layer.draws) draws · \(layer.regions) regions",
                                          color: Corv.cyan, small: true)
                                if let warning = layer.warnings.first {
                                    Text(warning).font(.caption2).foregroundStyle(Corv.gold)
                                }
                            }
                            Spacer(minLength: 0)
                        }
                        .padding(8)
                        .background(Color.white.opacity(layer.visible ? 0.04 : 0.01), in: .rect(cornerRadius: 10))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(layer.title), \(layer.visible ? "shown" : "hidden")")
                }
                ForEach(errors, id: \.self) { Text($0).font(.caption2).foregroundStyle(Corv.textFaint) }
            }
        }
    }

    private var canvas: some View {
        Canvas { context, size in
            guard let bounds else { return }
            let fit = min((size.width - 40) / max(bounds.width, 0.1), (size.height - 40) / max(bounds.height, 0.1))
            let scale = fit * zoom * pinch
            context.translateBy(x: size.width / 2 + pan.width + drag.width, y: size.height / 2 + pan.height + drag.height)
            // Gerber y runs up; the view's y runs down.
            context.scaleBy(x: scale, y: -scale)
            context.translateBy(x: -bounds.midX, y: -bounds.midY)
            for layer in layers where layer.visible {
                var layerContext = context
                layerContext.opacity = layer.opacity
                layerContext.drawLayer { drawing in
                    for shape in layer.shapes {
                        var shapeContext = drawing
                        if !shape.dark { shapeContext.blendMode = .destinationOut }
                        if let width = shape.stroke {
                            shapeContext.stroke(shape.path, with: .color(layer.color),
                                                style: StrokeStyle(lineWidth: max(width, 1 / scale), lineCap: .round, lineJoin: .round))
                        } else {
                            shapeContext.fill(shape.path, with: .color(layer.color))
                        }
                    }
                }
                if layer.drill {
                    // Holes: void over the copper with a thin rim, so they read on the background too.
                    for shape in layer.shapes {
                        if let width = shape.stroke {
                            context.stroke(shape.path, with: .color(.white.opacity(0.5)),
                                           style: StrokeStyle(lineWidth: width + 2 / scale, lineCap: .round))
                            context.stroke(shape.path, with: .color(Corv.void), style: StrokeStyle(lineWidth: width, lineCap: .round))
                        } else {
                            context.stroke(shape.path, with: .color(.white.opacity(0.5)), lineWidth: 1 / scale)
                        }
                    }
                }
            }
        }
        .background(Color.white.opacity(0.02), in: .rect(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Corv.line))
        .contentShape(.rect)
        .gesture(MagnifyGesture()
            .onChanged { pinch = $0.magnification }
            .onEnded { zoom = min(max(zoom * $0.magnification, 0.2), 200); pinch = 1 })
        .simultaneousGesture(DragGesture()
            .onChanged { drag = $0.translation }
            .onEnded { pan.width += $0.translation.width; pan.height += $0.translation.height; drag = .zero })
        .onTapGesture(count: 2) { withAnimation(Corv.spring) { zoom = 1; pan = .zero } }
        .accessibilityElement()
        .accessibilityLabel("Gerber layers")
        .accessibilityIdentifier("gerberCanvas")
    }
}

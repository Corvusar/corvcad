import SwiftUI

/// Sketch mode (FreeCAD edition): closed profiles drawn on one face of the enclosure, then cut in
/// or extruded out. The session makes each profile a fully constrained FreeCAD Sketcher sketch,
/// with the horizontal and vertical edges inferred here while drawing, and a Pad or Pocket.
struct SketchEditor: View {
    let design: Design
    /// The sketch's index in `design.sketches`; nil for a new one.
    let index: Int?
    @Environment(\.dismiss) private var dismiss
    /// A working copy, seeded once; the enclosure changes only on Apply, as one undo step.
    @State private var sketch: UserSketch
    @State private var tool = Tool.rect
    @State private var selection: Int?
    /// The selected polygon edge (from corner `edge` to the next).
    @State private var edge: Int?
    @State private var edgeLength = 10.0
    /// A rectangle or circle under the finger.
    @State private var draft: SketchShape?
    /// The polygon being drawn, and where its next corner would go.
    @State private var corners: [CGPoint] = []
    @State private var next: CGPoint?
    /// The selected shape as it was when a move began.
    @State private var moving: SketchShape?
    /// A drag on the selected edge's bend handle.
    @State private var bending = false
    @State private var message: String?

    enum Tool: Hashable { case select, rect, circle, polygon }

    init(design: Design, index: Int?) {
        self.design = design
        self.index = index
        _sketch = State(initialValue: index.map { design.sketches[$0] }
                        ?? UserSketch(face: "lid", operation: "cut", depth: nil, shapes: []))
    }

    private static let faces = ["lid", "front", "back", "left", "right"]
    private static let grid = 0.5

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            if let message { Text(message).font(.caption).foregroundStyle(Corv.red).accessibilityIdentifier("sketchMessage") }
            HStack(alignment: .top, spacing: 12) {
                ToolRail(items: [.init(tool: .select, title: "Select", icon: "hand.point.up.left"),
                                 .init(tool: .rect, title: "Rect", icon: "rectangle"),
                                 .init(tool: .circle, title: "Circle", icon: "circle"),
                                 .init(tool: .polygon, title: "Lines", icon: "skew")],
                         selection: $tool)
                canvas
                    .background(Color.white.opacity(0.02), in: .rect(cornerRadius: 12))
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Corv.line))
                ScrollView { sidebar.frame(width: 290) }
            }
        }
        .padding(20)
        .background(Corv.void)
        .onChange(of: tool) { corners = []; next = nil; if tool != .select { selection = nil; edge = nil } }
    }

    private var header: some View {
        HStack(spacing: 8) {
            MonoLabel("\(index.map { "Sketch \($0 + 1)" } ?? "New sketch") · \(sketch.faceName)", color: Corv.gold)
            Spacer()
            Button("Cancel") { dismiss() }.buttonStyle(.corvGhost)
            Button(sketch.cut ? "Cut" : "Extrude") { apply() }
                .buttonStyle(.corvPrimary)
                .disabled(sketch.shapes.isEmpty)
                .accessibilityIdentifier("sketchApply")
        }
    }

    // MARK: Face geometry

    /// The face's extent and the region its shapes must stay in, in face millimetres (the
    /// session's rule: cuts stay off the corners and 1 mm inside the lid's edge).
    private var regions: (face: CGRect, allowed: CGRect) {
        let b = design.bounds, s = design.settings
        let outer = s.margin + s.wall
        if sketch.face == "lid" {
            let full = b.insetBy(dx: -(outer - s.lid_gap), dy: -(outer - s.lid_gap))
            return (full, sketch.cut ? full.insetBy(dx: 1, dy: 1) : full)
        }
        let along = sketch.face == "front" || sketch.face == "back" ? (b.minX, b.maxX) : (b.minY, b.maxY)
        let full = CGRect(x: along.0 - outer, y: 0, width: along.1 - along.0 + 2 * outer, height: design.trayHeight)
        return (full, sketch.cut ? full.insetBy(dx: s.wall, dy: 0) : full)
    }

    /// Cutouts and vent slots already on this face.
    private var openings: [CGRect] {
        let face = sketch.face, height = design.trayHeight
        var boxes = design.cutouts.filter { $0.side == face }.map {
            CGRect(x: $0.center - $0.width / 2, y: $0.bottom, width: $0.width, height: min($0.height, height - $0.bottom))
        }
        for row in design.vents where row.face == face {
            let first = row.center - Double(row.count - 1) * row.pitch / 2
            for k in 0..<row.count {
                let x = first + Double(k) * row.pitch - row.width / 2
                boxes.append(face == "lid" ? CGRect(x: x, y: row.across - row.length / 2, width: row.width, height: row.length)
                                           : CGRect(x: x, y: row.across, width: row.width, height: min(row.length, height - row.across)))
            }
        }
        return boxes
    }

    private var otherShapes: [SketchShape] {
        design.sketches.enumerated().filter { $0.offset != index && $0.element.face == sketch.face }.flatMap(\.element.shapes)
    }

    /// Walls are drawn as seen from outside, z up; the lid from above, as in the PCB view.
    private func frame(_ size: CGSize) -> FaceFrame? {
        FaceFrame(face: regions.face, flipU: sketch.face == "back" || sketch.face == "left",
                  flipV: sketch.face != "lid", size: size)
    }

    // MARK: Canvas

    private var canvas: some View {
        GeometryReader { geometry in
            Canvas { context, size in
                guard let f = frame(size) else { return }
                draw(&context, f)
            }
            .contentShape(.rect)
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { value in
                    guard let f = frame(geometry.size) else { return }
                    changed(value, f)
                }
                .onEnded { value in
                    guard let f = frame(geometry.size) else { return }
                    ended(value, f)
                })
            .accessibilityElement()
            .accessibilityLabel("Sketch canvas, \(sketch.faceName)")
            .accessibilityValue("\(sketch.shapes.count) shapes")
            .accessibilityIdentifier("sketchCanvas")
            .accessibilityAddTraits(.allowsDirectInteraction)
        }
        .frame(minWidth: 300, minHeight: 300)
    }

    private func draw(_ context: inout GraphicsContext, _ f: FaceFrame) {
        let (face, allowed) = regions
        var lines = Path()
        for x in stride(from: (face.minX / 5).rounded(.up) * 5, through: face.maxX, by: 5) {
            lines.move(to: f.view(CGPoint(x: x, y: face.minY))); lines.addLine(to: f.view(CGPoint(x: x, y: face.maxY)))
        }
        for y in stride(from: (face.minY / 5).rounded(.up) * 5, through: face.maxY, by: 5) {
            lines.move(to: f.view(CGPoint(x: face.minX, y: y))); lines.addLine(to: f.view(CGPoint(x: face.maxX, y: y)))
        }
        context.stroke(lines, with: .color(.white.opacity(0.05)), lineWidth: 0.6)
        context.stroke(Path(f.rect(face)), with: .color(Corv.lineStrong), lineWidth: 1.2)
        context.stroke(Path(f.rect(allowed)), with: .color(Corv.textFaint), style: StrokeStyle(lineWidth: 0.8, dash: [4, 4]))
        for box in openings { context.fill(Path(f.rect(box)), with: .color(.white.opacity(0.12))) }
        for shape in otherShapes { context.stroke(path(shape, f), with: .color(Corv.textFaint), lineWidth: 1) }
        for (i, shape) in sketch.shapes.enumerated() {
            let selected = i == selection
            context.fill(path(shape, f), with: .color(Corv.gold.opacity(sketch.cut ? 0.12 : 0.22)))
            context.stroke(path(shape, f), with: .color(selected ? Corv.cyan : Corv.gold), lineWidth: selected ? 2 : 1.4)
            if case let .polygon(points, bulges) = shape { glyphs(&context, points, f, closed: true, bulges: bulges) }
            if selected { dimensions(&context, shape, f) }
        }
        if let selection, let edge, sketch.shapes.indices.contains(selection),
           case let .polygon(points, bulges) = sketch.shapes[selection], edge < points.count {
            let b = points[(edge + 1) % points.count]
            var line = Path()
            line.addLines((SketchShape.edge(points[edge], b, bulges[edge]) + [b]).map(f.view))
            context.stroke(line, with: .color(Corv.cyan), lineWidth: 3)
            // The bend handle: drag it to bow the edge into an arc through it.
            let handle = f.view(bendPoint(points[edge], b, bulges[edge]))
            context.fill(Path(ellipseIn: CGRect(x: handle.x - 9, y: handle.y - 9, width: 18, height: 18)), with: .color(Corv.gold))
            context.stroke(Path(ellipseIn: CGRect(x: handle.x - 9, y: handle.y - 9, width: 18, height: 18)), with: .color(Corv.void), lineWidth: 2)
        }
        if let draft {
            context.stroke(path(draft, f), with: .color(Corv.gold), style: StrokeStyle(lineWidth: 1.4, dash: [5, 4]))
            dimensions(&context, draft, f)
        }
        if !corners.isEmpty {
            let shown = corners + (next.map { [$0] } ?? [])
            var line = Path()
            line.addLines(shown.map(f.view))
            context.stroke(line, with: .color(Corv.gold), lineWidth: 1.4)
            glyphs(&context, shown, f, closed: false)
            let first = f.view(corners[0])
            context.stroke(Path(ellipseIn: CGRect(x: first.x - 10, y: first.y - 10, width: 20, height: 20)),
                           with: .color(corners.count >= 3 ? Corv.cyan : Corv.gold.opacity(0.5)), lineWidth: 1.2)
            for corner in corners {
                let p = f.view(corner)
                context.fill(Path(ellipseIn: CGRect(x: p.x - 3, y: p.y - 3, width: 6, height: 6)), with: .color(Corv.gold))
            }
        }
    }

    /// The horizontal and vertical constraints FreeCAD will get, marked at each edge's middle.
    private func glyphs(_ context: inout GraphicsContext, _ points: [CGPoint], _ f: FaceFrame, closed: Bool,
                        bulges: [Double] = []) {
        let count = closed ? points.count : points.count - 1
        guard count > 0 else { return }
        for i in 0..<count {
            let a = points[i], b = points[(i + 1) % points.count]
            guard a != b, a.x == b.x || a.y == b.y, bulges.indices.contains(i) ? bulges[i] == 0 : true else { continue }
            let middle = f.view(CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2))
            let offset: CGPoint = a.y == b.y ? CGPoint(x: 0, y: -10) : CGPoint(x: 10, y: 0)
            context.draw(Text(a.y == b.y ? "H" : "V").font(.corvMonoSmall).foregroundStyle(Corv.cyan),
                         at: CGPoint(x: middle.x + offset.x, y: middle.y + offset.y))
        }
    }

    private func dimensions(_ context: inout GraphicsContext, _ shape: SketchShape, _ f: FaceFrame) {
        let box = f.rect(shape.extent)
        let text: String
        switch shape {
        case let .rect(_, _, width, height): text = String(format: "%.1f × %.1f", width, height)
        case let .circle(_, _, diameter): text = String(format: "⌀ %.1f", diameter)
        case .polygon: text = String(format: "%.1f × %.1f", shape.extent.width, shape.extent.height)
        }
        context.draw(Text(text).font(.corvMonoSmall).foregroundStyle(Corv.cyan), at: CGPoint(x: box.midX, y: box.minY - 26))  // clear of an edge's H glyph
    }

    private func path(_ shape: SketchShape, _ f: FaceFrame) -> Path {
        switch shape {
        case .rect: return Path(f.rect(shape.extent))
        case .circle: return Path(ellipseIn: f.rect(shape.extent))
        case let .polygon(points, bulges): return Path { $0.addLines(SketchShape.flatten(points, bulges).map(f.view)); $0.closeSubpath() }
        }
    }

    // MARK: Touch

    private func snap(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x / Self.grid).rounded() * Self.grid, y: (p.y / Self.grid).rounded() * Self.grid)
    }

    /// A polygon corner on the grid, aligned with the last and first corners when within 10 pt:
    /// that is the horizontal or vertical edge FreeCAD will be told about.
    private func corner(at location: CGPoint, _ f: FaceFrame) -> CGPoint {
        var p = snap(f.model(location))
        for anchor in [corners.first, corners.last].compactMap({ $0 }) {  // the last corner wins
            let a = f.view(anchor)
            if abs(a.x - location.x) < 10 { p.x = anchor.x }
            if abs(a.y - location.y) < 10 { p.y = anchor.y }
        }
        return p
    }

    private func changed(_ value: DragGesture.Value, _ f: FaceFrame) {
        let start = snap(f.model(value.startLocation)), now = snap(f.model(value.location))
        switch tool {
        case .rect:
            draft = .rect(x: min(start.x, now.x), y: min(start.y, now.y), width: abs(now.x - start.x), height: abs(now.y - start.y))
        case .circle:
            let radius = hypot(now.x - start.x, now.y - start.y)
            draft = .circle(x: start.x, y: start.y, diameter: (radius * 2 / Self.grid).rounded() * Self.grid)
        case .polygon:
            next = corner(at: value.location, f)
        case .select:
            guard let selection, sketch.shapes.indices.contains(selection) else { return }
            if bending || moving == nil, let edge, case let .polygon(points, bulges) = sketch.shapes[selection], edge < points.count {
                let a = points[edge], b = points[(edge + 1) % points.count]
                let handle = f.view(bendPoint(a, b, bulges[edge]))
                if bending || hypot(handle.x - value.startLocation.x, handle.y - value.startLocation.y) < 22 {
                    bending = true
                    var next = bulges
                    next[edge] = bulge(a, b, through: f.model(value.location))
                    sketch.shapes[selection] = .polygon(points, next)
                    return
                }
            }
            let original = moving ?? sketch.shapes[selection]
            if moving == nil {
                guard path(original, f).contains(value.startLocation) else { return }
                moving = original
            }
            sketch.shapes[selection] = original.moved(by: CGSize(width: now.x - start.x, height: now.y - start.y))
        }
    }

    private func ended(_ value: DragGesture.Value, _ f: FaceFrame) {
        let tap = hypot(value.translation.width, value.translation.height) < 6
        defer { draft = nil; moving = nil; bending = false }
        message = nil
        switch tool {
        case .rect, .circle:
            if let draft, draft.extent.width >= 0.5, draft.extent.height >= 0.5 {
                sketch.shapes.append(draft)
                selection = sketch.shapes.count - 1
            } else if tap {
                select(at: value.location, f)
            }
        case .polygon:
            if corners.count >= 3, hypot(f.view(corners[0]).x - value.location.x, f.view(corners[0]).y - value.location.y) < 16 {
                sketch.shapes.append(.polygon(corners, Array(repeating: 0, count: corners.count)))
                selection = sketch.shapes.count - 1
                corners = []
            } else {
                let p = corner(at: value.location, f)
                if p != corners.last { corners.append(p) }
            }
            next = nil
        case .select:
            if tap, !bending {  // a tap moves nothing, even across a grid line
                if let moving, let selection, sketch.shapes.indices.contains(selection) { sketch.shapes[selection] = moving }
                select(at: value.location, f)
            }
        }
    }

    /// A polygon edge within 14 pt (selecting its polygon), else the topmost shape under the finger.
    private func select(at location: CGPoint, _ f: FaceFrame) {
        for (i, shape) in sketch.shapes.enumerated().reversed() {
            guard case let .polygon(points, bulges) = shape else { continue }
            for k in points.indices {
                let a = points[k], b = points[(k + 1) % points.count]
                let chain = (SketchShape.edge(a, b, bulges[k]) + [b]).map(f.view)
                guard zip(chain, chain.dropFirst()).contains(where: { distance(location, $0, $1) < 14 }) else { continue }
                selection = i
                edge = k
                edgeLength = (hypot(b.x - a.x, b.y - a.y) * 100).rounded() / 100
                return
            }
        }
        edge = nil
        selection = sketch.shapes.indices.reversed().first { path(sketch.shapes[$0], f).contains(location) }
    }

    private func distance(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
        let ab = CGPoint(x: b.x - a.x, y: b.y - a.y)
        let t = max(0, min(1, ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / max(ab.x * ab.x + ab.y * ab.y, 1e-9)))
        return hypot(p.x - a.x - t * ab.x, p.y - a.y - t * ab.y)
    }

    // MARK: Sidebar

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 22) {
            InspectorSection("Face") {
                Picker("Face", selection: $sketch.face) {
                    ForEach(Self.faces, id: \.self) { face in
                        Text(face == "lid" ? "Lid" : VentRow.faceNames[face]?.replacingOccurrences(of: " wall", with: "") ?? face).tag(face)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(!sketch.shapes.isEmpty || !corners.isEmpty)
                .accessibilityIdentifier("sketchFace")
                Text(sketch.face == "lid" ? "The lid's top, seen from above."
                     : "The wall's outer face, seen from outside; up is up.")
                    .font(.caption).foregroundStyle(Corv.textDim)
            }
            InspectorSection("Operation") {
                Picker("Operation", selection: Binding(get: { sketch.operation }, set: { operation in
                    sketch.operation = operation
                    sketch.depth = operation == "cut" ? nil : max(sketch.depth ?? 2, 2)
                })) {
                    Text("Cut").tag("cut")
                    Text("Extrude").tag("extrude")
                }
                .pickerStyle(.segmented)
                .accessibilityIdentifier("sketchOperation")
                if sketch.cut {
                    Toggle(isOn: Binding(get: { sketch.depth == nil }, set: { through in
                        sketch.depth = through ? nil : thickness / 2
                    })) { MonoLabel("Through", small: true) }
                        .tint(Corv.gold)
                        .accessibilityIdentifier("sketchThrough")
                }
                if let depth = sketch.depth {
                    let range = sketch.cut ? 0.2...max(thickness, 0.2) : 0.2...50.0
                    ParameterSlider(title: "Depth", value: Binding(get: { depth }, set: { sketch.depth = $0 }), range: range) {}
                    NumberField(title: "Depth mm", value: Binding(get: { depth }, set: {
                        sketch.depth = min(max($0, range.lowerBound), range.upperBound)
                    }))
                }
            }
            if let selection, sketch.shapes.indices.contains(selection) {
                InspectorSection("Shape \(selection + 1)") {
                    shapeFields(selection)
                    Button(role: .destructive) {
                        sketch.shapes.remove(at: selection)
                        self.selection = nil
                        edge = nil
                    } label: { Label("Delete shape", systemImage: "trash") }
                        .buttonStyle(.corvDanger)
                        .accessibilityIdentifier("deleteShape")
                }
            }
            InspectorSection("How to") {
                Text(help).font(.caption).foregroundStyle(Corv.textDim)
                CautionNote(text: "Shapes stay inside the dashed region and may not touch each other or the face's cutouts, vents and screw holes (1 mm clear); one may sit inside another's curve or notch. FreeCAD's Sketcher holds each one fully constrained.")
            }
        }
        .padding(.vertical, 4)
    }

    private var thickness: Double { sketch.face == "lid" ? design.settings.lid_thickness : design.settings.wall }

    private var help: String {
        switch tool {
        case .select: return "Tap a shape to select it and drag to move it; tap a polygon edge to set its length."
        case .rect: return "Drag a rectangle from corner to corner."
        case .circle: return "Drag from the centre outward."
        case .polygon: return "Tap or lift to place each corner; tap the first corner to close. Edges within 10 pt of horizontal or vertical snap to it (H, V). To make an arc, select an edge and drag its handle."
        }
    }

    @ViewBuilder private func shapeFields(_ i: Int) -> some View {
        switch sketch.shapes[i] {
        case let .rect(x, y, width, height):
            Grid(horizontalSpacing: 10, verticalSpacing: 10) {
                GridRow { field("X", x, i) { .rect(x: $0, y: y, width: width, height: height) }
                          field("Y", y, i) { .rect(x: x, y: $0, width: width, height: height) } }
                GridRow { field("Width", width, i) { .rect(x: x, y: y, width: $0, height: height) }
                          field("Height", height, i) { .rect(x: x, y: y, width: width, height: $0) } }
            }
        case let .circle(x, y, diameter):
            Grid(horizontalSpacing: 10, verticalSpacing: 10) {
                GridRow { field("X", x, i) { .circle(x: $0, y: y, diameter: diameter) }
                          field("Y", y, i) { .circle(x: x, y: $0, diameter: diameter) } }
                GridRow { field("Diameter", diameter, i) { .circle(x: x, y: y, diameter: $0) }; Color.clear }
            }
        case let .polygon(points, bulges):
            MonoLabel("\(points.count) corners · \(arcCount(bulges))", color: Corv.cyan, small: true)
            if let edge, edge < points.count {
                let a = points[edge], b = points[(edge + 1) % points.count]
                let chord = Double(hypot(b.x - a.x, b.y - a.y))
                if bulges[edge] == 0 {
                    NumberField(title: "Edge \(edge + 1) length mm", value: $edgeLength)
                        .accessibilityIdentifier("edgeLength")
                    Button("Set length") {
                        if case let .polygon(solvedPoints, solvedBulges)? = design.solveSketch(points, bulges: bulges, edge: edge, length: edgeLength) {
                            sketch.shapes[i] = .polygon(solvedPoints.map(snapNear), solvedBulges)
                        } else {
                            message = design.error
                            design.error = nil
                        }
                    }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("setEdgeLength")
                }
                // Signed: the arc's height off its chord, positive to the right going corner to corner.
                NumberField(title: "Edge \(edge + 1) arc height mm", value: Binding(get: { bulges[edge] * chord / 2 }, set: { height in
                    guard sketch.shapes.indices.contains(i) else { return }
                    var next = bulges
                    next[edge] = min(max(2 * height / chord, -1), 1)
                    sketch.shapes[i] = .polygon(points, next)
                }))
                .accessibilityIdentifier("arcHeight")
                if bulges[edge] != 0 {
                    Button("Straighten") {
                        var next = bulges
                        next[edge] = 0
                        sketch.shapes[i] = .polygon(points, next)
                    }
                    .buttonStyle(.corvGhost)
                }
            } else {
                Text("Tap an edge with the Select tool to set its length, or drag its handle to bend it into an arc.")
                    .font(.caption).foregroundStyle(Corv.textDim)
            }
        }
    }

    private func arcCount(_ bulges: [Double]) -> String {
        let count = bulges.filter { $0 != 0 }.count
        return "\(count) arc\(count == 1 ? "" : "s")"
    }

    /// Where an edge's bend handle sits: its arc's middle, or the chord's middle when straight.
    private func bendPoint(_ a: CGPoint, _ b: CGPoint, _ bulge: Double) -> CGPoint {
        let dx = Double(b.x - a.x), dy = Double(b.y - a.y), chord = max(hypot(dx, dy), 1e-9)
        let height = bulge * chord / 2  // to the right of a -> b
        return CGPoint(x: (a.x + b.x) / 2 + dy / chord * height, y: (a.y + b.y) / 2 - dx / chord * height)
    }

    /// The bulge of an arc from `a` to `b` through the point level with `p` off the chord's middle,
    /// its height on the 0.5 mm grid, at most a half circle.
    private func bulge(_ a: CGPoint, _ b: CGPoint, through p: CGPoint) -> Double {
        let dx = Double(b.x - a.x), dy = Double(b.y - a.y), chord = max(hypot(dx, dy), 1e-9)
        let height = (Double(p.x - (a.x + b.x) / 2) * dy - Double(p.y - (a.y + b.y) / 2) * dx) / chord
        return min(max(2 * (height / Self.grid).rounded() * Self.grid / chord, -1), 1)
    }

    /// The solver's coordinates, with float noise below 1 µm removed so equal x or y stay equal
    /// (they are the horizontal and vertical edges).
    private func snapNear(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x * 1e6).rounded() / 1e6, y: (p.y * 1e6).rounded() / 1e6)
    }

    private func field(_ title: String, _ value: Double, _ i: Int, _ make: @escaping (Double) -> SketchShape) -> some View {
        NumberField(title: "\(title) mm", value: Binding(get: { value }, set: { new in
            guard sketch.shapes.indices.contains(i) else { return }
            sketch.shapes[i] = make(new)
        }))
    }

    // MARK: Apply

    private func apply() {
        var sketches = design.sketches
        if let index, sketches.indices.contains(index) { sketches[index] = sketch } else { sketches.append(sketch) }
        if design.setSketches(sketches) {
            dismiss()
        } else {
            message = design.error ?? "The enclosure did not accept this sketch."
            design.error = nil
        }
    }
}

/// Face millimetres to canvas points and back, fitted with a margin.
private struct FaceFrame {
    let face: CGRect
    let flipU: Bool
    let flipV: Bool
    let scale: CGFloat
    let offset: CGPoint

    init?(face: CGRect, flipU: Bool, flipV: Bool, size: CGSize, padding: CGFloat = 36) {
        guard face.width > 0, face.height > 0, size.width > 2 * padding, size.height > 2 * padding else { return nil }
        self.face = face
        self.flipU = flipU
        self.flipV = flipV
        scale = min((size.width - 2 * padding) / face.width, (size.height - 2 * padding) / face.height)
        offset = CGPoint(x: (size.width - face.width * scale) / 2, y: (size.height - face.height * scale) / 2)
    }

    func view(_ p: CGPoint) -> CGPoint {
        CGPoint(x: offset.x + (flipU ? face.maxX - p.x : p.x - face.minX) * scale,
                y: offset.y + (flipV ? face.maxY - p.y : p.y - face.minY) * scale)
    }

    func model(_ p: CGPoint) -> CGPoint {
        let u = (p.x - offset.x) / scale, v = (p.y - offset.y) / scale
        return CGPoint(x: flipU ? face.maxX - u : face.minX + u, y: flipV ? face.maxY - v : face.minY + v)
    }

    func rect(_ r: CGRect) -> CGRect {
        let a = view(CGPoint(x: r.minX, y: r.minY)), b = view(CGPoint(x: r.maxX, y: r.maxY))
        return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y))
    }
}

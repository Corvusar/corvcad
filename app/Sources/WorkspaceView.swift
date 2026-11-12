import SwiftUI

struct ShareItem { let title: String; let url: URL }

struct WorkspaceHistory {
    let canUndo: Bool
    let canRedo: Bool
    let undo: () -> Void
    let redo: () -> Void
}

struct Stat { let title: String; let value: String; init(_ title: String, _ value: String) { self.title = title; self.value = value } }

enum WorkspaceMode: String, CaseIterable, Hashable {
    case schematic = "Schematic", pcb = "PCB", enclosure = "Enclosure", fabricate = "Fabricate"
    var icon: String {
        switch self {
        case .schematic: return "point.3.connected.trianglepath.dotted"
        case .pcb: return "cpu"
        case .enclosure: return "cube.transparent"
        case .fabricate: return "hammer"
        }
    }
}

/// Full-bleed workspace: top bar with mode switcher, canvas, slide-in inspector.
struct WorkspaceShell<Canvas: View, Inspector: View>: View {
    let title: String
    let subtitle: String
    let modes: [WorkspaceMode]
    @Binding var mode: WorkspaceMode
    let shareItems: [ShareItem]
    /// Undo/redo for local projects; nil hides the controls (imported files).
    let history: WorkspaceHistory?
    /// False for a mode whose canvas is its own inspector (Fabricate).
    var showsInspector = true
    let onBack: () -> Void
    @ViewBuilder let canvas: Canvas
    @ViewBuilder let inspector: Inspector
    @State private var showInspector = false
    @Namespace private var modeHighlight

    var body: some View {
        VStack(spacing: 0) {
            topBar
            canvas
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .overlay(alignment: .trailing) {
                    if showInspector && showsInspector {
                        ScrollView {
                            VStack(alignment: .leading, spacing: 22) { inspector }
                                .padding(20)
                        }
                        .scrollIndicators(.hidden)
                        .frame(width: 340)
                        .corvPanel(radius: 22)
                        .padding(.trailing, 16).padding(.vertical, 16)
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                    }
                }
                .clipped()
        }
        .background(Corv.void.ignoresSafeArea())
        .animation(Corv.spring, value: showInspector)
    }

    private var topBar: some View {
        HStack(spacing: 14) {
            Button(action: onBack) {
                Image(systemName: "chevron.left").font(.system(size: 15, weight: .semibold))
                    .frame(width: 38, height: 38)
                    .background(Circle().fill(Color.white.opacity(0.05)))
                    .overlay(Circle().strokeBorder(Corv.line))
            }
            .buttonStyle(.plain)
            .foregroundStyle(Corv.text)
            .accessibilityLabel("Projects")
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.system(size: 17, weight: .semibold)).foregroundStyle(Corv.text).lineLimit(1)
                MonoLabel(subtitle, color: Corv.textFaint, small: true)
            }
            .lineLimit(1)
            // The title keeps room for a few words: the mode switcher drops its labels first.
            .frame(minWidth: 130, maxWidth: 240, alignment: .leading)
            .layoutPriority(-1)
            Spacer(minLength: 8)
            ViewThatFits(in: .horizontal) {
                modeSwitcher(labels: true)
                modeSwitcher(labels: false)
            }
            Spacer(minLength: 8)
            if let history { historyControls(history) }
            if !shareItems.isEmpty {
                Menu {
                    ForEach(shareItems, id: \.url) { item in
                        ShareLink(item.title, item: item.url)
                    }
                } label: { barIcon("square.and.arrow.up") }
                .accessibilityLabel("Share")
            }
            if showsInspector {
                Button { showInspector.toggle() } label: {
                    barIcon("sidebar.right", active: showInspector)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(showInspector ? "Hide inspector" : "Show inspector")
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .background(Corv.space)
        .overlay(alignment: .bottom) { Rectangle().fill(Corv.line).frame(height: 1) }
    }

    private func modeSwitcher(labels: Bool) -> some View {
        HStack(spacing: 2) {
            ForEach(modes, id: \.self) { option in
                let selected = option == mode
                Button {
                    withAnimation(Corv.spring) { mode = option }
                } label: {
                    HStack(spacing: 7) {
                        Image(systemName: option.icon).font(.system(size: 13, weight: .semibold))
                        if labels { Text(option.rawValue).font(.system(size: 14, weight: .semibold)) }
                    }
                    .padding(.horizontal, 16).padding(.vertical, 9)
                    .foregroundStyle(selected ? Corv.void : Corv.textDim)
                    .background {
                        if selected {
                            Capsule().fill(Corv.gold)
                                .shadow(color: Corv.gold.opacity(0.4), radius: 10)
                                .matchedGeometryEffect(id: "mode", in: modeHighlight)
                        }
                    }
                    .contentShape(.capsule)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(option.rawValue)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
        .fixedSize()
        .padding(4)
        .background(Capsule().fill(Color.white.opacity(0.04)))
        .overlay(Capsule().strokeBorder(Corv.line))
        .sensoryFeedback(.selection, trigger: mode)
    }

    private func historyControls(_ history: WorkspaceHistory) -> some View {
        HStack(spacing: 0) {
            historyButton("arrow.uturn.backward", "Undo", enabled: history.canUndo, action: history.undo)
                .keyboardShortcut("z", modifiers: .command)
            Rectangle().fill(Corv.line).frame(width: 1, height: 20)
            historyButton("arrow.uturn.forward", "Redo", enabled: history.canRedo, action: history.redo)
                .keyboardShortcut("z", modifiers: [.command, .shift])
        }
        .background(Capsule().fill(Color.white.opacity(0.05)))
        .overlay(Capsule().strokeBorder(Corv.line))
    }

    private func historyButton(_ icon: String, _ title: String, enabled: Bool,
                               action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon).font(.system(size: 15, weight: .semibold))
                .frame(width: 44, height: 38)
                .foregroundStyle(enabled ? Corv.text : Corv.textFaint.opacity(0.5))
                .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .accessibilityLabel(title)
    }

    private func barIcon(_ name: String, active: Bool = false) -> some View {
        Image(systemName: name).font(.system(size: 15, weight: .medium))
            .frame(width: 38, height: 38)
            .foregroundStyle(active ? Corv.void : Corv.text)
            .background(Circle().fill(active ? AnyShapeStyle(Corv.gold) : AnyShapeStyle(Color.white.opacity(0.05))))
            .overlay(Circle().strokeBorder(active ? .clear : Corv.line))
    }
}

// MARK: - Inspector building blocks

struct InspectorSection<Content: View>: View {
    let title: String
    @ViewBuilder let content: Content
    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title; self.content = content()
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Rectangle().fill(Corv.gold).frame(width: 10, height: 1.5)
                MonoLabel(title, color: Corv.gold.opacity(0.9))
            }
            content
        }
    }
}

struct StatGrid: View {
    let items: [Stat]
    var body: some View {
        LazyVGrid(columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)], spacing: 10) {
            ForEach(items, id: \.title) { item in
                VStack(alignment: .leading, spacing: 4) {
                    Text(item.value).font(.system(size: 20, weight: .semibold, design: .monospaced))
                        .foregroundStyle(Corv.cyan).lineLimit(1).minimumScaleFactor(0.6)
                    MonoLabel(item.title, color: Corv.textFaint, small: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
                .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 12))
                .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Corv.line))
            }
        }
    }
}

struct ParameterSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>
    let onCommit: () -> Void
    /// The value under the finger during a drag. The model only receives the final value, in
    /// the same step as its commit, so no Slider write can land after a rejected edit's rollback.
    @State private var draft: Double?
    /// Bumped after each drag commit to rebuild the control: UISlider can settle its thumb where
    /// the finger lifted after the commit, and SwiftUI does not push an unchanged value back, so a
    /// rejected edit could leave the thumb off the restored value.
    @State private var generation = 0

    var body: some View {
        let shown = draft ?? value
        VStack(spacing: 6) {
            HStack {
                MonoLabel(title, small: true)
                Spacer()
                Text(String(format: "%.1f", shown)).font(.corvReadout).foregroundStyle(Corv.gold)
                    .monospacedDigit().contentTransition(.numericText(value: shown))
                MonoLabel("mm", color: Corv.textFaint, small: true)
            }
            // Slider can write its bound value again just after the drag ends; that write still
            // reaches the draft, which the next main-queue turn commits.
            Slider(value: Binding(get: { draft ?? value }, set: { if draft != nil { draft = $0 } }),
                   in: range, step: 0.5) { isEditing in
                if isEditing {
                    draft = value
                } else {
                    DispatchQueue.main.async {
                        guard let final = draft else { return }
                        draft = nil
                        value = final
                        onCommit()
                        generation += 1
                    }
                }
            }
            .id(generation)
            .tint(Corv.gold)
            .accessibilityLabel(title)
            .accessibilityAdjustableAction { direction in
                let step = direction == .increment ? 0.5 : direction == .decrement ? -0.5 : 0
                guard step != 0 else { return }
                value = min(max(value + step, range.lowerBound), range.upperBound)
                onCommit()
            }
        }
    }
}

struct NumberField: View {
    let title: String
    @Binding var value: Double
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            MonoLabel(title, color: Corv.textFaint, small: true)
            TextField(title, value: $value, format: .number)
                .keyboardType(.numbersAndPunctuation).corvField()
        }
    }
}

struct CautionNote: View {
    let text: String
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle").font(.caption).foregroundStyle(Corv.gold.opacity(0.8))
            Text(text).font(.caption).foregroundStyle(Corv.textFaint)
        }
    }
}

// MARK: - Inspectors

struct EnclosureInspector: View {
    @Bindable var design: Design
    let onCommit: (String, Double) -> Void

    var body: some View {
        if MechanicalSession.enabled && !design.mechanicalFeatures.isEmpty {
            EnclosureTimeline(design: design, onCommit: onCommit)
        } else {
            parameterSections
        }
        documentAndExport
    }

    @ViewBuilder private var parameterSections: some View {
        InspectorSection("Enclosure") {
            ParameterSlider(title: "Board margin", value: $design.settings.margin, range: 0...15) { onCommit("margin", design.settings.margin) }
            ParameterSlider(title: "Wall", value: $design.settings.wall, range: 1...6) { onCommit("wall", design.settings.wall) }
            ParameterSlider(title: "Floor", value: $design.settings.floor, range: 1...6) { onCommit("floor", design.settings.floor) }
            ParameterSlider(title: "Top clearance", value: $design.settings.board_clearance, range: 0...20) { onCommit("board_clearance", design.settings.board_clearance) }
        }
        .disabled(MechanicalSession.enabled && !design.mechanicalReady)
        InspectorSection("Standoffs") {
            ParameterSlider(title: "Height", value: $design.settings.standoff_height, range: 1...15) { onCommit("standoff_height", design.settings.standoff_height) }
            ParameterSlider(title: "Diameter", value: $design.settings.standoff_diameter, range: 3...12) { onCommit("standoff_diameter", design.settings.standoff_diameter) }
        }
        .disabled(MechanicalSession.enabled && !design.mechanicalReady)
        InspectorSection("Lid") {
            ParameterSlider(title: "Thickness", value: $design.settings.lid_thickness, range: 1...6) { onCommit("lid_thickness", design.settings.lid_thickness) }
            ParameterSlider(title: "Gap", value: $design.settings.lid_gap, range: 0...1) { onCommit("lid_gap", design.settings.lid_gap) }
        }
        .disabled(MechanicalSession.enabled && !design.mechanicalReady)
    }

    @ViewBuilder private var documentAndExport: some View {
        if MechanicalSession.enabled {
            InspectorSection("Document") {
                Button("Save enclosure") { design.saveMechanical() }.buttonStyle(.corvPrimary)
                    .accessibilityIdentifier("saveEnclosure")
                    .disabled(!design.mechanicalReady)
                Text(!design.mechanicalReady ? "Enclosure unavailable" : design.mechanicalDirty ? "Unsaved changes" : "Enclosure saved")
                    .font(.caption).foregroundStyle(Corv.textDim)
                    .accessibilityIdentifier("enclosureSaveStatus")
            }
        }
        InspectorSection("Export") {
            HStack(spacing: 10) {
                Button("Tray STL") { design.exportSTL() }.buttonStyle(.corvGhost).accessibilityIdentifier("exportTraySTL")
                Button("Lid STL") { design.exportLidSTL() }.buttonStyle(.corvGhost).accessibilityIdentifier("exportLidSTL")
            }
            if let url = design.exportURL { ShareLink("Share tray STL", item: url).foregroundStyle(Corv.gold) }
            if let url = design.lidExportURL { ShareLink("Share lid STL", item: url).foregroundStyle(Corv.gold) }
            CautionNote(text: MechanicalSession.enabled
                ? "Check fit and clearances before fabrication. Save the enclosure to keep parameter edits."
                : "Fit and layout prototypes with solid bosses. STEP export needs an Open CASCADE desktop build.")
        }
        .disabled(MechanicalSession.enabled && !design.mechanicalReady)
    }
}

/// The enclosure document's PartDesign features in tree order, each with the parameters that
/// drive it (FreeCAD edition). The base features are fixed; optional ones arrive with toggles.
private struct EnclosureTimeline: View {
    @Bindable var design: Design
    let onCommit: (String, Double) -> Void
    /// Rows with parameters start open; tapping a row folds it.
    @State private var folded: Set<String> = []
    @State private var sketchTarget: SketchTarget?

    private struct Parameter {
        let title: String
        let name: String
        let value: WritableKeyPath<cc_settings, Double>
        let range: ClosedRange<Double>
    }

    private static let parameters: [String: [Parameter]] = [
        "OuterPad": [.init(title: "Board margin", name: "margin", value: \.margin, range: 0...15),
                     .init(title: "Wall", name: "wall", value: \.wall, range: 1...6),
                     .init(title: "Floor", name: "floor", value: \.floor, range: 1...6),
                     .init(title: "Top clearance", name: "board_clearance", value: \.board_clearance, range: 0...20)],
        "BossPad": [.init(title: "Height", name: "standoff_height", value: \.standoff_height, range: 1...15),
                    .init(title: "Diameter", name: "standoff_diameter", value: \.standoff_diameter, range: 3...12)],
        "LidPad": [.init(title: "Thickness", name: "lid_thickness", value: \.lid_thickness, range: 1...6),
                   .init(title: "Gap", name: "lid_gap", value: \.lid_gap, range: 0...1)],
    ]

    var body: some View {
        InspectorSection("Timeline · FreeCAD") {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(design.mechanicalFeatures.enumerated()), id: \.element.id) { index, feature in
                    row(index + 1, feature, last: index == design.mechanicalFeatures.count - 1)
                }
            }
            .disabled(!design.mechanicalReady)
        }
        if design.box == nil {
            connectorSection
            ComponentClearance(design: design, onCommit: onCommit)
        }
        InspectorSection("Vents") {
            HStack(spacing: 8) {
                Button { design.setVents(design.vents + [design.defaultVents(on: "lid")]) } label: {
                    Label("Lid vents", systemImage: "plus")
                }
                .buttonStyle(.corvGhost)
                .accessibilityIdentifier("addLidVents")
                Menu {
                    ForEach(["front", "back", "left", "right"], id: \.self) { side in
                        let row = design.defaultVents(on: side)
                        Button(row.faceName) { design.setVents(design.vents + [row]) }
                    }
                } label: {
                    Label("Wall vents", systemImage: "plus")
                        .font(.system(size: 15, weight: .semibold))
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .background(Capsule().fill(Color.white.opacity(0.04)).overlay(Capsule().strokeBorder(Corv.lineStrong)))
                        .foregroundStyle(Corv.text)
                }
                .accessibilityIdentifier("addWallVents")
            }
            CautionNote(text: "Slot rows through the lid or a side wall, kept off the edges, the corners and other openings. Each row is a slot and a FreeCAD linear pattern.")
        }
        .disabled(!design.mechanicalReady || design.vents.count >= 6)
        InspectorSection("Lid screws") {
            Picker("Screws", selection: Binding(get: { design.screws?.screw ?? "None" }, set: { screw in
                design.setScrews(screw == "None" ? nil : LidScrews(screw: screw, insert: design.screws?.insert ?? false))
            })) {
                ForEach(["None", "M2", "M2.5", "M3"], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("lidScrews")
            if let screws = design.screws {
                Toggle(isOn: Binding(get: { screws.insert }, set: { insert in
                    design.setScrews(LidScrews(screw: screws.screw, insert: insert))
                })) { MonoLabel("Heat-set inserts", small: true) }
                    .tint(Corv.gold)
                    .accessibilityIdentifier("screwInserts")
                MonoLabel(String(format: "Corner posts %.1f mm · board margin at least %.1f mm",
                                 design.screwPosts[screws.postKey] ?? 0, design.screwPosts[screws.postKey] ?? 0),
                          color: Corv.cyan, small: true)
            }
            CautionNote(text: "Square posts in the cavity's corners with a pilot hole for self-tapping screws, or a hole for heat-set inserts, and clearance holes through the lid. Turning them on raises the board margin to fit the posts. Test-fit your screws before printing the whole box.")
        }
        .disabled(!design.mechanicalReady)
        InspectorSection("Sketches") {
            // A sketch's identity is its position: the session names its features UserSketch<n>.
            ForEach(design.sketches.indices, id: \.self) { i in
                let sketch = design.sketches[i]
                HStack(spacing: 10) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Sketch \(i + 1)").font(.system(size: 15, weight: .semibold)).foregroundStyle(Corv.text)
                        MonoLabel("\(sketch.faceName) · \(Self.operation(sketch)) · \(sketch.shapes.count) shape\(sketch.shapes.count == 1 ? "" : "s")",
                                  color: Corv.textFaint, small: true)
                    }
                    Spacer(minLength: 0)
                    Button { sketchTarget = SketchTarget(index: i) } label: { Image(systemName: "pencil") }
                        .buttonStyle(.corvGhost)
                        .accessibilityLabel("Edit sketch \(i + 1)")
                        .accessibilityIdentifier("editSketch-\(i + 1)")
                    Button {
                        var next = design.sketches
                        next.remove(at: i)
                        design.setSketches(next)
                    } label: { Image(systemName: "trash") }
                        .buttonStyle(.corvDanger)
                        .accessibilityLabel("Remove sketch \(i + 1)")
                        .accessibilityIdentifier("removeSketch-\(i + 1)")
                }
            }
            Button { sketchTarget = SketchTarget(index: nil) } label: { Label("New sketch", systemImage: "pencil.and.outline") }
                .buttonStyle(.corvGhost)
                .disabled(design.sketches.count >= 8)
                .accessibilityIdentifier("newSketch")
            CautionNote(text: "Rectangles, circles and polygons with arc edges on the lid or a wall, cut in (through or to a depth) or extruded out. Each is a FreeCAD Sketcher sketch and a Pad or Pocket.")
        }
        .disabled(!design.mechanicalReady)
        .fullScreenCover(item: $sketchTarget) { SketchEditor(design: design, index: $0.index) }
    }

    private var connectorSection: some View {
        InspectorSection("Connector cutouts") {
            let suggestions = design.connectorCutouts
            if suggestions.isEmpty {
                Text("No connector footprint (J, P, CN, USB, X) comes within 6 mm of a board edge.")
                    .font(.caption).foregroundStyle(Corv.textDim)
            }
            ForEach(suggestions, id: \.reference) { suggestion in
                Toggle(isOn: Binding(get: { design.cutouts.contains { matches($0, suggestion.cutout) } },
                                     set: { on in
                                         design.setCutouts(on ? design.cutouts + [suggestion.cutout]
                                                              : design.cutouts.filter { !matches($0, suggestion.cutout) })
                                     })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(suggestion.reference).font(.system(size: 15, weight: .semibold)).foregroundStyle(Corv.text)
                        MonoLabel(String(format: "%@ · %.1f mm wide", suggestion.cutout.edgeName, suggestion.cutout.width),
                                  color: Corv.textFaint, small: true)
                    }
                }
                .tint(Corv.gold)
                .accessibilityIdentifier("cutout-\(suggestion.reference)")
            }
            CautionNote(text: "Each cutout is a box through one wall, as wide as the connector's pads and as tall as its generated body (5 mm without one). Check the plug's clearance before printing.")
        }
        .disabled(!design.mechanicalReady)
    }

    private struct SketchTarget: Identifiable {
        let index: Int?
        var id: Int { index ?? -1 }
    }

    static func operation(_ sketch: UserSketch) -> String {
        if sketch.cut, sketch.depth == nil { return "cut through" }
        return String(format: "%@ %.1f mm", sketch.cut ? "cut" : "extrude", sketch.depth ?? 0)
    }

    /// The sketch behind a UserSketch<n>Cut or UserSketch<n>Extrude feature.
    private func sketchIndex(_ feature: MechanicalFeature) -> Int? {
        guard feature.name.hasPrefix("UserSketch"),
              let number = Int(feature.name.dropFirst(10).prefix { $0.isNumber }),
              design.sketches.indices.contains(number - 1) else { return nil }
        return number - 1
    }

    private func ventIndex(_ feature: MechanicalFeature) -> Int? {
        guard feature.type == "SubtractiveBox", feature.name.hasPrefix("Vent"),
              let number = Int(feature.name.dropFirst(4)), design.vents.indices.contains(number - 1) else { return nil }
        return number - 1
    }

    /// Slot count, width, length and pitch of vent row `index`, and its removal; each change is
    /// one undoable step.
    @ViewBuilder private func ventControls(_ index: Int) -> some View {
        if design.vents.indices.contains(index) {
            let row = design.vents[index]
            Stepper(value: Binding(get: { row.count }, set: { count in
                var next = design.vents
                next[index].count = count
                design.setVents(next)
            }), in: 1...40) {
                HStack {
                    MonoLabel("Slots", small: true)
                    Spacer()
                    Text("\(row.count)").font(.corvReadout).foregroundStyle(Corv.gold).monospacedDigit()
                }
            }
            .accessibilityIdentifier("ventCount-\(index + 1)")
            let fields: [(String, WritableKeyPath<VentRow, Double>, ClosedRange<Double>)] = [
                ("Slot width", \.width, 1...10), ("Slot length", \.length, 1...(row.face == "lid" ? 80 : max(design.trayHeight, 2))),
                ("Pitch", \.pitch, 2...20)]
            ForEach(fields, id: \.0) { title, field, range in
                ParameterSlider(title: title,
                                value: Binding(get: { design.vents.indices.contains(index) ? design.vents[index][keyPath: field] : range.lowerBound },
                                               set: { value in
                                                   guard design.vents.indices.contains(index) else { return }
                                                   var next = design.vents
                                                   next[index][keyPath: field] = value
                                                   design.setVents(next)
                                               }),
                                range: range) {}
            }
            Button(role: .destructive) {
                var next = design.vents
                next.remove(at: index)
                design.setVents(next)
            } label: { Label("Remove vents", systemImage: "trash") }
                .buttonStyle(.corvDanger)
                .accessibilityIdentifier("removeVents-\(index + 1)")
        }
    }

    private func matches(_ a: WallCutout, _ b: WallCutout) -> Bool {
        a.side == b.side && abs(a.center - b.center) < 0.01
    }

    /// Width, bottom and height of cutout `index`; each change is one undoable step.
    @ViewBuilder private func cutoutSliders(_ index: Int) -> some View {
        let fields: [(String, WritableKeyPath<WallCutout, Double>, ClosedRange<Double>)] = [
            ("Width", \.width, 1...60), ("Bottom", \.bottom, 0...max(design.trayHeight - 1, 0.5)), ("Height", \.height, 1...30)]
        ForEach(fields, id: \.0) { title, field, range in
            ParameterSlider(title: title,
                            value: Binding(get: { design.cutouts.indices.contains(index) ? design.cutouts[index][keyPath: field] : range.lowerBound },
                                           set: { value in
                                               guard design.cutouts.indices.contains(index) else { return }
                                               var next = design.cutouts
                                               next[index][keyPath: field] = value
                                               design.setCutouts(next)
                                           }),
                            range: range) {}
        }
    }

    private func cutoutIndex(_ feature: MechanicalFeature) -> Int? {
        guard feature.type == "SubtractiveBox", feature.name.hasPrefix("Cutout"),
              let number = Int(feature.name.dropFirst(6)), design.cutouts.indices.contains(number - 1) else { return nil }
        return number - 1
    }

    private func row(_ number: Int, _ feature: MechanicalFeature, last: Bool) -> some View {
        let cutout = cutoutIndex(feature)
        let vent = ventIndex(feature)
        let parameters = Self.parameters[feature.name] ?? []
        let open = (!parameters.isEmpty || cutout != nil || vent != nil) && !folded.contains(feature.name)
        return HStack(alignment: .top, spacing: 12) {
            VStack(spacing: 0) {
                Text("\(number)").font(.corvMonoSmall).foregroundStyle(Corv.void)
                    .frame(width: 22, height: 22).background(Circle().fill(feature.suppressed ? Corv.textFaint : Corv.gold))
                if !last { Rectangle().fill(Corv.line).frame(width: 1).frame(maxHeight: .infinity) }
            }
            VStack(alignment: .leading, spacing: 10) {
                Button {
                    guard !parameters.isEmpty || cutout != nil || vent != nil else { return }
                    withAnimation(Corv.spring) {
                        if folded.contains(feature.name) { folded.remove(feature.name) } else { folded.insert(feature.name) }
                    }
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: icon(feature.type)).foregroundStyle(Corv.gold).frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(title(feature)).font(.system(size: 15, weight: .semibold)).foregroundStyle(Corv.text)
                            MonoLabel([feature.type, feature.profile].compactMap { $0 }.joined(separator: " · "),
                                      color: Corv.textFaint, small: true)
                        }
                        Spacer(minLength: 0)
                        if !parameters.isEmpty || cutout != nil || vent != nil {
                            Image(systemName: "chevron.down").font(.system(size: 12, weight: .semibold))
                                .rotationEffect(.degrees(open ? 0 : -90)).foregroundStyle(Corv.textDim)
                        }
                    }
                    .contentShape(.rect)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(title(feature))
                .accessibilityValue(parameters.isEmpty && cutout == nil && vent == nil ? feature.type : open ? "Expanded" : "Collapsed")
                .accessibilityIdentifier("feature-\(feature.name)")
                if open, let cutout {
                    cutoutSliders(cutout)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                } else if open, let vent {
                    ventControls(vent)
                        .transition(.opacity.combined(with: .move(edge: .top)))
                } else if open {
                    ForEach(parameters, id: \.name) { parameter in
                        ParameterSlider(title: design.box != nil && parameter.name == "margin" ? "Margin" : parameter.title,
                                        value: Binding(get: { design.settings[keyPath: parameter.value] },
                                                       set: { design.settings[keyPath: parameter.value] = $0 }),
                                        range: parameter.range) {
                            onCommit(parameter.name, design.settings[keyPath: parameter.value])
                        }
                    }
                    .transition(.opacity.combined(with: .move(edge: .top)))
                } else if parameters.isEmpty, let note = note(feature) {
                    Text(note).font(.caption).foregroundStyle(Corv.textDim)
                }
            }
            .padding(.bottom, last ? 0 : 16)
        }
    }

    private func title(_ feature: MechanicalFeature) -> String {
        switch feature.name {
        case "OuterPad": return "Enclosure body"
        case "Cavity": return "Cavity"
        case "BossPad": return "Standoffs"
        case "LidPad": return "Lid"
        case "ScrewPost": return "Screw post"
        case "ScrewPosts": return "Screw posts · four corners"
        case "ScrewHole": return design.screws?.insert == true ? "Insert hole" : "Pilot hole"
        case "ScrewHoles": return "Screw holes · four corners"
        case "LidScrewHole": return "Lid clearance hole"
        case "LidScrewHoles": return "Lid holes · four corners"
        default:
            if let index = cutoutIndex(feature) { return "Cutout \(index + 1) · \(design.cutouts[index].edgeName)" }
            if let index = ventIndex(feature) { return "Vents \(index + 1) · \(design.vents[index].faceName)" }
            if let index = sketchIndex(feature) { return "Sketch \(index + 1) · \(design.sketches[index].faceName)" }
            if feature.type == "LinearPattern" { return "Vents \(feature.name.dropFirst(4).dropLast(7)) · repeat" }
            if feature.name.hasPrefix("MountingBore") { return "Mounting bore \(feature.name.dropFirst(12))" }
            return feature.label
        }
    }

    private func note(_ feature: MechanicalFeature) -> String? {
        if feature.name == "Cavity" {
            return design.box != nil ? "The inside space you typed, plus the margin and top clearance."
                                     : "Board space from the margin, standoffs and top clearance."
        }
        if let index = sketchIndex(feature) {
            return "FreeCAD Sketcher sketch \(feature.profile ?? ""), \(Self.operation(design.sketches[index])). Edit it in Sketches below."
        }
        if feature.type == "Hole" { return "Through bore sized from the board's mounting hole." }
        if feature.name.hasSuffix("Screws") || feature.name.hasSuffix("Holes") || feature.name.hasSuffix("Posts") {
            return "Repeated in the cavity's four corners (FreeCAD linear pattern, 2 × 2)."
        }
        if let screws = design.screws, feature.name.hasPrefix("Screw") || feature.name.hasPrefix("LidScrew") {
            return "\(screws.screw)\(screws.insert ? " with heat-set inserts" : ", self-tapping"). Set in Lid screws below."
        }
        if feature.type == "LinearPattern", let number = Int(feature.name.dropFirst(4).dropLast(7)),
           design.vents.indices.contains(number - 1) {
            let row = design.vents[number - 1]
            return String(format: "%d slots, %.1f mm apart (FreeCAD linear pattern).", row.count, row.pitch)
        }
        return nil
    }

    private func icon(_ type: String) -> String {
        switch type {
        case "Pad": return "square.stack.3d.up"
        case "Pocket": return "square.dashed.inset.filled"
        case "Hole": return "circle.dashed"
        case "SubtractiveBox": return "rectangle.dashed"
        case "LinearPattern": return "square.grid.3x1.below.line.grid.1x2"
        case "AdditiveBox": return "square.fill"
        case "SubtractiveCylinder": return "circle.circle"
        default: return "cube"
        }
    }
}

struct BoardInspector: View {
    @Bindable var design: Design
    /// The project's board file; nil for an individually imported board.
    let projectBoard: URL?

    var body: some View {
        InspectorSection("Board") {
            Text(design.boardName).font(.headline).foregroundStyle(Corv.text)
            StatGrid(items: [
                Stat("Size mm", String(format: "%.1f×%.1f", design.width, design.height)),
                Stat("Footprints", "\(design.placements.count)"),
                Stat("Nets", "\(design.nets.count)"),
                // Arcs arrive as chords sharing their UUID.
                Stat("Tracks", "\(Set(design.tracks.map { schematicText($0.uuid) }).count)"),
                Stat("Vias", "\(design.vias.count)"),
                Stat("Holes", "\(design.holes.count)"),
            ] + (Design.hasKiCadDRC ? [Stat("Unrouted", "\(design.ratsnest.count)")] : []))
        }
        if Design.hasKiCadDRC, let projectBoard {
            BoardCheckSection(design: design, board: projectBoard)
            if cc_board_native_backend() != nil {
                BoardSetupSection(design: design, board: projectBoard)
                FabricationSection(design: design, board: projectBoard)
            }
        }
        if !design.footprintMarkers.isEmpty {
            InspectorSection("Footprint") {
                Picker("Reference", selection: $design.selectedFootprint) {
                    ForEach(design.footprintMarkers.indices, id: \.self) { index in
                        Text(design.footprintMarkers[index].2).tag(index)
                    }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                .onChange(of: design.selectedFootprint) { _, index in design.selectFootprint(index) }
                HStack(spacing: 10) {
                    NumberField(title: "X mm", value: $design.editX)
                    NumberField(title: "Y mm", value: $design.editY)
                    NumberField(title: "Rot °", value: $design.editRotation)
                }
                Button(projectBoard == nil ? "Save moved copy" : "Apply move") {
                    design.saveMovedFootprint(to: projectBoard)
                }
                .buttonStyle(.corvPrimary)
                if let url = design.editedBoardURL {
                    ShareLink("Share edited board", item: url).foregroundStyle(Corv.gold)
                }
                CautionNote(text: "Tracks are not rerouted. Run KiCad DRC before fabrication.")
            }
        }
        if !design.holes.isEmpty {
            InspectorSection("Mounting holes") {
                Text(design.holes.joined(separator: "\n")).font(.corvMono).foregroundStyle(Corv.textDim)
            }
        }
    }
}

/// KiCad's electrical rules check of the whole project (NATIVE_KICAD sessions only). Findings
/// on the sheet shown are marked on the canvas; picking one in another sheet says which.
private struct ERCSection: View {
    let schematic: SchematicDesign

    var body: some View {
        InspectorSection("Electrical rules · KiCad") {
            Button { Task { await schematic.runERC() } } label: {
                Label(schematic.ercRunning ? "Checking…" : "Run ERC", systemImage: "checkmark.shield")
            }
            .buttonStyle(.corvPrimary)
            .disabled(schematic.ercRunning)
            .accessibilityIdentifier("runERC")
            if let findings = schematic.erc {
                let errors = findings.filter(\.isError).count
                StatGrid(items: [Stat("Errors", "\(errors)"), Stat("Warnings", "\(findings.count - errors)")])
                // ponytail: first 200 findings, as the DRC list.
                ForEach(findings.prefix(200)) { finding in
                    Button { schematic.selectedERC = schematic.selectedERC == finding.id ? nil : finding.id } label: {
                        HStack(alignment: .top, spacing: 8) {
                            Circle().fill(finding.isError ? Corv.red : Corv.gold.opacity(0.7))
                                .frame(width: 7, height: 7).padding(.top, 5)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(finding.description).font(.caption).foregroundStyle(Corv.text)
                                if schematic.selectedERC == finding.id {
                                    if !finding.detail.isEmpty {
                                        Text(finding.detail).font(.corvMonoSmall).foregroundStyle(Corv.textDim)
                                    }
                                    if !schematic.ercOnDisplayedSheet(finding) {
                                        MonoLabel("On sheet \(schematic.ercSheetName(finding))", color: Corv.cyan, small: true)
                                    }
                                }
                            }
                            Spacer(minLength: 0)
                        }
                        .padding(8)
                        .background(schematic.selectedERC == finding.id ? Corv.gold.opacity(0.08) : .clear, in: .rect(cornerRadius: 8))
                        .contentShape(.rect)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("\(finding.isError ? "Error" : "Warning"): \(finding.description)")
                }
                if findings.count > 200 {
                    MonoLabel("\(findings.count - 200) more", color: Corv.textFaint, small: true)
                }
            }
            CautionNote(text: "KiCad 10.0.6 ERC with kicad-cli's defaults on the saved files, the footprint link check included. An edit clears the result.")
        }
    }
}

/// KiCad's Annotate and BOM for the whole project (NATIVE_KICAD sessions only).
private struct SchematicToolsSection: View {
    let schematic: SchematicDesign
    @State private var bomURL: URL?
    @State private var netlistURL: URL?
    @State private var plotFiles: [URL] = []
    @State private var confirmReset = false

    var body: some View {
        InspectorSection("Project · KiCad") {
            HStack(spacing: 8) {
                Button { schematic.annotate(reset: false) } label: {
                    Label("Annotate", systemImage: "number")
                }
                .buttonStyle(.corvGhost)
                .disabled(schematic.readOnlyReason != nil)
                .accessibilityIdentifier("annotate")
                Button { confirmReset = true } label: { Label("Renumber all", systemImage: "arrow.counterclockwise") }
                    .buttonStyle(.corvGhost)
                    .disabled(schematic.readOnlyReason != nil)
            }
            Button { bomURL = schematic.exportBOM() } label: {
                Label("Export BOM", systemImage: "list.bullet.rectangle")
            }
            .buttonStyle(.corvGhost)
            .accessibilityIdentifier("exportBOM")
            if let bomURL {
                ShareLink("Share \(bomURL.lastPathComponent)", item: bomURL).foregroundStyle(Corv.gold)
            }
            Button { netlistURL = schematic.exportNetlist() } label: {
                Label("Export netlist", systemImage: "point.3.connected.trianglepath.dotted")
            }
            .buttonStyle(.corvGhost)
            .accessibilityIdentifier("exportNetlist")
            if let netlistURL {
                ShareLink("Share \(netlistURL.lastPathComponent)", item: netlistURL).foregroundStyle(Corv.gold)
            }
            if schematic.plots {
                HStack(spacing: 8) {
                    Button { plotFiles = schematic.plot(svg: false) } label: {
                        Label("Plot PDF", systemImage: "doc.richtext")
                    }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("plotPDF")
                    Button { plotFiles = schematic.plot(svg: true) } label: {
                        Label("Plot SVG", systemImage: "square.on.square")
                    }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("plotSVG")
                }
                if !plotFiles.isEmpty {
                    ShareLink(items: plotFiles) {
                        Text(plotFiles.count == 1 ? "Share \(plotFiles[0].lastPathComponent)"
                                                  : "Share \(plotFiles.count) SVG sheets")
                    }
                    .foregroundStyle(Corv.gold)
                    .accessibilityIdentifier("sharePlot")
                }
            }
            CautionNote(text: "KiCad 10.0.6 annotation of every sheet (by X position, first free number; undoable). The BOM is grouped by value and footprint as kicad-cli writes it. Plots cover every sheet of the saved project, as kicad-cli writes them.")
        }
        .confirmationDialog("Renumber every symbol?", isPresented: $confirmReset, titleVisibility: .visible) {
            Button("Renumber all", role: .destructive) { schematic.annotate(reset: true) }
        } message: {
            Text("KiCad replaces every reference in the project. Update the board from the schematic afterwards.")
        }
    }
}

struct SchematicInspector: View {
    @Bindable var schematic: SchematicDesign

    var body: some View {
        InspectorSection("Sheet") {
            Text(schematic.name).font(.headline).foregroundStyle(Corv.text)
            StatGrid(items: [
                Stat("Symbols", "\(schematic.symbols.count)"),
                Stat("Wires", "\(schematic.wires.count)"),
                Stat("Labels", "\(schematic.labels.count)"),
                Stat("Junctions", "\(schematic.junctions.count)"),
            ])
            CautionNote(text: !schematic.sessionBacked
                ? "Imported sheet, read-only. Edits below save a separate copy. KiCad ERC remains the electrical check."
                : schematic.nativeKiCad
                ? "KiCad 10.0.6 core: edits run through KiCad's own schematic model and save as KiCad writes them. Run KiCad's ERC below."
                : "Compatible source editor: edits preserve the KiCad files, but connectivity and ERC are not computed here. Run KiCad ERC.")
            if let reason = schematic.readOnlyReason {
                CautionNote(text: "Read-only: \(reason)")
            }
        }
        if schematic.runsERC {
            ERCSection(schematic: schematic)
        }
        if schematic.simulates {
            SimulationSection(schematic: schematic)
        }
        if schematic.kicadTools {
            SchematicToolsSection(schematic: schematic)
        }
        if !schematic.symbols.isEmpty {
            InspectorSection("Symbol") {
                Picker("Reference", selection: $schematic.selectedSymbol) {
                    ForEach(schematic.symbols.indices, id: \.self) { index in
                        Text(schematicText(schematic.symbols[index].reference)).tag(index)
                    }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                .onChange(of: schematic.selectedSymbol) { _, index in schematic.select(index) }
                HStack(spacing: 10) {
                    NumberField(title: "X mm", value: $schematic.editX)
                    NumberField(title: "Y mm", value: $schematic.editY)
                }
                Button(schematic.sessionBacked ? "Apply move" : "Save moved copy") {
                    schematic.saveMove()
                }
                .buttonStyle(.corvGhost)
                propertyRow("Value", text: $schematic.editValue) { schematic.saveValue() }
                propertyRow("Footprint ID", text: $schematic.editFootprint) { schematic.saveFootprint() }
                if let url = schematic.editedURL {
                    ShareLink("Share edited schematic", item: url).foregroundStyle(Corv.gold)
                }
                CautionNote(text: "Wires stay in place after a move. Reconnect and run KiCad ERC.")
            }
        }
    }

    private func propertyRow(_ title: String, text: Binding<String>, save: @escaping () -> Void) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            MonoLabel(title, color: Corv.textFaint, small: true)
            HStack(spacing: 8) {
                TextField(title, text: text).autocorrectionDisabled().textInputAutocapitalization(.never).corvField()
                Button(action: save) {
                    Image(systemName: "checkmark").font(.system(size: 14, weight: .bold))
                        .frame(width: 40, height: 40)
                        .foregroundStyle(Corv.void)
                        .background(Circle().fill(Corv.gold))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Save \(title)")
            }
        }
    }
}

/// The generated parts' heights against the enclosure (FreeCAD edition, docs/3D_MODELS.md gate 5):
/// the tallest part against the lid's top clearance, the deepest reach under the board (leads, back
/// parts) against the standoffs. Each fix is one undoable parameter change.
private struct ComponentClearance: View {
    @Bindable var design: Design
    let onCommit: (String, Double) -> Void

    var body: some View {
        let heights = design.parts.heights
        let above = heights.max { $0.value.above < $1.value.above }
        let below = heights.max { $0.value.below < $1.value.below }
        InspectorSection("Component clearance") {
            if let above {
                check(part: above.key, reach: above.value.above, room: design.settings.board_clearance,
                      over: "above the board", named: "top clearance", fix: "Raise top clearance",
                      name: "board_clearance", value: \.board_clearance, limit: 20)
            }
            if let below, below.value.below > 0 {
                check(part: below.key, reach: below.value.below, room: design.settings.standoff_height,
                      over: "below the board", named: "standoff height", fix: "Raise standoffs",
                      name: "standoff_height", value: \.standoff_height, limit: 15)
            }
            if heights.isEmpty {
                Text("No generated part on this board yet: heights come from footprints whose 3D model the generator covers.")
                    .font(.caption).foregroundStyle(Corv.textDim)
            }
            if !design.parts.unknownHeights.isEmpty {
                CautionNote(text: "Not checked, no generated model: \(design.parts.unknownHeights.joined(separator: ", ")).")
            }
        }
        .disabled(!design.mechanicalReady)
    }

    @ViewBuilder private func check(part: String, reach: Double, room: Double, over: String, named roomName: String,
                                    fix: String, name: String, value: WritableKeyPath<cc_settings, Double>,
                                    limit: Double) -> some View {
        let fits = reach + 0.5 <= room
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: fits ? "checkmark.circle" : "exclamationmark.triangle.fill")
                    .foregroundStyle(fits ? Corv.cyan : Corv.red)
                Text(String(format: "%@ reaches %.1f mm %@; %@ %.1f mm", part, reach, over, roomName, room))
                    .font(.callout).foregroundStyle(fits ? Corv.textDim : Corv.text)
            }
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("clearance-\(name)")
            if !fits {
                let target = min(((reach + 0.5) * 10).rounded(.up) / 10, limit)
                Button("\(fix) to \(String(format: "%.1f", target)) mm") {
                    design.settings[keyPath: value] = target
                    onCommit(name, target)
                }
                .buttonStyle(.corvPrimary)
                .accessibilityIdentifier("fix-\(name)")
            }
        }
    }
}

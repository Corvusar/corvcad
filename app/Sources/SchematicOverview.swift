import SwiftUI

struct SchematicOverview: View {
    let symbols: [cc_sch_symbol]
    let wires: [cc_sch_wire]
    let junctions: [cc_sch_junction]
    let labels: [cc_sch_label]
    let noConnects: [cc_sch_junction]
    /// Portable connectivity of this sheet, for the net highlight.
    let nets: [SchematicDesign.SheetNet]
    /// Child-sheet boxes on this sheet and the breadcrumb to it (empty for a flat sheet).
    let sheets: [cc_sch_sheet]
    let trail: [SheetCrumb]
    let editable: Bool
    /// Why a project sheet cannot be edited here, shown as a banner.
    let readOnlyReason: String?
    let onAddTwoPin: (Double, Double) -> Void
    let onAddWire: (Double, Double, Double, Double) -> Void
    let onAddLabel: (String, Double, Double) -> Void
    let onSelectSymbol: (Int) -> Void
    let onDelete: (String) -> Void
    let onRenameSymbol: (String, String) -> Void
    let onRenameLabel: (String, String) -> Void
    let onMoveSymbol: (String, Double, Double) -> Void
    let onMoveLabel: (String, Double, Double) -> Void
    /// Saves a symbol's Value and Footprint (UUID, value, footprint) as one edit.
    let onSetFields: (String, String, String) -> Void
    let onRotateSymbol: (String) -> Void
    /// Engine route for a pending wire (KiCad millimetres); nil when blocked.
    let onRoute: (CGPoint, CGPoint) -> [CGPoint]?
    let onEnterSheet: (String) -> Void
    let onCrumb: (Int) -> Void

    /// Names the open file and sheet; the viewport is refitted only when it changes.
    let document: String
    /// The library part the Part tool places ("Library:Name"), and the picker that chooses it;
    /// nil when the edition has no KiCad libraries (the two-pin part is placed).
    var chosenPart: String? = nil
    var onChoosePart: (() -> Void)? = nil
    /// Library symbols' shapes and pins, parallel to `symbols` (empty: placeholders).
    var drawings: [SchematicDesign.SymbolDrawing] = []
    /// The sheet's paper and title block, drawn as the page frame (width 0: none).
    var page = cc_sch_page()
    /// The bundled footprints, for the footprint chooser, and the symbol index for each placed
    /// symbol's footprint filters and pin count; nil in the compatible edition.
    var footprints: FootprintLibrary? = nil
    var parts: PartLibrary? = nil
    /// Buses, wire-to-bus entries and each sheet box's pins (parallel to `sheets`).
    var buses: [cc_sch_wire] = []
    var busEntries: [cc_sch_wire] = []
    var sheetPins: [[cc_sch_label]] = []
    /// KiCad's editing tools (NATIVE_KICAD): the Bus, Entry and Sheet tools, label scopes and
    /// sheet pins. Off in the compatible edition, which has none of them.
    var kicadTools = false
    var onAddBus: (Double, Double, Double, Double) -> Void = { _, _, _, _ in }
    /// x, y on the bus and SCH_BUS_WIRE_ENTRY's quadrant (1 up-right … 4 up-left).
    var onAddBusEntry: (Double, Double, Int32) -> Void = { _, _, _ in }
    /// Name, x, y and kind (0 local, 1 global, 2 hierarchical).
    var onAddScopedLabel: (String, Double, Double, Int32) -> Void = { _, _, _, _ in }
    var onAddSheet: (String, CGRect) -> Void = { _, _ in }
    var onAddSheetPins: (String) -> Void = { _ in }
    /// Session API 8 (KiCad's tools): text notes and graphic note lines on this sheet, and the
    /// No-connect, Text and Line tools and the symbol Mirror action that add to them.
    var texts: [cc_sch_label] = []
    var noteLines: [cc_sch_wire] = []
    var onAddNoConnect: (Double, Double) -> Void = { _, _ in }
    /// Symbol UUID and axis: 0 flips top to bottom (KiCad's Mirror Vertically), 1 left to right.
    var onMirrorSymbol: (String, Int32) -> Void = { _, _ in }
    var onAddText: (String, Double, Double) -> Void = { _, _, _ in }
    var onAddNoteLine: (Double, Double, Double, Double) -> Void = { _, _, _, _ in }
    /// Session API 9 (KiCad's tools): bus-to-bus entries (x, y, quadrant), a sheet resized
    /// (UUID, width, height) or deleted, and power symbols for the Part tool (a library ID).
    var onAddBusBusEntry: (Double, Double, Int32) -> Void = { _, _, _ in }
    var onResizeSheet: (String, Double, Double) -> Void = { _, _, _ in }
    var onChoosePower: ((String) -> Void)? = nil
    /// KiCad ERC findings on the sheet shown (sheet millimetres), and which one is picked.
    var ercMarks: [(point: CGPoint, error: Bool, selected: Bool)] = []
    /// While the simulation plot is open: a tapped net (an index of `nets`) or symbol.
    var onProbeNet: ((Int) -> Void)? = nil
    var onProbeSymbol: ((Int) -> Void)? = nil

    @State private var tool = Tool.select
    @State private var finger: CGPoint?
    @State private var canvasSize = CGSize.zero
    /// Viewport fixed when a sheet is shown, so edits never rescale the canvas.
    @State private var viewport: CGRect?
    @State private var wireStart: CGPoint?
    @State private var labelName = "NET1"
    @State private var selected: Target?
    /// Net shown highlighted after tapping a wire, label or pin with Select.
    @State private var highlightedNet: Int?
    @State private var radialTarget: Target?
    @State private var radialLocation = CGPoint.zero
    @State private var radialHover: Int?
    @State private var moveTarget: Target?
    @State private var renameTarget: Target?
    @State private var renameText = ""
    @State private var fieldsTarget: Target?
    @State private var valueText = ""
    @State private var footprintText = ""
    @State private var choosingFootprint = false
    /// Where the Part tool last placed a library part, to offer a footprint once it appears.
    @State private var placedAt: CGPoint?
    /// Label scope for the Label tool with KiCad's tools: 0 local, 1 global, 2 hierarchical.
    @State private var labelKind: Int32 = 0
    /// The Sheet tool's first corner, then the box waiting for its name.
    @State private var sheetStart: CGPoint?
    @State private var sheetBox: CGRect?
    @State private var sheetName = ""
    /// The Text tool's note.
    @State private var noteText = "Note"
    /// The Entry tool's kind with KiCad's tools: a wire-to-bus or a bus-to-bus entry.
    @State private var busToBus = false
    /// The sheet being resized: its new bottom-right corner is the next tap.
    @State private var resizeSheet: Int?

    private enum Tool: Hashable { case select, part, wire, label, bus, entry, sheet, noConnect, text, note }
    /// flag: a no-connect flag; text and note: a text note and a note line segment.
    private enum Target: Equatable { case symbol(Int), wire(Int), label(Int), sheet(Int), flag(Int), text(Int), note(Int) }
    private struct Snap {
        let point: CGPoint // KiCad millimetres
        let reason: String
        let magnetic: Bool
    }
    private static let toolItems: [ToolRail<Tool>.Item] = [
        .init(tool: .select, title: "Select", icon: "hand.point.up.left"),
        .init(tool: .part, title: "Part", icon: "cpu"),
        .init(tool: .wire, title: "Wire", icon: "line.diagonal"),
        .init(tool: .label, title: "Label", icon: "tag"),
    ]
    private static let kicadToolItems: [ToolRail<Tool>.Item] = toolItems + [
        .init(tool: .bus, title: "Bus", icon: "line.3.horizontal"),
        .init(tool: .entry, title: "Entry", icon: "arrow.down.right"),
        .init(tool: .sheet, title: "Sheet", icon: "square.on.square"),
        .init(tool: .noConnect, title: "NC", icon: "xmark"),
        .init(tool: .text, title: "Text", icon: "textformat"),
        .init(tool: .note, title: "Line", icon: "pencil.line"),
    ]
    private static let chrome = EdgeInsets(top: 84, leading: 100, bottom: 84, trailing: 36)

    private var bounds: CGRect { viewport ?? fittedBounds }

    private var fittedBounds: CGRect {
        var xs = symbols.map(\.x), ys = symbols.map(\.y)
        for wire in wires { xs += [wire.x0, wire.x1]; ys += [wire.y0, wire.y1] }
        for junction in junctions { xs.append(junction.x); ys.append(junction.y) }
        for label in labels { xs.append(label.x); ys.append(label.y) }
        for flag in noConnects { xs.append(flag.x); ys.append(flag.y) }
        for text in texts { xs.append(text.x); ys.append(text.y) }
        for line in noteLines { xs += [line.x0, line.x1]; ys += [line.y0, line.y1] }
        for sheet in sheets {
            xs += [sheet.x, sheet.x + sheet.width]; ys += [sheet.y, sheet.y + sheet.height]
        }
        for bus in buses { xs += [bus.x0, bus.x1]; ys += [bus.y0, bus.y1] }
        // Fit the drawn content with a margin, never zooming tighter than 160 x 120 mm.
        guard let minX = xs.min(), let maxX = xs.max(), let minY = ys.min(), let maxY = ys.max() else {
            return CGRect(x: 0, y: 0, width: 160, height: 120)
        }
        let margin = 12.7
        let width = max(maxX - minX + 2 * margin, 160), height = max(maxY - minY + 2 * margin, 120)
        return CGRect(x: (minX + maxX - width) / 2, y: (minY + maxY - height) / 2, width: width, height: height)
    }

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                Canvas { context, size in draw(context: &context, size: size) }
                #if DEBUG
                // For UI tests (NativeKiCadTests): symbols drawn as placeholders (no definition).
                Color.clear.frame(width: 1, height: 1)
                    .allowsHitTesting(false)
                    .accessibilityElement()
                    .accessibilityIdentifier("placeholderProbe")
                    .accessibilityLabel("\(symbols.indices.filter { Self.twoPinEnds(symbols[$0]) == nil && drawing($0) == nil }.count)")
                // Sheet millimetres to window points: window = origin + offset + (mm - min) * scale.
                let scale = min(geometry.size.width / bounds.width, geometry.size.height / bounds.height)
                let origin = geometry.frame(in: .global).origin
                Color.clear.frame(width: 1, height: 1)
                    .allowsHitTesting(false)
                    .accessibilityElement()
                    .accessibilityIdentifier("viewportProbe")
                    .accessibilityLabel("\(bounds.minX) \(bounds.minY) \(scale) "
                        + "\(origin.x + (geometry.size.width - bounds.width * scale) / 2) "
                        + "\(origin.y + (geometry.size.height - bounds.height * scale) / 2)")
                #endif
                TouchCanvasSurface(
                    onTap: { tap($0, size: geometry.size) },
                    onHold: { hold($0, size: geometry.size) },
                    onPreview: { finger = $0 },
                    onHoldDrag: { radialHover = OrbitMenu.item(at: $0, center: radialLocation, count: menuItems.count) },
                    onHoldEnd: { _ in
                        if let radialHover, menuItems.indices.contains(radialHover) { menuItems[radialHover].perform() }
                        radialHover = nil
                    },
                    onZoom: { viewport = Viewport.zoom(bounds, by: $0, about: millimetres($1, size: geometry.size)) },
                    onPan: { viewport = Viewport.pan(bounds, by: $0, in: geometry.size) }
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                if let target = radialTarget {
                    OrbitMenu(title: title(for: target), items: menuItems, highlighted: radialHover)
                        .position(radialLocation)
                        .transition(.opacity)
                }
            }
            .onChange(of: geometry.size, initial: true) { _, size in canvasSize = size }
        }
        .onChange(of: document, initial: true) { viewport = fittedBounds }
        .padding(Self.chrome)
        .background(Corv.void)
        .overlay(alignment: .leading) {
            ToolRail(items: kicadTools ? Self.kicadToolItems : Self.toolItems, selection: $tool, disabled: !editable)
                .padding(.leading, 18)
        }
        .overlay(alignment: .top) { contextChips.padding(.top, 16) }
        .overlay(alignment: .topTrailing) { stats.padding(.top, 16).padding(.trailing, 18) }
        .overlay(alignment: .topLeading) { hierarchyBar.padding(.top, 16).padding(.leading, 18) }
        .overlay(alignment: .bottom) {
            let current = finger.map { snap($0, size: canvasSize, magnets: tool != .part && moveTarget == nil) }
            HUDBar(status: status, coordinate: current?.point,
                   snap: placing ? (collides ? "Occupied" : current?.reason) : nil,
                   snapColor: collides ? Corv.red : current?.magnetic == true ? Corv.cyan : Corv.textDim)
                .padding(.bottom, 18)
        }
        .animation(Corv.spring, value: tool)
        .animation(Corv.spring, value: wireStart)
        .animation(Corv.spring, value: moveTarget)
        .animation(.easeOut(duration: 0.16), value: radialTarget)
        .onChange(of: tool) { wireStart = nil; sheetStart = nil; highlightedNet = nil }
        .onChange(of: editable) { _, canEdit in
            if !canEdit { tool = .select; wireStart = nil; sheetStart = nil; moveTarget = nil; radialTarget = nil }
        }
        .alert("New sheet", isPresented: Binding(
            get: { sheetBox != nil },
            set: { if !$0 { sheetBox = nil } }
        )) {
            TextField("Name", text: $sheetName)
            Button("Create") {
                if let box = sheetBox, !sheetName.trimmingCharacters(in: .whitespaces).isEmpty {
                    onAddSheet(sheetName.trimmingCharacters(in: .whitespaces), box)
                }
                sheetBox = nil
            }
            Button("Cancel", role: .cancel) { sheetBox = nil }
        } message: { Text("KiCad creates the sheet's file, <name>.kicad_sch, in the project folder.") }
        .onChange(of: itemCount) { selected = nil; highlightedNet = nil; offerFootprint() }
        .onChange(of: document) { highlightedNet = nil }
        .alert("Rename", isPresented: Binding(
            get: { renameTarget != nil },
            set: { if !$0 { renameTarget = nil } }
        )) {
            TextField("Name", text: $renameText)
            Button("Save") { finishRename() }
            Button("Cancel", role: .cancel) { renameTarget = nil }
        } message: { Text("The new name is saved in the KiCad project file.") }
        .sheet(isPresented: Binding(get: { fieldsTarget != nil }, set: { if !$0 { fieldsTarget = nil } })) {
            fieldsSheet
        }
    }

    private var fieldsSheet: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("Edit symbol", color: Corv.gold)
                    Text(fieldsTarget.map(title(for:)) ?? "").font(.corvDisplay(28)).foregroundStyle(Corv.text)
                }
                Spacer()
                Button("Cancel") { fieldsTarget = nil }.buttonStyle(.corvGhost)
            }
            VStack(alignment: .leading, spacing: 6) {
                MonoLabel("Value", color: Corv.textFaint, small: true)
                TextField("10k", text: $valueText).autocorrectionDisabled().corvField()
            }
            VStack(alignment: .leading, spacing: 6) {
                MonoLabel("Footprint", color: Corv.textFaint, small: true)
                HStack(spacing: 8) {
                    TextField("Library:Footprint", text: $footprintText)
                        .textInputAutocapitalization(.never).autocorrectionDisabled().corvField()
                    if footprints != nil {
                        Button("Choose") { choosingFootprint = true }
                            .buttonStyle(.corvGhost)
                            .accessibilityIdentifier("chooseFootprint")
                    }
                }
            }
            Button {
                if let target = fieldsTarget { onSetFields(uuid(for: target), valueText, footprintText) }
                fieldsTarget = nil
            } label: { Text("Save").frame(maxWidth: .infinity) }
                .buttonStyle(.corvPrimary)
                .accessibilityIdentifier("saveFields")
        }
        .padding(28)
        .frame(maxWidth: 480)
        .sheet(isPresented: $choosingFootprint) {
            if let footprints, case let .symbol(index)? = fieldsTarget, symbols.indices.contains(index) {
                let id = schematicText(symbols[index].library_id)
                let part = parts?.part(id)
                FootprintPicker(library: footprints, title: "\(schematicText(symbols[index].reference)) · \(id)",
                                filters: part?.filters ?? [], pins: part?.pins ?? 0) { footprint in
                    // Saved at once with the value, as one undoable edit.
                    footprintText = footprint
                    onSetFields(schematicText(symbols[index].uuid), valueText, footprint)
                    fieldsTarget = nil
                }
                    .presentationDetents([.large])
            }
        }
        .presentationDetents([.height(360)])
        .presentationBackground(Corv.space)
        .presentationCornerRadius(28)
    }

    // MARK: Chrome

    private var itemCount: Int { symbols.count + wires.count + labels.count }

    private var placing: Bool { editable && (tool != .select || moveTarget != nil) }

    private var collides: Bool {
        guard let finger, tool == .part || isMovingSymbol else { return false }
        let point = snap(finger, size: canvasSize, magnets: false).point
        return symbols.contains {
            hypot($0.x - Double(point.x), $0.y - Double(point.y)) < 10.16 &&
            (moveTarget == nil || schematicText($0.uuid) != uuid(for: moveTarget!))
        }
    }

    private var status: String {
        if !editable { return "Imported schematic · read-only. Open a local project to edit." }
        if let moveTarget { return "Tap the new location for \(title(for: moveTarget))." }
        if let resizeSheet, sheets.indices.contains(resizeSheet) {
            return "Tap the new bottom-right corner of \(schematicText(sheets[resizeSheet].name))."
        }
        if wireStart != nil {
            return tool == .bus ? "Tap where the bus segment ends"
                : tool == .note ? "Tap where the line ends" : "Tap the destination · the route avoids pins and parts"
        }
        if sheetStart != nil { return "Tap the opposite corner of the sheet" }
        switch tool {
        case .select: return "Tap to select · hold an item or sheet for actions"
        case .part: return "Tap a free grid point to place \(chosenPart ?? "a two-pin part")"
        case .wire: return "Tap a pin or endpoint to start a wire"
        case .label: return "Tap a pin or wire endpoint to label it \(labelName)"
        case .bus: return "Tap where the bus starts"
        case .entry: return "Tap beside a bus · the entry leans toward your side"
        case .sheet: return "Tap the first corner of a new sheet"
        case .noConnect: return "Tap an unused pin to mark it not connected"
        case .text: return "Tap where the note goes: \(noteText)"
        case .note: return "Tap where the line starts"
        }
    }

    @ViewBuilder private var contextChips: some View {
        if resizeSheet != nil {
            ChipBar {
                MonoLabel("Resizing sheet", color: Corv.textFaint, small: true)
                Button("Cancel") { resizeSheet = nil }.buttonStyle(.corvGhost)
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if let moveTarget {
            ChipBar {
                MonoLabel("Moving", color: Corv.textFaint, small: true)
                Text(title(for: moveTarget)).font(.corvReadout).foregroundStyle(Corv.gold)
                Button("Cancel") { self.moveTarget = nil }.buttonStyle(.corvGhost)
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if wireStart != nil || sheetStart != nil {
            ChipBar {
                Circle().fill(Corv.gold).frame(width: 8, height: 8).shadow(color: Corv.gold, radius: 5)
                MonoLabel(sheetStart != nil ? "Drawing sheet" : tool == .bus ? "Drawing bus" : tool == .note ? "Drawing line" : "Routing wire",
                          color: Corv.text, small: true)
                Button("Cancel") { wireStart = nil; sheetStart = nil }.buttonStyle(.corvGhost)
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .part && editable, let onChoosePart {
            ChipBar {
                MonoLabel("Part", color: Corv.textFaint, small: true)
                Text(chosenPart ?? "Two-pin").font(.corvReadout).foregroundStyle(Corv.gold).lineLimit(1)
                Button("Choose", action: onChoosePart).buttonStyle(.corvGhost)
                    .accessibilityIdentifier("choosePart")
                if kicadTools, let onChoosePower {
                    // KiCad's power library: the usual rails one tap away.
                    Menu {
                        ForEach(["GND", "+3V3", "+5V", "+12V", "VCC", "GNDA", "PWR_FLAG"], id: \.self) { name in
                            Button(name) { onChoosePower("power:" + name) }
                        }
                    } label: { Label("Power", systemImage: "bolt") }
                        .buttonStyle(.corvGhost)
                        .accessibilityIdentifier("choosePower")
                }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .label && editable {
            ChipBar {
                MonoLabel("Net", color: Corv.textFaint, small: true)
                TextField("NET1", text: $labelName).frame(width: 140).corvField()
                    .textInputAutocapitalization(.characters).autocorrectionDisabled()
                if kicadTools {
                    Picker("Scope", selection: $labelKind) {
                        Text("Local").tag(Int32(0))
                        Text("Global").tag(Int32(1))
                        Text("Hier.").tag(Int32(2))
                    }
                    .pickerStyle(.segmented).frame(width: 200)
                    .accessibilityIdentifier("labelScope")
                }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .entry && editable && kicadTools {
            ChipBar {
                MonoLabel("Entry", color: Corv.textFaint, small: true)
                Picker("Entry", selection: $busToBus) {
                    Text("Wire to bus").tag(false)
                    Text("Bus to bus").tag(true)
                }
                .pickerStyle(.segmented).frame(width: 240)
                .accessibilityIdentifier("entryKind")
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .text && editable {
            ChipBar {
                MonoLabel("Text", color: Corv.textFaint, small: true)
                TextField("Note", text: $noteText).frame(width: 220).corvField()
                    .accessibilityIdentifier("noteText")
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if let net = highlighted {
            ChipBar {
                MonoLabel("Net", color: Corv.textFaint, small: true)
                Text(net.name.isEmpty ? "Unnamed" : net.name).font(.corvReadout).foregroundStyle(Corv.cyan)
                MonoLabel("\(net.pins.count) pins", small: true)
                Button("Clear") { highlightedNet = nil }.buttonStyle(.corvGhost)
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    @ViewBuilder private var hierarchyBar: some View {
        VStack(alignment: .leading, spacing: 10) {
            if trail.count > 1 || !sheets.isEmpty {
                HStack(spacing: 6) {
                    Image(systemName: "square.stack.3d.down.right").foregroundStyle(Corv.gold)
                    ForEach(Array(trail.enumerated()), id: \.element.path) { index, crumb in
                        HStack(spacing: 6) {
                            if index > 0 {
                                Image(systemName: "chevron.right").font(.caption2).foregroundStyle(Corv.textFaint)
                            }
                            Button(crumb.name) { onCrumb(index) }
                                .buttonStyle(.plain)
                                .font(.system(size: 14, weight: index == trail.count - 1 ? .semibold : .regular))
                                .foregroundStyle(index == trail.count - 1 ? Corv.text : Corv.textDim)
                                .disabled(index == trail.count - 1)
                        }
                    }
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .corvPanel(radius: 14)
            }
            if let readOnlyReason {
                HStack(spacing: 8) {
                    Image(systemName: "lock").foregroundStyle(Corv.gold)
                    Text("Read-only: \(readOnlyReason)").font(.caption).foregroundStyle(Corv.textDim)
                        .lineLimit(2).frame(maxWidth: 320, alignment: .leading)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .corvPanel(radius: 14)
            }
        }
    }

    private var stats: some View {
        HStack(spacing: 14) {
            stat(symbols.count, "Symbols")
            stat(wires.count, "Wires")
            stat(labels.count, "Labels")
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .corvPanel(radius: 14)
    }

    private func stat(_ count: Int, _ title: String) -> some View {
        HStack(spacing: 6) {
            Text("\(count)").font(.corvReadout).foregroundStyle(Corv.cyan)
            MonoLabel(title, small: true)
        }
    }

    private var menuItems: [OrbitMenu.Item] {
        guard let target = radialTarget else { return [] }
        var items: [OrbitMenu.Item] = []
        if case .sheet = target {
            // KiCad keeps a sheet's box tied to its file; only opening it and its pins are offered.
            items.append(.init(id: "Open", icon: "arrow.down.right.square") {
                radialTarget = nil
                onEnterSheet(uuid(for: target))
            })
            if kicadTools {
                items.append(.init(id: "Pins", icon: "pin") {
                    radialTarget = nil
                    onAddSheetPins(uuid(for: target))
                })
                if case let .sheet(index) = target {
                    items.append(.init(id: "Resize", icon: "arrow.up.left.and.arrow.down.right") {
                        radialTarget = nil; tool = .select; resizeSheet = index
                    })
                }
                items.append(.init(id: "Delete", icon: "trash", destructive: true) {
                    radialTarget = nil; selected = nil
                    onDelete(uuid(for: target))
                })
            }
            return items
        }
        if movable(target) {
            items.append(.init(id: "Move", icon: "arrow.up.and.down.and.arrow.left.and.right") {
                moveTarget = target; radialTarget = nil; tool = .select
            })
            items.append(.init(id: "Rename", icon: "character.cursor.ibeam") { beginRename(target) })
        }
        if case let .symbol(index) = target {
            items.append(.init(id: "Rotate", icon: "rotate.left") {
                radialTarget = nil
                onRotateSymbol(uuid(for: target))
            })
            if kicadTools {
                items.append(.init(id: "Mirror", icon: "arrow.left.and.right.righttriangle.left.righttriangle.right") {
                    radialTarget = nil
                    onMirrorSymbol(uuid(for: target), 1)
                })
                items.append(.init(id: "Flip", icon: "arrow.up.and.down.righttriangle.up.righttriangle.down") {
                    radialTarget = nil
                    onMirrorSymbol(uuid(for: target), 0)
                })
            }
            items.append(.init(id: "Edit", icon: "slider.horizontal.3") {
                valueText = schematicText(symbols[index].value)
                footprintText = schematicText(symbols[index].footprint)
                fieldsTarget = target
                radialTarget = nil
            })
        }
        items.append(.init(id: "Delete", icon: "trash", destructive: true) {
            radialTarget = nil; selected = nil
            onDelete(uuid(for: target))
        })
        return items
    }

    // MARK: Geometry

    private func screen(_ point: CGPoint, size: CGSize) -> CGPoint {
        let scale = min(size.width / bounds.width, size.height / bounds.height)
        return CGPoint(x: (size.width - bounds.width * scale) / 2 + (point.x - bounds.minX) * scale,
                       y: (size.height - bounds.height * scale) / 2 + (point.y - bounds.minY) * scale)
    }

    private func millimetres(_ location: CGPoint, size: CGSize) -> CGPoint {
        let scale = min(size.width / bounds.width, size.height / bounds.height)
        guard scale > 0 else { return .zero }
        return CGPoint(x: (location.x - (size.width - bounds.width * scale) / 2) / scale + bounds.minX,
                       y: (location.y - (size.height - bounds.height * scale) / 2) / scale + bounds.minY)
    }

    private func snap(_ location: CGPoint, size: CGSize, magnets: Bool = true) -> Snap {
        let raw = millimetres(location, size: size)
        let grid = CGPoint(x: (raw.x / 2.54).rounded() * 2.54,
                           y: (raw.y / 2.54).rounded() * 2.54)
        guard magnets else { return Snap(point: grid, reason: "Grid", magnetic: false) }
        var candidates: [(CGPoint, String)] = []
        for (index, symbol) in symbols.enumerated() {
            if let ends = Self.twoPinEnds(symbol) {
                candidates.append((ends.0, "Pin"))
                candidates.append((ends.1, "Pin"))
            } else if let drawing = drawing(index) {
                for pin in drawing.pins where pin.visible != 0 {
                    let name = schematicText(pin.name)
                    candidates.append((CGPoint(x: pin.x, y: pin.y),
                                       "Pin \(schematicText(pin.number))" + (name.isEmpty || name == "~" ? "" : " \(name)")))
                }
            }
        }
        for wire in wires {
            candidates.append((CGPoint(x: wire.x0, y: wire.y0), "Wire endpoint"))
            candidates.append((CGPoint(x: wire.x1, y: wire.y1), "Wire endpoint"))
            // The grid point along a straight wire's interior; the engine writes the junction.
            if abs(wire.y0 - wire.y1) < 0.001, grid.x > min(wire.x0, wire.x1), grid.x < max(wire.x0, wire.x1) {
                candidates.append((CGPoint(x: grid.x, y: wire.y0), "Wire"))
            } else if abs(wire.x0 - wire.x1) < 0.001, grid.y > min(wire.y0, wire.y1), grid.y < max(wire.y0, wire.y1) {
                candidates.append((CGPoint(x: wire.x0, y: grid.y), "Wire"))
            }
        }
        for bus in buses {
            candidates.append((CGPoint(x: bus.x0, y: bus.y0), "Bus endpoint"))
            candidates.append((CGPoint(x: bus.x1, y: bus.y1), "Bus endpoint"))
        }
        for entry in busEntries {
            candidates.append((CGPoint(x: entry.x0, y: entry.y0), "Bus entry"))
            candidates.append((CGPoint(x: entry.x1, y: entry.y1), "Bus entry"))
        }
        for pin in sheetPins.joined() {
            candidates.append((CGPoint(x: pin.x, y: pin.y), "Sheet pin \(schematicText(pin.text))"))
        }
        for junction in junctions { candidates.append((CGPoint(x: junction.x, y: junction.y), "Junction")) }
        for label in labels {
            candidates.append((CGPoint(x: label.x, y: label.y), Self.labelKindName(label)))
        }
        for flag in noConnects { candidates.append((CGPoint(x: flag.x, y: flag.y), "No connect")) }
        if let nearest = candidates.min(by: {
            distance(screen($0.0, size: size), location) < distance(screen($1.0, size: size), location)
        }), distance(screen(nearest.0, size: size), location) <= 22 {
            return Snap(point: nearest.0, reason: nearest.1, magnetic: true)
        }
        return Snap(point: grid, reason: "Grid", magnetic: false)
    }

    private static func labelKindName(_ label: cc_sch_label) -> String {
        switch schematicText(label.kind) {
        case "global_label": return "Global label"
        case "hierarchical_label": return "Hierarchical label"
        default: return "Label"
        }
    }

    private func distance(_ a: CGPoint, _ b: CGPoint) -> CGFloat {
        hypot(a.x - b.x, a.y - b.y)
    }

    // MARK: Drawing

    private func draw(context: inout GraphicsContext, size: CGSize) {
        let scale = min(size.width / bounds.width, size.height / bounds.height)
        guard scale > 0 else { return }
        var minor = Path(), major = Path()
        // 2.54 mm grid aligned to the sheet origin across the whole canvas, since parts can be
        // placed anywhere on it; every fifth line is major.
        let pitch = 2.54
        let topLeft = millimetres(.zero, size: size)
        let bottomRight = millimetres(CGPoint(x: size.width, y: size.height), size: size)
        for step in Int((topLeft.x / pitch).rounded(.up))...Int((bottomRight.x / pitch).rounded(.down)) {
            let x = Double(step) * pitch
            let a = screen(CGPoint(x: x, y: topLeft.y), size: size)
            let b = screen(CGPoint(x: x, y: bottomRight.y), size: size)
            if step % 5 == 0 { major.move(to: a); major.addLine(to: b) } else { minor.move(to: a); minor.addLine(to: b) }
        }
        for step in Int((topLeft.y / pitch).rounded(.up))...Int((bottomRight.y / pitch).rounded(.down)) {
            let y = Double(step) * pitch
            let a = screen(CGPoint(x: topLeft.x, y: y), size: size)
            let b = screen(CGPoint(x: bottomRight.x, y: y), size: size)
            if step % 5 == 0 { major.move(to: a); major.addLine(to: b) } else { minor.move(to: a); minor.addLine(to: b) }
        }
        context.stroke(minor, with: .color(.white.opacity(0.035)), lineWidth: 0.5)
        context.stroke(major, with: .color(.white.opacity(0.08)), lineWidth: 0.7)
        drawPage(context: &context, size: size, scale: scale)

        for sheet in sheets {
            let a = screen(CGPoint(x: sheet.x, y: sheet.y), size: size)
            let b = screen(CGPoint(x: sheet.x + sheet.width, y: sheet.y + sheet.height), size: size)
            let box = Path(roundedRect: CGRect(x: a.x, y: a.y, width: b.x - a.x, height: b.y - a.y), cornerRadius: 4)
            context.fill(box, with: .color(Corv.surface.opacity(0.7)))
            context.stroke(box, with: .color(Corv.gold.opacity(0.75)), lineWidth: 1.5)
            context.draw(Text(schematicText(sheet.name)).font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(Corv.gold),
                         at: CGPoint(x: a.x, y: a.y - 4), anchor: .bottomLeading)
            context.draw(Text(schematicText(sheet.file)).font(.corvMonoSmall).foregroundStyle(Corv.textFaint),
                         at: CGPoint(x: a.x + 6, y: b.y - 6), anchor: .bottomLeading)
            context.draw(Text(Image(systemName: "arrow.down.right.square")).font(.system(size: 13))
                            .foregroundStyle(Corv.gold.opacity(0.8)),
                         at: CGPoint(x: b.x - 6, y: a.y + 6), anchor: .topTrailing)
        }
        // Sheet pins: a dot on the box edge and the name inside, toward the box's centre.
        for (sheet, pins) in zip(sheets, sheetPins) {
            for pin in pins {
                let point = screen(CGPoint(x: pin.x, y: pin.y), size: size)
                let inward = pin.x <= sheet.x + sheet.width / 2
                context.dot(point, radius: 3, color: Corv.gold)
                context.draw(Text(schematicText(pin.text)).font(.corvMonoSmall).foregroundStyle(Corv.gold),
                             at: CGPoint(x: point.x + (inward ? 6 : -6), y: point.y),
                             anchor: inward ? .leading : .trailing)
            }
        }
        // Buses thicker than wires, as KiCad draws them; entries as wires.
        for bus in buses {
            var line = Path()
            line.move(to: screen(CGPoint(x: bus.x0, y: bus.y0), size: size))
            line.addLine(to: screen(CGPoint(x: bus.x1, y: bus.y1), size: size))
            context.stroke(line, with: .color(Corv.cyan.opacity(0.75)), style: StrokeStyle(lineWidth: 4.5, lineCap: .round))
        }
        for entry in busEntries {
            var line = Path()
            line.move(to: screen(CGPoint(x: entry.x0, y: entry.y0), size: size))
            line.addLine(to: screen(CGPoint(x: entry.x1, y: entry.y1), size: size))
            context.stroke(line, with: .color(Corv.cyan.opacity(0.9)), style: StrokeStyle(lineWidth: 2, lineCap: .round))
        }
        for (index, wire) in wires.enumerated() {
            var line = Path()
            line.move(to: screen(CGPoint(x: wire.x0, y: wire.y0), size: size))
            line.addLine(to: screen(CGPoint(x: wire.x1, y: wire.y1), size: size))
            let isSelected = selected == .wire(index)
            let inNet = highlighted?.wires.contains(index) ?? false
            if isSelected || inNet {
                var glow = context
                glow.addFilter(.blur(radius: 5))
                glow.stroke(line, with: .color((isSelected ? Corv.gold : Corv.cyan).opacity(0.8)), lineWidth: 6)
            }
            let dimmed = highlighted != nil && !inNet
            context.stroke(line, with: .color(isSelected ? Corv.gold : Corv.cyan.opacity(dimmed ? 0.3 : 0.9)),
                           style: StrokeStyle(lineWidth: inNet ? 3 : 2, lineCap: .round))
        }
        for junction in junctions {
            context.dot(screen(CGPoint(x: junction.x, y: junction.y), size: size), radius: 4, color: Corv.cyan)
        }
        for (index, symbol) in symbols.enumerated() {
            let center = screen(CGPoint(x: symbol.x, y: symbol.y), size: size)
            let isSelected = selected == .symbol(index)
            let stroke = isSelected ? Corv.gold : Color.white.opacity(0.9)
            if let ends = Self.twoPinEnds(symbol) {
                let first = screen(ends.0, size: size), second = screen(ends.1, size: size)
                let horizontal = abs(first.y - second.y) < 1
                if isSelected { context.halo(at: center, radius: max(22, 4 * scale)) }
                // Drawn along +x about the centre, then turned onto the pin axis.
                var body = context
                body.translateBy(x: center.x, y: center.y)
                body.rotate(by: .radians(atan2(second.y - first.y, second.x - first.x)))
                let rect = CGRect(x: -2.54 * scale, y: -1.27 * scale, width: 5.08 * scale, height: 2.54 * scale)
                var pins = Path()
                pins.move(to: CGPoint(x: -5.08 * scale, y: 0)); pins.addLine(to: CGPoint(x: rect.minX, y: 0))
                pins.move(to: CGPoint(x: rect.maxX, y: 0)); pins.addLine(to: CGPoint(x: 5.08 * scale, y: 0))
                body.stroke(pins, with: .color(stroke), lineWidth: 1.5)
                body.fill(Path(roundedRect: rect, cornerRadius: 2), with: .color(Corv.surface))
                body.stroke(Path(roundedRect: rect, cornerRadius: 2), with: .color(stroke), lineWidth: 1.5)
                context.dot(first, radius: 2.5, color: Corv.goldSoft)
                context.dot(second, radius: 2.5, color: Corv.goldSoft)
                let offset = horizontal ? CGSize(width: 0, height: 3.8 * scale) : CGSize(width: 4.4 * scale, height: 0)
                context.draw(Text(schematicText(symbol.reference)).font(.system(size: 12, weight: .semibold))
                                .foregroundStyle(isSelected ? Corv.gold : Corv.text),
                             at: CGPoint(x: center.x - offset.width, y: center.y - offset.height))
                context.draw(Text(schematicText(symbol.value)).font(.corvMonoSmall)
                                .foregroundStyle(Corv.textDim),
                             at: CGPoint(x: center.x + offset.width, y: center.y + offset.height))
            } else if let drawing = drawing(index), let box = drawing.box {
                drawLibrarySymbol(drawing, box: box, symbol: symbol, selected: isSelected, scale: scale,
                                  context: &context, size: size)
            } else {
                let rect = CGRect(x: center.x - 28, y: center.y - 16, width: 56, height: 32)
                if isSelected { context.halo(at: center, radius: 36) }
                context.fill(Path(roundedRect: rect, cornerRadius: 6), with: .color(Corv.surface))
                context.stroke(Path(roundedRect: rect, cornerRadius: 6), with: .color(stroke.opacity(0.7)),
                               style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
                context.draw(Text(schematicText(symbol.reference)).font(.corvMono)
                                .foregroundStyle(isSelected ? Corv.gold : Corv.text), at: center)
            }
        }
        for pin in highlighted?.pins ?? [] {
            let point = screen(pin, size: size)
            context.halo(at: point, radius: 9)
            context.dot(point, radius: 4, color: Corv.cyan)
        }
        for flag in noConnects {
            let point = screen(CGPoint(x: flag.x, y: flag.y), size: size)
            let arm = max(5, 0.635 * scale) // KiCad's no-connect cross is 1.27 mm across
            var cross = Path()
            cross.move(to: CGPoint(x: point.x - arm, y: point.y - arm))
            cross.addLine(to: CGPoint(x: point.x + arm, y: point.y + arm))
            cross.move(to: CGPoint(x: point.x - arm, y: point.y + arm))
            cross.addLine(to: CGPoint(x: point.x + arm, y: point.y - arm))
            context.stroke(cross, with: .color(Corv.cyan.opacity(0.9)), style: StrokeStyle(lineWidth: 1.5, lineCap: .round))
        }
        // Notes: graphic lines dashed (KiCad's notes style) and free text, neither on a net.
        for (index, line) in noteLines.enumerated() {
            var path = Path()
            path.move(to: screen(CGPoint(x: line.x0, y: line.y0), size: size))
            path.addLine(to: screen(CGPoint(x: line.x1, y: line.y1), size: size))
            context.stroke(path, with: .color(selected == .note(index) ? Corv.gold : .white.opacity(0.55)),
                           style: StrokeStyle(lineWidth: 1, lineCap: .round, dash: [5, 3]))
        }
        for (index, text) in texts.enumerated() {
            context.draw(Text(schematicText(text.text)).font(.system(size: max(9, 1.27 * scale)))
                            .foregroundStyle(selected == .text(index) ? Corv.gold : Corv.textDim),
                         at: screen(CGPoint(x: text.x, y: text.y), size: size), anchor: .bottomLeading)
        }
        for (index, label) in labels.enumerated() {
            let point = screen(CGPoint(x: label.x, y: label.y), size: size)
            let color = selected == .label(index) ? Corv.text : Corv.gold
            context.dot(point, radius: 2.5, color: color)
            let text = context.resolve(Text(schematicText(label.text)).font(.corvMono).tracking(1)
                                           .foregroundStyle(color))
            let origin = CGPoint(x: point.x + 4, y: point.y - 3)
            context.draw(text, at: origin, anchor: .bottomLeading)
            // Global labels get a capsule and hierarchical labels a box, so the scope reads at a glance.
            let kind = schematicText(label.kind)
            if kind == "global_label" || kind == "hierarchical_label" {
                let extent = text.measure(in: CGSize(width: 1000, height: 100))
                let frame = CGRect(x: origin.x - 4, y: origin.y - extent.height - 2,
                                   width: extent.width + 8, height: extent.height + 4)
                let outline = kind == "global_label" ? Path(roundedRect: frame, cornerRadius: frame.height / 2)
                                                     : Path(roundedRect: frame, cornerRadius: 2)
                context.stroke(outline, with: .color(color.opacity(0.8)), lineWidth: 1)
            }
        }
        if let wireStart {
            let start = screen(wireStart, size: size)
            context.halo(at: start, radius: 8)
            context.dot(start, radius: 4, color: Corv.gold)
            if let finger, tool == .bus || tool == .note {
                var preview = Path()
                preview.move(to: start); preview.addLine(to: screen(snap(finger, size: size).point, size: size))
                context.stroke(preview, with: .color(Corv.gold.opacity(0.9)),
                               style: StrokeStyle(lineWidth: 4, lineCap: .round, dash: [6, 5]))
            } else if let finger {
                let end = snap(finger, size: size).point
                var preview = Path()
                if let route = onRoute(wireStart, end) {
                    preview.addLines(route.map { screen($0, size: size) })
                    context.stroke(preview, with: .color(Corv.gold.opacity(0.9)),
                                   style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [6, 5]))
                } else if distance(wireStart, end) > 0.01 {
                    preview.move(to: start); preview.addLine(to: screen(end, size: size))
                    context.stroke(preview, with: .color(Corv.red.opacity(0.8)),
                                   style: StrokeStyle(lineWidth: 1.5, lineCap: .round, dash: [2, 5]))
                }
            }
        }
        if let sheetStart {
            let a = screen(sheetStart, size: size)
            let b = finger.map { screen(snap($0, size: size, magnets: false).point, size: size) } ?? a
            let box = Path(CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y)))
            context.stroke(box, with: .color(Corv.gold.opacity(0.9)), style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
        }
        if let finger, placing {
            let point = snap(finger, size: size, magnets: tool != .part && moveTarget == nil)
            let color: Color = collides ? Corv.red : point.magnetic ? Corv.cyan : .white.opacity(0.7)
            context.reticle(at: screen(point.point, size: size), color: color,
                            label: collides ? "Occupied" : point.magnetic ? point.reason : nil)
        }
        // KiCad ERC findings on this sheet, as the board marks DRC findings.
        for finding in ercMarks {
            let center = screen(finding.point, size: size)
            let radius: CGFloat = finding.selected ? 11 : 6
            if finding.selected { context.halo(at: center, radius: 22) }
            context.stroke(Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)),
                           with: .color(finding.error ? Corv.red : Corv.gold.opacity(0.75)), lineWidth: finding.selected ? 2.5 : 1.5)
        }
    }

    private var isMovingSymbol: Bool {
        if case .some(.symbol(_)) = moveTarget { return true }
        return false
    }

    // MARK: Interaction

    private func tap(_ location: CGPoint, size: CGSize) {
        if radialTarget != nil { radialTarget = nil; return }
        guard editable || tool == .select else { return }
        if let index = resizeSheet {
            resizeSheet = nil
            guard sheets.indices.contains(index) else { return }
            let corner = snap(location, size: size, magnets: false).point, sheet = sheets[index]
            onResizeSheet(schematicText(sheet.uuid), Double(corner.x) - sheet.x, Double(corner.y) - sheet.y)
            return
        }
        if let moveTarget {
            let point = snap(location, size: size, magnets: false).point
            let id = uuid(for: moveTarget)
            switch moveTarget {
            case .symbol: onMoveSymbol(id, Double(point.x), Double(point.y))
            case .label: onMoveLabel(id, Double(point.x), Double(point.y))
            default: break
            }
            self.moveTarget = nil
            return
        }
        let snapped = snap(location, size: size, magnets: tool != .part).point
        switch tool {
        case .select:
            selected = hit(location, size: size)
            highlightedNet = net(at: location, for: selected, size: size)
            if selected == nil, highlightedNet == nil, let uuid = sheetHit(location, size: size) {
                onEnterSheet(uuid)
                return
            }
            if case let .symbol(index) = selected { onSelectSymbol(index) }
            if let highlightedNet { onProbeNet?(highlightedNet) }
            else if case let .symbol(index) = selected { onProbeSymbol?(index) }
        case .part:
            // Power symbols have no footprint to choose.
            if let chosenPart, footprints != nil, !chosenPart.hasPrefix("power:") { placedAt = snapped }
            onAddTwoPin(Double(snapped.x), Double(snapped.y))
        case .label:
            if kicadTools {
                onAddScopedLabel(labelName, Double(snapped.x), Double(snapped.y), labelKind)
            } else {
                onAddLabel(labelName, Double(snapped.x), Double(snapped.y))
            }
        case .noConnect:
            onAddNoConnect(Double(snapped.x), Double(snapped.y))
        case .text:
            let at = snap(location, size: size, magnets: false).point
            if !noteText.trimmingCharacters(in: .whitespaces).isEmpty { onAddText(noteText, Double(at.x), Double(at.y)) }
        case .wire, .bus, .note:
            if let start = wireStart {
                if distance(start, snapped) > 0.01 {
                    if tool == .note {
                        onAddNoteLine(Double(start.x), Double(start.y), Double(snapped.x), Double(snapped.y))
                    } else if tool == .bus {
                        onAddBus(Double(start.x), Double(start.y), Double(snapped.x), Double(snapped.y))
                    } else {
                        onAddWire(Double(start.x), Double(start.y), Double(snapped.x), Double(snapped.y))
                    }
                    wireStart = nil
                }
            } else {
                wireStart = snapped
            }
        case .entry:
            if let entry = busEntry(at: location, size: size) {
                (kicadTools && busToBus ? onAddBusBusEntry : onAddBusEntry)(Double(entry.point.x), Double(entry.point.y), entry.quadrant)
            }
        case .sheet:
            let corner = snap(location, size: size, magnets: false).point
            if let start = sheetStart {
                let box = CGRect(x: min(start.x, corner.x), y: min(start.y, corner.y),
                                 width: abs(corner.x - start.x), height: abs(corner.y - start.y))
                sheetStart = nil
                if box.width >= 5.08 && box.height >= 5.08 {
                    sheetName = "Sheet\(sheets.count + 1)"
                    sheetBox = box
                }
            } else {
                sheetStart = corner
            }
        }
    }

    /// Where a bus entry goes for a tap beside a bus: the grid point on the nearest bus within
    /// reach, leaning toward the tap (KiCad's quadrants: 1 up-right, 2 down-right, 3 down-left,
    /// 4 up-left).
    private func busEntry(at location: CGPoint, size: CGSize) -> (point: CGPoint, quadrant: Int32)? {
        let tap = millimetres(location, size: size)
        var best: (CGPoint, CGFloat, Bool)? // point, screen distance, horizontal bus
        for bus in buses {
            let horizontal = abs(bus.y0 - bus.y1) < 0.001
            guard horizontal || abs(bus.x0 - bus.x1) < 0.001 else { continue } // straight buses only
            let along = horizontal ? tap.x : tap.y
            let low = horizontal ? min(bus.x0, bus.x1) : min(bus.y0, bus.y1)
            let high = horizontal ? max(bus.x0, bus.x1) : max(bus.y0, bus.y1)
            let snapped = min(high, max(low, (along / 2.54).rounded() * 2.54))
            let point = horizontal ? CGPoint(x: snapped, y: bus.y0) : CGPoint(x: bus.x0, y: snapped)
            let gap = distance(screen(point, size: size), location)
            if gap <= 40 && (best == nil || gap < best!.1) { best = (point, gap, horizontal) }
        }
        guard let best else { return nil }
        let point = best.0
        if best.2 { return (point, tap.y < point.y ? 1 : 2) }
        return (point, tap.x >= point.x ? 2 : 3)
    }

    private func hold(_ location: CGPoint, size: CGSize) {
        let sheet = sheetHit(location, size: size).flatMap { uuid in sheets.firstIndex { schematicText($0.uuid) == uuid } }
        guard editable, let target = hit(location, size: size) ?? sheet.map(Target.sheet) else { return }
        tool = .select
        wireStart = nil
        sheetStart = nil
        selected = target
        radialHover = nil
        radialTarget = target
        if case let .symbol(index) = target { onSelectSymbol(index) }
        radialLocation = OrbitMenu.clamped(location, in: size)
    }

    private func hit(_ location: CGPoint, size: CGSize) -> Target? {
        var best: (Target, CGFloat)?
        func consider(_ target: Target, _ distance: CGFloat, limit: CGFloat) {
            if distance <= limit && (best == nil || distance < best!.1) { best = (target, distance) }
        }
        for index in symbols.indices {
            let symbol = symbols[index]
            let center = screen(CGPoint(x: symbol.x, y: symbol.y), size: size)
            // Anywhere on a drawn library symbol's body counts as on it.
            let inside = drawing(index)?.box.map { $0.contains(millimetres(location, size: size)) } ?? false
            consider(.symbol(index), inside ? 0 : distance(center, location), limit: 30)
        }
        for index in labels.indices {
            let label = labels[index]
            consider(.label(index), distance(screen(CGPoint(x: label.x, y: label.y), size: size), location), limit: 26)
        }
        func segment(_ line: cc_sch_wire) -> CGFloat {
            let a = screen(CGPoint(x: line.x0, y: line.y0), size: size)
            let b = screen(CGPoint(x: line.x1, y: line.y1), size: size)
            let dx = b.x - a.x, dy = b.y - a.y
            let lengthSquared = dx * dx + dy * dy
            guard lengthSquared > 0 else { return .infinity }
            let t = min(1, max(0, ((location.x - a.x) * dx + (location.y - a.y) * dy) / lengthSquared))
            return distance(CGPoint(x: a.x + t * dx, y: a.y + t * dy), location)
        }
        for index in wires.indices { consider(.wire(index), segment(wires[index]), limit: 13) }
        for index in noteLines.indices { consider(.note(index), segment(noteLines[index]), limit: 13) }
        for index in noConnects.indices {
            consider(.flag(index), distance(screen(CGPoint(x: noConnects[index].x, y: noConnects[index].y), size: size), location), limit: 18)
        }
        for index in texts.indices {
            consider(.text(index), distance(screen(CGPoint(x: texts[index].x, y: texts[index].y), size: size), location), limit: 26)
        }
        return best?.0
    }

    /// Net of a tapped wire or label, or of a pin end within reach of the tap.
    private func net(at location: CGPoint, for target: Target?, size: CGSize) -> Int? {
        switch target {
        case .wire(let index): return nets.firstIndex { $0.wires.contains(index) }
        case .label(let index): return nets.firstIndex { $0.labels.contains(index) }
        default:
            return nets.indices.first { index in
                nets[index].pins.contains { distance(screen($0, size: size), location) <= 14 }
            }
        }
    }

    private var highlighted: SchematicDesign.SheetNet? {
        highlightedNet.flatMap { nets.indices.contains($0) ? nets[$0] : nil }
    }

    /// UUID of the child-sheet box under `location`, if any.
    private func sheetHit(_ location: CGPoint, size: CGSize) -> String? {
        let point = millimetres(location, size: size)
        return sheets.first {
            CGRect(x: $0.x, y: $0.y, width: $0.width, height: $0.height).contains(point)
        }.map { schematicText($0.uuid) }
    }

    /// After the Part tool placed a library part without a default footprint (LM358, R, LED…),
    /// opens its fields with the footprint chooser, as KiCad needs one for Update PCB.
    private func offerFootprint() {
        guard let point = placedAt else { return }
        placedAt = nil
        guard let index = symbols.firstIndex(where: {
            abs($0.x - point.x) < 0.01 && abs($0.y - point.y) < 0.01 && schematicText($0.footprint).isEmpty
        }) else { return }
        valueText = schematicText(symbols[index].value)
        footprintText = ""
        fieldsTarget = .symbol(index)
        Task {
            try? await Task.sleep(for: .milliseconds(450)) // after the fields sheet has appeared
            if fieldsTarget == .symbol(index) { choosingFootprint = true }
        }
    }

    private func drawing(_ index: Int) -> SchematicDesign.SymbolDrawing? {
        drawings.indices.contains(index) && drawings[index].box != nil ? drawings[index] : nil
    }

    /// A library symbol from its lib_symbols shapes: body, pin lines with names and numbers,
    /// reference and value at the sheet's field positions.
    private func drawLibrarySymbol(_ drawing: SchematicDesign.SymbolDrawing, box: CGRect, symbol: cc_sch_symbol,
                                   selected: Bool, scale: CGFloat, context: inout GraphicsContext, size: CGSize) {
        let stroke = selected ? Corv.gold : Color.white.opacity(0.9)
        let line = max(1, min(1.6, 0.18 * scale))
        let a = screen(box.origin, size: size), b = screen(CGPoint(x: box.maxX, y: box.maxY), size: size)
        if selected {
            context.halo(at: CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2), radius: max(22, max(b.x - a.x, b.y - a.y) / 2 + 8))
        }
        for shape in drawing.shapes {
            var path = Path()
            path.addLines(shape.points.map { screen($0, size: size) })
            if shape.closed { path.closeSubpath() }
            if shape.fill == 2 { context.fill(path, with: .color(Corv.surface)) }
            if shape.fill == 1 { context.fill(path, with: .color(stroke)) }
            context.stroke(path, with: .color(stroke), style: StrokeStyle(lineWidth: line, lineCap: .round, lineJoin: .round))
        }
        // KiCad's 1.27 mm text (cap height) at this zoom; pin text only where it stays legible.
        let textSize = min(16, 1.27 * scale / 0.72)
        let showText = textSize >= 6
        let text = drawing.text
        for pin in drawing.pins where pin.visible != 0 {
            let start = screen(CGPoint(x: pin.x, y: pin.y), size: size)
            let end = screen(CGPoint(x: pin.end_x, y: pin.end_y), size: size)
            var path = Path()
            path.move(to: start); path.addLine(to: end)
            context.stroke(path, with: .color(stroke), lineWidth: line)
            context.dot(start, radius: 2.5, color: Corv.goldSoft)
            guard showText else { continue }
            let length = max(distance(start, end), 0.001)
            let dx = (end.x - start.x) / length, dy = (end.y - start.y) / length
            let horizontal = abs(dx) >= abs(dy)
            let middle = CGPoint(x: (start.x + end.x) / 2, y: (start.y + end.y) / 2)
            // Over (above or left of) the pin line, and under (below or right of) it.
            let over = horizontal ? CGPoint(x: middle.x, y: middle.y - 2) : CGPoint(x: middle.x - 3, y: middle.y)
            let under = horizontal ? CGPoint(x: middle.x, y: middle.y + 2) : CGPoint(x: middle.x + 3, y: middle.y)
            let name = schematicText(pin.name)
            let showName = text.pin_names_visible != 0 && !name.isEmpty && name != "~"
            // As KiCad: with a name offset the name sits inside the body, else over the pin line
            // with the number under it.
            let outside = text.pin_name_offset <= 0
            if showName {
                let inset = text.pin_name_offset * scale
                let font = Text(name).font(.system(size: textSize)).foregroundStyle(Corv.textDim)
                if outside {
                    context.draw(font, at: over, anchor: horizontal ? .bottom : .trailing)
                } else {
                    let anchor: UnitPoint = horizontal ? (dx > 0 ? .leading : .trailing) : (dy > 0 ? .top : .bottom)
                    context.draw(font, at: CGPoint(x: end.x + dx * inset, y: end.y + dy * inset), anchor: anchor)
                }
            }
            if text.pin_numbers_visible != 0 {
                let below = outside && showName
                context.draw(Text(schematicText(pin.number)).font(.system(size: textSize - 1, design: .monospaced))
                                .foregroundStyle(Corv.textFaint),
                             at: below ? under : over,
                             anchor: horizontal ? (below ? .top : .bottom) : (below ? .leading : .trailing))
            }
        }
        // Reference and value where the sheet puts them, as KiCad does. The text grows away from the
        // body: a field right of the body box starts at its anchor, one left of it ends there, and so on.
        let body = drawing.shapes.flatMap(\.points)
        let bodyBox = body.isEmpty ? box : body.dropFirst().reduce(CGRect(origin: body[0], size: .zero)) {
            $0.union(CGRect(origin: $1, size: .zero))
        }
        let fieldSize = max(7, textSize)
        func field(_ label: Text, x: Double, y: Double, vertical: Bool) {
            let point = CGPoint(x: x, y: y)
            let anchor: UnitPoint = point.x > bodyBox.maxX ? .leading : point.x < bodyBox.minX ? .trailing
                : point.y < bodyBox.minY ? .bottom : point.y > bodyBox.maxY ? .top : .center
            let at = screen(point, size: size)
            guard vertical else { context.draw(label, at: at, anchor: anchor); return }
            var turned = context
            turned.translateBy(x: at.x, y: at.y)
            turned.rotate(by: .degrees(-90))
            turned.draw(label, at: .zero)
        }
        // Power symbols and flags (#PWR01, #FLG02): KiCad keeps their references hidden.
        let reference = schematicText(symbol.reference)
        if text.reference_visible != 0 && !reference.hasPrefix("#") {
            field(Text(reference).font(.system(size: fieldSize, weight: .semibold))
                    .foregroundStyle(selected ? Corv.gold : Corv.text),
                  x: text.reference_x, y: text.reference_y, vertical: text.reference_vertical != 0)
        }
        if text.value_visible != 0 {
            field(Text(schematicText(symbol.value)).font(.system(size: fieldSize, design: .monospaced))
                    .foregroundStyle(Corv.textDim),
                  x: text.value_x, y: text.value_y, vertical: text.value_vertical != 0)
        }
    }

    /// The page as KiCad's default drawing sheet lays it out (10 mm margins, a 2 mm zone band
    /// numbered every 50 mm, the title block bottom right), drawn in CorvCAD's own lines and labels.
    private func drawPage(context: inout GraphicsContext, size: CGSize, scale: CGFloat) {
        guard page.width > 0, page.height > 0 else { return }
        let paper = CGRect(x: 0, y: 0, width: page.width, height: page.height)
        let border = paper.insetBy(dx: 10, dy: 10), inner = border.insetBy(dx: 2, dy: 2)
        func rect(_ r: CGRect) -> Path {
            let a = screen(r.origin, size: size), b = screen(CGPoint(x: r.maxX, y: r.maxY), size: size)
            return Path(CGRect(x: a.x, y: a.y, width: b.x - a.x, height: b.y - a.y))
        }
        context.stroke(rect(paper), with: .color(.white.opacity(0.12)), lineWidth: 1)
        context.stroke(rect(border), with: .color(.white.opacity(0.32)), lineWidth: 1)
        context.stroke(rect(inner), with: .color(.white.opacity(0.18)), lineWidth: 0.7)
        let label = min(14, 1.5 * scale / 0.72) // 1.5 mm text
        // Zones: numbered columns and lettered rows, ticks every 50 mm in the 2 mm band.
        var ticks = Path()
        let columns = Int((border.width / 50).rounded(.up)), rows = Int((border.height / 50).rounded(.up))
        for column in 0..<columns {
            let x = border.minX + Double(column) * 50
            if column > 0 {
                for (y0, y1) in [(border.minY, inner.minY), (inner.maxY, border.maxY)] {
                    ticks.move(to: screen(CGPoint(x: x, y: y0), size: size)); ticks.addLine(to: screen(CGPoint(x: x, y: y1), size: size))
                }
            }
            guard label >= 5 else { continue }
            let mid = min(x + 25, (x + border.maxX) / 2)
            for y in [border.minY + 1, border.maxY - 1] {
                context.draw(Text("\(column + 1)").font(.system(size: label, design: .monospaced)).foregroundStyle(Corv.textFaint),
                             at: screen(CGPoint(x: mid, y: y), size: size))
            }
        }
        for row in 0..<rows {
            let y = border.minY + Double(row) * 50
            if row > 0 {
                for (x0, x1) in [(border.minX, inner.minX), (inner.maxX, border.maxX)] {
                    ticks.move(to: screen(CGPoint(x: x0, y: y), size: size)); ticks.addLine(to: screen(CGPoint(x: x1, y: y), size: size))
                }
            }
            guard label >= 5 else { continue }
            let mid = min(y + 25, (y + border.maxY) / 2)
            let letter = String(UnicodeScalar(UInt8(65 + row % 26)))
            for x in [border.minX + 1, border.maxX - 1] {
                context.draw(Text(letter).font(.system(size: label, design: .monospaced)).foregroundStyle(Corv.textFaint),
                             at: screen(CGPoint(x: x, y: mid), size: size))
            }
        }
        context.stroke(ticks, with: .color(.white.opacity(0.18)), lineWidth: 0.7)
        // Title block: 110 x 24 mm in three rows (title; company; date and revision).
        let block = CGRect(x: inner.maxX - 110, y: inner.maxY - 24, width: 110, height: 24)
        guard block.minX > inner.minX, block.minY > inner.minY else { return }
        context.fill(rect(block), with: .color(Corv.surface.opacity(0.6)))
        context.stroke(rect(block), with: .color(.white.opacity(0.32)), lineWidth: 1)
        var rules = Path()
        for y in [block.minY + 10, block.minY + 17] {
            rules.move(to: screen(CGPoint(x: block.minX, y: y), size: size)); rules.addLine(to: screen(CGPoint(x: block.maxX, y: y), size: size))
        }
        rules.move(to: screen(CGPoint(x: block.minX + 70, y: block.minY + 17), size: size))
        rules.addLine(to: screen(CGPoint(x: block.minX + 70, y: block.maxY), size: size))
        context.stroke(rules, with: .color(.white.opacity(0.18)), lineWidth: 0.7)
        guard label >= 5 else { return }
        func cell(_ caption: String, _ value: String, x: Double, y: Double, valueSize: CGFloat) {
            context.draw(Text(caption).font(.system(size: label * 0.8, weight: .medium, design: .monospaced)).tracking(1.2)
                            .foregroundStyle(Corv.textFaint),
                         at: screen(CGPoint(x: x, y: y + 1), size: size), anchor: .topLeading)
            context.draw(Text(value.isEmpty ? "—" : value).font(.system(size: valueSize)).foregroundStyle(Corv.text),
                         at: screen(CGPoint(x: x, y: y + 2.6), size: size), anchor: .topLeading)
        }
        cell("TITLE", schematicText(page.title), x: block.minX + 2, y: block.minY, valueSize: label * 1.6)
        cell("COMPANY", schematicText(page.company), x: block.minX + 2, y: block.minY + 10, valueSize: label)
        cell("DATE", schematicText(page.date), x: block.minX + 2, y: block.minY + 17, valueSize: label)
        cell("REV", schematicText(page.revision), x: block.minX + 72, y: block.minY + 17, valueSize: label)
    }

    /// Pin ends of CorvCAD's two-pin part (library pins at ±5.08 mm on x) at right-angle
    /// rotations, placed as KiCad does: Y flipped, then turned counter-clockwise.
    /// ponytail: mirror flags ignored; touch placement never mirrors.
    private static func twoPinEnds(_ symbol: cc_sch_symbol) -> (CGPoint, CGPoint)? {
        guard schematicText(symbol.library_id) == "CorvCAD:TwoPin" else { return nil }
        let angle = symbol.rotation * .pi / 180
        let dx = (5.08 * cos(angle) * 1000).rounded() / 1000, dy = (-5.08 * sin(angle) * 1000).rounded() / 1000
        guard dx == 0 || dy == 0 else { return nil }
        return (CGPoint(x: symbol.x - dx, y: symbol.y - dy), CGPoint(x: symbol.x + dx, y: symbol.y + dy))
    }

    /// Symbols and labels move and rename; everything else is held only to delete it.
    private func movable(_ target: Target) -> Bool {
        switch target {
        case .symbol, .label: return true
        default: return false
        }
    }

    private func title(for target: Target) -> String {
        switch target {
        case .symbol(let index): return schematicText(symbols[index].reference)
        case .wire: return "Wire"
        case .flag: return "No connect"
        case .text(let index): return schematicText(texts[index].text)
        case .note: return "Line"
        case .label(let index): return schematicText(labels[index].text)
        case .sheet(let index): return schematicText(sheets[index].name)
        }
    }

    private func uuid(for target: Target) -> String {
        switch target {
        case .symbol(let index): return schematicText(symbols[index].uuid)
        case .wire(let index): return schematicText(wires[index].uuid)
        case .flag(let index): return schematicText(noConnects[index].uuid)
        case .text(let index): return schematicText(texts[index].uuid)
        case .note(let index): return schematicText(noteLines[index].uuid)
        case .label(let index): return schematicText(labels[index].uuid)
        case .sheet(let index): return schematicText(sheets[index].uuid)
        }
    }

    private func beginRename(_ target: Target) {
        renameText = title(for: target)
        renameTarget = target
        radialTarget = nil
    }

    private func finishRename() {
        guard let target = renameTarget else { return }
        switch target {
        case .symbol: onRenameSymbol(uuid(for: target), renameText)
        case .label: onRenameLabel(uuid(for: target), renameText)
        default: break
        }
        renameTarget = nil
    }
}

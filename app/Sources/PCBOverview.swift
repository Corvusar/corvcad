import SwiftUI

/// A zone outline or one filled polygon of a zone, in board millimetres.
struct ZoneShape {
    let net: Int32
    let layer: String
    let filled: Bool
    let points: [CGPoint]
    var uuid = ""
    /// The zone's properties as the board file has them (priority, pad connection, clearances).
    var properties = cc_zone()
}

/// A drawing the Draw tool adds, in board millimetres.
enum BoardDrawing {
    /// Replaces the board outline.
    case outline([CGPoint])
    case zone([CGPoint], layer: String, net: String)
    case line(CGPoint, CGPoint, layer: String)
    case text(String, CGPoint, layer: String, size: Double)
    /// KiCad's aligned dimension (NATIVE_KICAD): measured points and the crossbar's offset,
    /// positive along (-dy, dx) as PCB_DIM_ALIGNED's height.
    case dimension(CGPoint, CGPoint, height: Double, layer: String)
}

struct PCBOverview: View {
    /// The whole board, fitted when nothing is zoomed.
    let boardBounds: CGRect
    let outline: [CGPoint]
    let tracks: [cc_track]
    let vias: [cc_via]
    let pads: [cc_pad]
    let nets: [cc_net]
    let footprints: [(Double, Double, String)]
    let editable: Bool
    @Binding var partReference: String
    @Binding var partValue: String
    let onAddTwoPad: (String, String, Double, Double) -> Void
    let onAddHole: (Double, Double) -> Void
    let onSelectFootprint: (Int) -> Void
    let onMoveFootprint: (String, Double, Double) -> Void
    let onDeleteFootprint: (String) -> Void
    let onRenameFootprint: (String, String) -> Void
    let onRotateFootprint: (String) -> Void
    /// Update PCB from schematic: pad nets and missing two-pad parts.
    let onUpdateFromSchematic: () -> Void
    /// Copper in board millimetres: track (start, end, layer, net name), via (centre, net name),
    /// and deletion of a segment or via by UUID.
    let onAddTrack: (CGPoint, CGPoint, String, String) -> Void
    let onAddVia: (CGPoint, String) -> Void
    let onDeleteCopper: (String) -> Void
    /// The Draw tool: outline, zone, silkscreen/fab line or text. Deletion goes through
    /// onDeleteCopper by UUID; a drawn item moves by an offset in mm (true for a zone) and a
    /// drawn text takes a new string and height.
    var onDraw: ((BoardDrawing) -> Void)? = nil
    var onMoveDrawing: ((String, CGSize, Bool) -> Void)? = nil
    var onEditText: ((String, String, Double) -> Void)? = nil
    /// KiCad's board edits (NATIVE_KICAD; nil otherwise): ["flip", reference] and
    /// ["track", uuid, width, layer]; it also enables the Draw tool's dimensions.
    var onBoardEdit: (([String]) -> Void)? = nil
    /// KiCad's footprint libraries for placing and swapping footprints (NATIVE_KICAD; nil otherwise).
    var footprintLibrary: FootprintLibrary? = nil
    var zones: [ZoneShape] = []
    var graphics: [cc_track] = []
    var texts: [cc_board_text] = []
    /// KiCad's ratsnest (NATIVE_KICAD edition), drawn over the copper.
    var ratsnest: [RatsnestEdge] = []
    /// KiCad DRC findings to mark on the board, and the one picked in the inspector.
    var findings: [DRCFinding] = []
    var selectedFinding: Int?
    /// KiCad's router (NATIVE_KICAD edition): the route in progress and the Track tool's
    /// actions. Nil: the Track tool places straight manual segments.
    var route: BoardRoute? = nil
    var onRoute: ((RouteAction) -> Void)? = nil
    /// 0 highlight collisions, 1 shove, 2 walk around (KiCad's router modes).
    @State private var routeMode = 2
    @State private var routeKind = RouteKind.track
    /// Tuning target (length, or skew for Skew) in millimetres.
    @State private var tuneTarget = 50.0
    @State private var layer = 0
    @State private var copperLayer = "F.Cu"
    /// The routing track's current start and net; it continues from free ends until it reaches
    /// a pad, track end or via.
    @State private var routeStart: (point: CGPoint, net: String)?
    /// The track, via or drawn item whose hold menu is open, and where it was held (board mm).
    @State private var radialCopper: HeldItem?
    @State private var heldAt = CGPoint.zero
    /// A drawn item being moved: the next tap sets its offset from the held point.
    @State private var moveDrawing: (item: HeldItem, from: CGPoint)?
    @State private var editingText: HeldItem?
    @State private var editText = ""
    @State private var editSize = ""
    @State private var selectedNet = -1
    @State private var tool = Tool.inspect
    @State private var finger: CGPoint?
    @State private var canvasSize = CGSize.zero
    @State private var selected: Int?
    @State private var radialIndex: Int?
    @State private var radialLocation = CGPoint.zero
    @State private var radialHover: Int?
    @State private var moveReference: String?
    @State private var renameReference: String?
    @State private var renameText = ""
    /// Pinch/pan viewport in board millimetres; nil fits the board.
    @State private var viewport: CGRect?
    @State private var drawKind = DrawKind.zone
    /// The library footprint the Part tool places (nil: CorvCAD's two-pad part), the picker for
    /// it, and the footprint being swapped.
    @State private var placeFootprint: String?
    @State private var choosingPlacement = false
    @State private var swapReference: String?
    /// The zone held for Properties (KiCad's zone settings).
    @State private var editingZone: ZoneShape?
    /// The track held for Edit (KiCad's width and layer).
    @State private var editingTrack: HeldItem?
    @State private var trackWidth = ""
    @State private var trackLayer = ""
    /// Corners of the outline or zone being drawn, or the start of the next line.
    @State private var corners: [CGPoint] = []
    @State private var zoneNet = ""
    @State private var drawLayer = "F.SilkS"
    @State private var drawText = "CorvCAD"
    @State private var textSize = 1.0

    private var bounds: CGRect { viewport ?? boardBounds }

    private enum Tool: Hashable { case inspect, twoPad, mountingHole, track, via, draw }
    private enum DrawKind: String, CaseIterable { case zone = "Zone", outline = "Outline", line = "Line", text = "Text", dimension = "Dim" }
    private var drawKinds: [DrawKind] { DrawKind.allCases.filter { $0 != .dimension || onBoardEdit != nil } }
    private static let drawLayers = ["F.SilkS", "B.SilkS", "F.Fab", "B.Fab"]
    private static let toolItems: [ToolRail<Tool>.Item] = [
        .init(tool: .inspect, title: "Select", icon: "hand.point.up.left"),
        .init(tool: .twoPad, title: "Part", icon: "cpu"),
        .init(tool: .mountingHole, title: "M3", icon: "circle.circle"),
        .init(tool: .track, title: "Track", icon: "point.topleft.down.to.point.bottomright.curvepath"),
        .init(tool: .via, title: "Via", icon: "smallcircle.filled.circle"),
        .init(tool: .draw, title: "Draw", icon: "pencil.and.outline"),
    ]
    private struct HeldItem {
        enum Kind { case copper, track(width: Double, layer: String), line, zone, text(String, Double) }
        let uuid: String
        let title: String
        let kind: Kind
        var drawn: Bool {
            switch kind {
            case .copper, .track: return false
            default: return true
            }
        }
        var isZone: Bool { if case .zone = kind { return true } else { return false } }
    }
    /// A copper point a track or via can attach to, with its net name.
    private struct Anchor { let point: CGPoint; let net: String; let reason: String }
    /// Canvas inset so the board clears the floating chrome.
    private static let chrome = EdgeInsets(top: 84, leading: 100, bottom: 84, trailing: 36)

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                Canvas { context, size in draw(&context, size: size) }
                TouchCanvasSurface(
                    onTap: { place(at: $0, in: geometry.size) },
                    onHold: { hold(at: $0, in: geometry.size) },
                    onPreview: { location in
                        finger = location
                        if let location, route != nil, tool == .track {
                            onRoute?(.move(routerPoint(location, in: geometry.size)))
                        }
                    },
                    onHoldDrag: { radialHover = OrbitMenu.item(at: $0, center: radialLocation, count: menuItems.count) },
                    onHoldEnd: { _ in
                        if let radialHover, menuItems.indices.contains(radialHover) { menuItems[radialHover].perform() }
                        radialHover = nil
                    },
                    onZoom: { factor, anchor in
                        let point = raw(anchor, in: geometry.size)
                        viewport = Viewport.zoom(bounds, by: factor, about: CGPoint(x: point.x, y: point.y))
                    },
                    onPan: { viewport = Viewport.pan(bounds, by: $0, in: geometry.size) }
                )
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                #if DEBUG
                testProbe(geometry)
                #endif
                if let index = radialIndex, footprints.indices.contains(index) {
                    OrbitMenu(title: footprints[index].2, items: menuItems, highlighted: radialHover)
                        .position(radialLocation)
                        .transition(.opacity)
                } else if let copper = radialCopper {
                    OrbitMenu(title: copper.title, items: menuItems, highlighted: radialHover)
                        .position(radialLocation)
                        .transition(.opacity)
                }
            }
            .onChange(of: geometry.size, initial: true) { _, size in canvasSize = size }
        }
        .onChange(of: boardBounds) { viewport = nil }
        .padding(Self.chrome)
        .background(Corv.void)
        .overlay(alignment: .leading) {
            ToolRail(items: Self.toolItems, selection: $tool, disabled: !editable).padding(.leading, 18)
        }
        .overlay(alignment: .top) { if !chipsBelow { contextChips.padding(.top, 16) } }
        .overlay(alignment: .topTrailing) { layerPanel.padding(.top, 16).padding(.trailing, 18) }
        .overlay(alignment: .bottom) {
            VStack(spacing: 10) {
                if chipsBelow { contextChips }
                HUDBar(status: status, coordinate: fingerMillimetres,
                       snap: placing ? "0.5 mm grid" : nil)
            }
            .padding(.bottom, 18)
        }
        .animation(Corv.spring, value: tool)
        .animation(Corv.spring, value: moveReference)
        .animation(.easeOut(duration: 0.16), value: radialIndex)
        .onChange(of: editable) { _, canEdit in
            if !canEdit {
                tool = .inspect; moveReference = nil; moveDrawing = nil; editingText = nil
                radialIndex = nil; radialCopper = nil; routeStart = nil; corners = []
            }
        }
        .onChange(of: drawKind) { corners = [] }
        .onChange(of: tool) {
            routeStart = nil
            corners = []
            if route != nil { onRoute?(.cancel) }
        }
        .onChange(of: footprints.count) { selected = nil }
        .alert("Rename reference", isPresented: Binding(
            get: { renameReference != nil },
            set: { if !$0 { renameReference = nil } }
        )) {
            TextField("Reference", text: $renameText)
            Button("Save") {
                if let renameReference { onRenameFootprint(renameReference, renameText) }
                renameReference = nil
            }
            Button("Cancel", role: .cancel) { renameReference = nil }
        } message: { Text("This changes the board reference. Update its schematic reference separately.") }
        .sheet(isPresented: Binding(get: { choosingPlacement || swapReference != nil },
                                    set: { if !$0 { choosingPlacement = false; swapReference = nil } })) {
            if let footprintLibrary {
                FootprintPicker(library: footprintLibrary, title: swapReference.map { "Swap \($0)" } ?? "Place on the board",
                                filters: [], pins: 0) { id in
                    if let reference = swapReference {
                        onBoardEdit?(["swap", reference, id])
                    } else {
                        placeFootprint = id
                    }
                    choosingPlacement = false
                    swapReference = nil
                }
                .presentationDetents([.large])
            }
        }
        .sheet(isPresented: Binding(get: { editingZone != nil }, set: { if !$0 { editingZone = nil } })) {
            if let zone = editingZone {
                ZonePropertiesSheet(zone: zone, netName: nets.first { $0.code == zone.net && zone.net > 0 }.map { schematicText($0.name) } ?? "",
                                    nets: nets.filter { $0.code > 0 }.map { schematicText($0.name) }) { fields in
                    onBoardEdit?(fields)
                    editingZone = nil
                } onCancel: { editingZone = nil }
            }
        }
        .alert("Edit track", isPresented: Binding(
            get: { editingTrack != nil },
            set: { if !$0 { editingTrack = nil } }
        )) {
            TextField("Width (mm)", text: $trackWidth).keyboardType(.decimalPad)
            TextField("Copper layer", text: $trackLayer).autocorrectionDisabled().textInputAutocapitalization(.never)
            Button("Save") {
                if let editingTrack {
                    // KiCad checks the width (0.05-25 mm) and that the layer is one of the board's copper layers.
                    onBoardEdit?(["track", editingTrack.uuid,
                                  String(Double(trackWidth.replacingOccurrences(of: ",", with: ".")) ?? 0), trackLayer])
                }
                editingTrack = nil
            }
            Button("Cancel", role: .cancel) { editingTrack = nil }
        } message: { Text("KiCad's track width and copper layer, e.g. F.Cu, B.Cu or In1.Cu.") }
        .alert("Edit text", isPresented: Binding(
            get: { editingText != nil },
            set: { if !$0 { editingText = nil } }
        )) {
            TextField("Text", text: $editText)
            TextField("Height (mm)", text: $editSize).keyboardType(.decimalPad)
            Button("Save") {
                if let editingText {
                    // The engine checks the range (0.5-10 mm); an unreadable height is sent as 0.
                    onEditText?(editingText.uuid, editText, Double(editSize.replacingOccurrences(of: ",", with: ".")) ?? 0)
                }
                editingText = nil
            }
            Button("Cancel", role: .cancel) { editingText = nil }
        } message: { Text("One line; height 0.5-10 mm.") }
    }

    // MARK: Chrome

    /// Below a 1000 pt wide view (iPad mini and 11-inch portrait) the tool chips go above the
    /// status line: at the top they run under the layer panel.
    private var chipsBelow: Bool { canvasSize.width + Self.chrome.leading + Self.chrome.trailing < 1000 }

    private var placing: Bool { editable && (tool != .inspect || moveReference != nil || moveDrawing != nil) }

    private var status: String {
        if !editable { return "Imported board · read-only. Open a local project to edit." }
        if let moveReference { return "Tap the new location for \(moveReference)." }
        if let moveDrawing { return "Tap where the point you held goes · the \(moveDrawing.item.title) moves with it" }
        switch tool {
        case .inspect: return "Tap to select · hold a footprint, track, via, zone, line or text for actions"
        case .twoPad: return "Tap inside the board to place \(partReference)"
        case .mountingHole: return "Tap inside the board to drill an M3 hole"
        case .track:
            if let route {
                if let message = route.message { return message }
                if let tuning = route.tuning {
                    return String(format: "%.2f mm · %@ · drag along the track · tap to commit", tuning.length, tuning.status)
                }
                if route.dragging { return "Drag to move it · tap to drop it there" }
                return route.placingVia ? "Tap where the via goes · the route continues on the other layer"
                                        : "Drag to route · tap to fix · end on a pad, via or track of the net"
            }
            if onRoute != nil {
                switch routeKind {
                case .drag: return "Tap a track or via to drag it with KiCad's router"
                case .tune, .tunePair, .skew: return "Tap a routed track to tune it to the target"
                case .pair: return "Tap a pad of a differential pair to route both nets on \(copperLayer)"
                case .track: return "Tap a pad, track end or via to route on \(copperLayer) with KiCad's router"
                }
            }
            return routeStart == nil ? "Tap a pad, track end or via to start \(copperLayer) copper"
                                     : "Tap the next corner · end on a pad, track end or via"
        case .via: return "Tap a track end or pad to drop a via joining F.Cu and B.Cu"
        case .draw:
            switch drawKind {
            case .zone, .outline:
                let what = drawKind == .zone ? "zone on \(copperLayer)" : "new board outline (it replaces the old one)"
                return corners.count < 3 ? "Tap the corners of the \(what)" : "Tap the first corner or ✓ to finish"
            case .line: return corners.isEmpty ? "Tap where the \(drawLayer) line starts" : "Tap the next point · Done to stop"
            case .text: return "Tap where the \(drawLayer) text goes"
            case .dimension:
                return ["Tap where the \(drawLayer) dimension starts", "Tap where it ends",
                        "Tap where the dimension line goes"][min(corners.count, 2)]
            }
        }
    }

    private var fingerMillimetres: CGPoint? {
        guard let finger, bounds.width > 0 else { return nil }
        let point = placing ? snapped(finger, in: canvasSize) : raw(finger, in: canvasSize)
        return CGPoint(x: point.x, y: point.y)
    }

    @ViewBuilder private var contextChips: some View {
        if let moveReference {
            ChipBar {
                MonoLabel("Moving", color: Corv.textFaint, small: true)
                Text(moveReference).font(.corvReadout).foregroundStyle(Corv.gold)
                Button("Cancel") { self.moveReference = nil }.buttonStyle(.corvGhost)
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if let moveDrawing {
            ChipBar {
                MonoLabel("Moving", color: Corv.textFaint, small: true)
                Text(moveDrawing.item.title).font(.corvReadout).foregroundStyle(Corv.gold)
                Button("Cancel") { self.moveDrawing = nil }.buttonStyle(.corvGhost)
                    .accessibilityIdentifier("cancelMoveDrawing")
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .twoPad && editable {
            ChipBar {
                MonoLabel("Ref", color: Corv.textFaint, small: true)
                TextField("R1", text: $partReference).frame(width: 70).corvField()
                if let placeFootprint {
                    Text(placeFootprint).font(.corvReadout).foregroundStyle(Corv.gold).lineLimit(1).frame(maxWidth: 220)
                    Button { self.placeFootprint = nil } label: { Image(systemName: "xmark") }
                        .buttonStyle(.corvGhost)
                        .accessibilityLabel("Back to the two-pad part")
                } else {
                    MonoLabel("Value", color: Corv.textFaint, small: true)
                    TextField("10k", text: $partValue).frame(width: 90).corvField()
                }
                if footprintLibrary != nil && onBoardEdit != nil {
                    Button("Footprint") { choosingPlacement = true }
                        .buttonStyle(.corvGhost)
                        .accessibilityIdentifier("chooseBoardFootprint")
                }
            }
            .autocorrectionDisabled()
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .draw && editable {
            ChipBar {
                Picker("Draw", selection: $drawKind) {
                    ForEach(drawKinds, id: \.self) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .frame(width: onBoardEdit != nil ? 300 : 250)
                .accessibilityIdentifier("drawKind")
                switch drawKind {
                case .zone:
                    ForEach(["F.Cu", "B.Cu"], id: \.self) { name in
                        Button(name) { copperLayer = name }
                            .buttonStyle(.plain)
                            .font(.corvReadout)
                            .foregroundStyle(copperLayer == name ? (name == "B.Cu" ? Corv.cyan : Corv.gold) : Corv.textDim)
                            .accessibilityAddTraits(copperLayer == name ? .isSelected : [])
                    }
                    Menu {
                        Picker("Net", selection: $zoneNet) {
                            Text("No net").tag("")
                            ForEach(nets.filter { $0.code > 0 }, id: \.code) { net in
                                Text(schematicText(net.name)).tag(schematicText(net.name))
                            }
                        }
                    } label: {
                        Text(zoneNet.isEmpty ? "No net" : zoneNet).font(.corvReadout).foregroundStyle(Corv.cyan)
                    }
                    .accessibilityIdentifier("zoneNet")
                case .outline:
                    EmptyView()
                case .line, .text, .dimension:
                    Menu {
                        Picker("Layer", selection: $drawLayer) {
                            ForEach(Self.drawLayers, id: \.self) { Text($0).tag($0) }
                        }
                    } label: {
                        Text(drawLayer).font(.corvReadout).foregroundStyle(Corv.gold)
                    }
                    .accessibilityIdentifier("drawLayer")
                    if drawKind == .text {
                        TextField("Text", text: $drawText).frame(width: 130).corvField()
                            .autocorrectionDisabled()
                        TextField("mm", value: $textSize, format: .number)
                            .keyboardType(.decimalPad)
                            .frame(width: 56).corvField()
                            .accessibilityLabel("Text height in millimetres")
                    }
                }
                if !corners.isEmpty {
                    if drawKind == .line || drawKind == .dimension {
                        Button(drawKind == .line ? "Done" : "Cancel") { corners = [] }.buttonStyle(.corvGhost)
                    } else {
                        // Icons: the chip must fit beside the layer panel in portrait.
                        if corners.count >= 3 {
                            Button { finishShape() } label: { Image(systemName: "checkmark") }
                                .buttonStyle(.corvGhost)
                                .accessibilityLabel("Close shape")
                                .accessibilityIdentifier("drawClose")
                        }
                        Button { corners.removeLast() } label: { Image(systemName: "arrow.uturn.backward") }
                            .buttonStyle(.corvGhost)
                            .accessibilityLabel("Remove last corner")
                        Button { corners = [] } label: { Image(systemName: "xmark") }
                            .buttonStyle(.corvGhost)
                            .accessibilityLabel("Cancel shape")
                    }
                }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .track && editable, onRoute != nil {
            ChipBar {
                if let route {
                    Circle().fill(route.layer == "B.Cu" ? Corv.cyan : Corv.gold).frame(width: 8, height: 8)
                    MonoLabel("\(route.dragging ? "Dragging" : routeKind.title) · \(route.layer)", color: Corv.text, small: true)
                    if routeKind == .track || routeKind == .pair {
                        Button(route.placingVia ? "Via ✓" : "Via") { onRoute?(.toggleVia) }
                            .buttonStyle(.corvGhost)
                            .accessibilityIdentifier("routeVia")
                    }
                    Button("Finish") { onRoute?(.finish) }.buttonStyle(.corvGhost)
                        .accessibilityIdentifier("routeFinish")
                    Button("Cancel") { onRoute?(.cancel) }.buttonStyle(.corvGhost)
                        .accessibilityIdentifier("routeCancel")
                } else {
                    MonoLabel("Layer", color: Corv.textFaint, small: true)
                    ForEach(["F.Cu", "B.Cu"], id: \.self) { name in
                        Button(name) { copperLayer = name }
                            .buttonStyle(.plain)
                            .font(.corvReadout)
                            .foregroundStyle(copperLayer == name ? (name == "B.Cu" ? Corv.cyan : Corv.gold) : Corv.textDim)
                            .accessibilityAddTraits(copperLayer == name ? .isSelected : [])
                    }
                    Menu {
                        Picker("Route", selection: $routeKind) {
                            ForEach(RouteKind.allCases, id: \.self) { Text($0.title).tag($0) }
                        }
                    } label: {
                        Text(routeKind.title).font(.corvReadout).foregroundStyle(Corv.gold)
                    }
                    .accessibilityIdentifier("routeKind")
                    if routeKind.tunes {
                        TextField("mm", value: $tuneTarget, format: .number)
                            .keyboardType(.decimalPad)
                            .frame(width: 70).corvField()
                            .accessibilityIdentifier("tuneTarget")
                    } else {
                        Picker("Mode", selection: $routeMode) {
                            Text("Walk around").tag(2)
                            Text("Shove").tag(1)
                            Text("Highlight").tag(0)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 280)
                        .accessibilityIdentifier("routeMode")
                    }
                }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        } else if tool == .track && editable {
            ChipBar {
                MonoLabel("Layer", color: Corv.textFaint, small: true)
                ForEach(["F.Cu", "B.Cu"], id: \.self) { name in
                    Button(name) { copperLayer = name }
                        .buttonStyle(.plain)
                        .font(.corvReadout)
                        .foregroundStyle(copperLayer == name ? (name == "B.Cu" ? Corv.cyan : Corv.gold) : Corv.textDim)
                        .accessibilityAddTraits(copperLayer == name ? .isSelected : [])
                }
                if let routeStart {
                    MonoLabel("Net", color: Corv.textFaint, small: true)
                    Text(routeStart.net.isEmpty ? "None" : routeStart.net).font(.corvReadout).foregroundStyle(Corv.cyan)
                    Button("Done") { self.routeStart = nil }.buttonStyle(.corvGhost)
                }
            }
            .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    private var layerPanel: some View {
        VStack(alignment: .trailing, spacing: 10) {
            Menu {
                Picker("Copper", selection: $layer) {
                    Text("All copper").tag(0)
                    Text("Front · F.Cu").tag(1)
                    Text("Back · B.Cu").tag(2)
                }
                Picker("Net", selection: $selectedNet) {
                    Text("All nets").tag(-1)
                    ForEach(nets.filter { $0.code > 0 }, id: \.code) { net in
                        Text(schematicText(net.name)).tag(Int(net.code))
                    }
                }
                .pickerStyle(.menu)
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "square.3.layers.3d").foregroundStyle(Corv.gold)
                    MonoLabel(["All copper", "Front", "Back"][layer], color: Corv.text)
                    if selectedNet != -1,
                       let net = nets.first(where: { Int($0.code) == selectedNet }) {
                        MonoLabel("· " + schematicText(net.name), color: Corv.cyan)
                    }
                    Image(systemName: "chevron.down").font(.caption2).foregroundStyle(Corv.textFaint)
                }
                .padding(.horizontal, 14).padding(.vertical, 10)
                .corvPanel(radius: 14)
            }
            HStack(spacing: 12) {
                swatch(Corv.gold, "F.Cu")
                swatch(Corv.cyan, "B.Cu")
                swatch(Corv.goldSoft, "Pad")
                swatch(.white, "Via")
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            .corvPanel(radius: 12)
            if editable {
                Button(action: onUpdateFromSchematic) {
                    Label("Update from schematic", systemImage: "arrow.triangle.2.circlepath")
                }
                .buttonStyle(.corvGhost)
                .accessibilityIdentifier("updateFromSchematic")
            }
        }
    }

    private func swatch(_ color: Color, _ title: String) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 7, height: 7)
            MonoLabel(title, small: true)
        }
    }

    private var menuItems: [OrbitMenu.Item] {
        if let copper = radialCopper {
            var items: [OrbitMenu.Item] = []
            if copper.drawn, onMoveDrawing != nil {
                items.append(.init(id: "Move", icon: "arrow.up.and.down.and.arrow.left.and.right") {
                    moveDrawing = (copper, heldAt); radialCopper = nil; tool = .inspect
                })
            }
            if copper.isZone, onBoardEdit != nil, let zone = zones.first(where: { $0.uuid == copper.uuid && !$0.filled }),
               zone.properties.teardrop == 0 {
                items.append(.init(id: "Properties", icon: "slider.horizontal.3") {
                    editingZone = zone; radialCopper = nil
                })
            }
            if case let .track(width, layer) = copper.kind, onBoardEdit != nil {
                items.append(.init(id: "Edit", icon: "slider.horizontal.3") {
                    trackWidth = String(format: "%g", width); trackLayer = layer
                    editingTrack = copper; radialCopper = nil
                })
            }
            if case .text(let value, let height) = copper.kind, onEditText != nil {
                items.append(.init(id: "Edit", icon: "character.cursor.ibeam") {
                    editText = value; editSize = String(format: "%g", height)
                    editingText = copper; radialCopper = nil
                })
            }
            items.append(.init(id: "Delete", icon: "trash", destructive: true) {
                radialCopper = nil
                onDeleteCopper(copper.uuid)
            })
            return items
        }
        guard let index = radialIndex, footprints.indices.contains(index) else { return [] }
        let reference = footprints[index].2
        var items: [OrbitMenu.Item] = [
            .init(id: "Move", icon: "arrow.up.and.down.and.arrow.left.and.right") {
                moveReference = reference; radialIndex = nil; tool = .inspect
            },
            .init(id: "Rotate", icon: "rotate.left") {
                radialIndex = nil
                onRotateFootprint(reference)
            },
            .init(id: "Rename", icon: "character.cursor.ibeam") {
                renameReference = reference; renameText = reference; radialIndex = nil
            },
        ]
        if onBoardEdit != nil, footprintLibrary != nil {
            // KiCad's Change Footprint: another library footprint in its place.
            items.append(.init(id: "Swap", icon: "arrow.triangle.2.circlepath") {
                radialIndex = nil
                swapReference = reference
            })
        }
        if let onBoardEdit {
            // KiCad's Flip: to the other side, mirrored left to right about its position.
            items.append(.init(id: "Flip", icon: "arrow.left.and.right.righttriangle.left.righttriangle.right") {
                radialIndex = nil
                onBoardEdit(["flip", reference])
            })
        }
        items.append(.init(id: "Delete", icon: "trash", destructive: true) {
            radialIndex = nil; selected = nil
            onDeleteFootprint(reference)
        })
        return items
    }

    // MARK: Drawing

    private func layout(_ size: CGSize) -> (scale: CGFloat, left: CGFloat, top: CGFloat) {
        let scale = min(size.width / bounds.width, size.height / bounds.height)
        return (scale, (size.width - bounds.width * scale) / 2, (size.height - bounds.height * scale) / 2)
    }

    private func draw(_ context: inout GraphicsContext, size: CGSize) {
        guard bounds.width > 0, bounds.height > 0 else { return }
        let (scale, left, top) = layout(size)
        func position(_ x: Double, _ y: Double) -> CGPoint {
            CGPoint(x: left + (CGFloat(x) - bounds.minX) * scale,
                    y: top + (CGFloat(y) - bounds.minY) * scale)
        }

        // 5 mm reference grid behind the board.
        var grid = Path()
        let step = 5.0
        let x0 = (Double(bounds.minX) / step).rounded(.down) * step - step * 2
        let y0 = (Double(bounds.minY) / step).rounded(.down) * step - step * 2
        for x in stride(from: x0, through: Double(bounds.maxX) + step * 2, by: step) {
            grid.move(to: position(x, y0)); grid.addLine(to: position(x, Double(bounds.maxY) + step * 2))
        }
        for y in stride(from: y0, through: Double(bounds.maxY) + step * 2, by: step) {
            grid.move(to: position(x0, y)); grid.addLine(to: position(Double(bounds.maxX) + step * 2, y))
        }
        context.stroke(grid, with: .color(.white.opacity(0.045)), lineWidth: 0.6)

        var edge = Path()
        for (index, p) in outline.enumerated() {
            let q = position(Double(p.x), Double(p.y))
            if index == 0 { edge.move(to: q) } else { edge.addLine(to: q) }
        }
        edge.closeSubpath()
        context.fill(edge, with: .linearGradient(
            Gradient(colors: [Corv.solderMask, Corv.solderMask.opacity(0.75)]),
            startPoint: CGPoint(x: left, y: top), endPoint: CGPoint(x: left, y: top + bounds.height * scale)))
        var glow = context
        glow.addFilter(.blur(radius: 6))
        glow.stroke(edge, with: .color(Corv.cyan.opacity(0.25)), lineWidth: 4)
        context.stroke(edge, with: .color(.white.opacity(0.92)), lineWidth: 1.5)

        // Zones under the copper: fills as stored (fractured, even-odd), outlines dashed.
        for zone in zones where zone.points.count > 2 {
            if layer == 1 && zone.layer != "F.Cu" { continue }
            if layer == 2 && zone.layer != "B.Cu" { continue }
            let active = selectedNet == -1 || selectedNet == Int(zone.net)
            var path = Path()
            path.move(to: position(Double(zone.points[0].x), Double(zone.points[0].y)))
            for point in zone.points.dropFirst() { path.addLine(to: position(Double(point.x), Double(point.y))) }
            path.closeSubpath()
            let color = zone.layer == "B.Cu" ? Corv.cyan : zone.layer == "F.Cu" ? Corv.gold : Color.white
            if zone.filled {
                context.fill(path, with: .color(color.opacity(active ? 0.22 : 0.05)), style: FillStyle(eoFill: true))
            } else {
                context.stroke(path, with: .color(color.opacity(active ? 0.5 : 0.12)),
                               style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
            }
        }
        let hidden = route?.hidden ?? []
        for track in tracks where !hidden.contains(schematicText(track.uuid)) {
            let name = schematicText(track.layer)
            if layer == 1 && name != "F.Cu" { continue }
            if layer == 2 && name != "B.Cu" { continue }
            let active = selectedNet == -1 || selectedNet == Int(track.net)
            var path = Path()
            path.move(to: position(track.x0, track.y0))
            path.addLine(to: position(track.x1, track.y1))
            let color = name == "B.Cu" ? Corv.cyan : Corv.gold
            context.stroke(path, with: .color(color.opacity(active ? 0.95 : 0.12)),
                           style: StrokeStyle(lineWidth: max(2, CGFloat(track.width) * scale), lineCap: .round))
        }
        for pad in pads {
            let copper = schematicText(pad.layers)
            if layer == 1 && !copper.contains("F.Cu") && !copper.contains("*.Cu") { continue }
            if layer == 2 && !copper.contains("B.Cu") && !copper.contains("*.Cu") { continue }
            let active = selectedNet == -1 || selectedNet == Int(pad.net)
            let center = position(pad.x, pad.y)
            let width = max(4, CGFloat(pad.size_x) * scale)
            let height = max(4, CGFloat(pad.size_y) * scale)
            let shape: Path
            let padShape = schematicText(pad.shape)
            if padShape == "circle" || padShape == "oval" {
                shape = Path(ellipseIn: CGRect(x: center.x-width/2, y: center.y-height/2,
                                               width: width, height: height))
            } else {
                let angle = pad.rotation * Double.pi / 180
                let c = CGFloat(cos(angle)), s = CGFloat(sin(angle))
                func corner(_ dx: CGFloat, _ dy: CGFloat) -> CGPoint {
                    CGPoint(x: center.x + dx*c - dy*s, y: center.y + dx*s + dy*c)
                }
                var polygon = Path()
                polygon.move(to: corner(-width/2, -height/2))
                polygon.addLine(to: corner(width/2, -height/2))
                polygon.addLine(to: corner(width/2, height/2))
                polygon.addLine(to: corner(-width/2, height/2))
                polygon.closeSubpath()
                shape = polygon
            }
            if schematicText(pad.type) == "np_thru_hole" {
                // Non-plated hole: a drilled opening, not copper.
                context.fill(shape, with: .color(Corv.void))
                context.stroke(shape, with: .color(.white.opacity(0.55)), lineWidth: 1.2)
            } else {
                context.fill(shape, with: .color(Corv.goldSoft.opacity(active ? 0.95 : 0.12)))
            }
        }
        for via in vias where !hidden.contains(schematicText(via.uuid)) {
            let active = selectedNet == -1 || selectedNet == Int(via.net)
            let center = position(via.x, via.y)
            let radius = max(3, CGFloat(via.size) * scale / 2)
            let ring = Path(ellipseIn: CGRect(x: center.x-radius, y: center.y-radius,
                                              width: radius*2, height: radius*2))
            context.fill(ring, with: .color(Corv.void.opacity(active ? 0.9 : 0.3)))
            context.stroke(ring, with: .color(.white.opacity(active ? 0.9 : 0.15)), lineWidth: 2)
        }
        // Silkscreen and fab drawings over the copper; back-side items dimmer, back text mirrored.
        func visible(_ layerName: String) -> Bool {
            !(layer == 1 && layerName.hasPrefix("B.")) && !(layer == 2 && layerName.hasPrefix("F."))
        }
        func ink(_ layerName: String) -> Color {
            layerName.hasSuffix("SilkS") ? .white.opacity(layerName.hasPrefix("B.") ? 0.45 : 0.85) : Corv.textFaint
        }
        for line in graphics {
            let name = schematicText(line.layer)
            guard visible(name) else { continue }
            var path = Path()
            path.move(to: position(line.x0, line.y0))
            path.addLine(to: position(line.x1, line.y1))
            context.stroke(path, with: .color(ink(name)),
                           style: StrokeStyle(lineWidth: max(1, CGFloat(line.width) * scale), lineCap: .round))
        }
        for text in texts {
            let name = schematicText(text.layer)
            guard visible(name) else { continue }
            var label = context
            label.translateBy(x: position(text.x, text.y).x, y: position(text.x, text.y).y)
            label.rotate(by: .degrees(-text.rotation))
            if name.hasPrefix("B.") { label.scaleBy(x: -1, y: 1) }
            // KiCad's size is the glyph height; a system font's cap height is about 0.7 of its size.
            label.draw(Text(schematicText(text.text)).font(.system(size: max(6, CGFloat(text.size) * scale / 0.7)))
                        .foregroundStyle(ink(name)), at: .zero)
        }
        // Ratsnest: thin straight lines, as KiCad draws unrouted connections.
        let netName = selectedNet == -1 ? nil : nets.first { Int($0.code) == selectedNet }.map { schematicText($0.name) }
        var unrouted = Path()
        for edge in ratsnest where netName == nil || edge.net == netName {
            unrouted.move(to: position(Double(edge.start.x), Double(edge.start.y)))
            unrouted.addLine(to: position(Double(edge.end.x), Double(edge.end.y)))
        }
        context.stroke(unrouted, with: .color(.white.opacity(0.55)), lineWidth: 1)
        for finding in findings {
            guard let location = finding.location else { continue }
            let center = position(Double(location.x), Double(location.y))
            let picked = finding.id == selectedFinding
            let radius: CGFloat = picked ? 11 : 6
            let ring = Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius,
                                              width: radius * 2, height: radius * 2))
            if picked { context.halo(at: center, radius: 22) }
            context.stroke(ring, with: .color(finding.isError ? Corv.red : Corv.gold.opacity(0.75)),
                           lineWidth: picked ? 2.5 : 1.5)
        }
        for (index, (x, y, reference)) in footprints.enumerated() where !reference.isEmpty {
            let point = position(x, y)
            if index == selected { context.halo(at: point, radius: 20) }
            context.draw(Text(reference).font(.corvMonoSmall).tracking(0.8)
                            .foregroundStyle(index == selected ? Corv.gold : Color.white.opacity(0.78)),
                         at: CGPoint(x: point.x + 6, y: point.y + 6), anchor: .topLeading)
        }
        if let route {
            // KiCad's preview: the routed head and shoved tracks at their real width.
            for track in route.tracks where track.points.count > 1 {
                if layer == 1 && track.layer != "F.Cu" { continue }
                if layer == 2 && track.layer != "B.Cu" { continue }
                var path = Path()
                path.addLines(track.points.map { position(Double($0.x), Double($0.y)) })
                let color = track.layer == "B.Cu" ? Corv.cyan : Corv.gold
                context.stroke(path, with: .color(color.opacity(route.message == nil ? 0.95 : 0.5)),
                               style: StrokeStyle(lineWidth: max(2, CGFloat(track.width) * scale), lineCap: .round,
                                                  lineJoin: .round))
            }
            for via in route.vias {
                let center = position(Double(via.center.x), Double(via.center.y))
                let radius = max(3, CGFloat(via.diameter) * scale / 2)
                let ring = Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius,
                                                  width: radius * 2, height: radius * 2))
                context.fill(ring, with: .color(Corv.void.opacity(0.9)))
                context.stroke(ring, with: .color(.white), lineWidth: 2)
            }
            if let end = route.end {
                context.halo(at: position(Double(end.x), Double(end.y)), radius: 8)
            }
        }
        if let routeStart {
            let start = position(Double(routeStart.point.x), Double(routeStart.point.y))
            context.halo(at: start, radius: 8)
            if let finger {
                let end = anchor(near: finger, in: size)?.point ?? corner(at: finger, in: size, from: routeStart.point)
                var preview = Path()
                preview.move(to: start)
                preview.addLine(to: position(Double(end.x), Double(end.y)))
                context.stroke(preview, with: .color((copperLayer == "B.Cu" ? Corv.cyan : Corv.gold).opacity(0.9)),
                               style: StrokeStyle(lineWidth: max(2, 0.25 * scale), lineCap: .round, dash: [6, 5]))
            }
        }
        if tool == .draw, !corners.isEmpty {
            // The shape or line in progress, to the finger's grid point; the first corner glows
            // once the shape can close.
            var preview = Path()
            preview.addLines(corners.map { position(Double($0.x), Double($0.y)) })
            if let finger {
                let point = snapped(finger, in: size)
                preview.addLine(to: position(point.x, point.y))
            }
            if drawKind != .line, corners.count >= 3 { preview.closeSubpath() }
            let color = drawKind == .zone ? (copperLayer == "B.Cu" ? Corv.cyan : Corv.gold) : .white
            context.stroke(preview, with: .color(color.opacity(0.9)), style: StrokeStyle(lineWidth: 1.5, dash: [6, 5]))
            if drawKind != .line, corners.count >= 3, let first = corners.first {
                context.halo(at: position(Double(first.x), Double(first.y)), radius: 10)
            }
        }
        if let finger, placing {
            if tool == .track || tool == .via {
                let hit = anchor(near: finger, in: size)
                let point = hit?.point ?? routeStart.map { corner(at: finger, in: size, from: $0.point) }
                    ?? CGPoint(x: snapped(finger, in: size).x, y: snapped(finger, in: size).y)
                context.reticle(at: position(Double(point.x), Double(point.y)), color: hit == nil ? .white.opacity(0.7) : Corv.cyan,
                                label: hit.map { $0.net.isEmpty ? $0.reason : "\($0.reason) · \($0.net)" })
            } else {
                let point = snapped(finger, in: size)
                context.reticle(at: position(point.x, point.y), color: Corv.cyan, label: nil)
            }
        }
    }

    // MARK: Interaction

    private func netName(_ code: Int32) -> String {
        guard code > 0 else { return "" }
        return nets.first { $0.code == code }.map { schematicText($0.name) } ?? ""
    }

    /// Pads, track ends and vias within reach of `location`, nearest first.
    private func anchor(near location: CGPoint, in size: CGSize) -> Anchor? {
        let (scale, left, top) = layout(size)
        var candidates: [Anchor] = []
        for pad in pads where schematicText(pad.type) != "np_thru_hole" {
            candidates.append(Anchor(point: CGPoint(x: pad.x, y: pad.y), net: netName(pad.net),
                                     reason: "\(schematicText(pad.reference)).\(schematicText(pad.number))"))
        }
        for track in tracks {
            for end in [CGPoint(x: track.x0, y: track.y0), CGPoint(x: track.x1, y: track.y1)] {
                candidates.append(Anchor(point: end, net: netName(track.net), reason: "Track end"))
            }
        }
        for via in vias { candidates.append(Anchor(point: CGPoint(x: via.x, y: via.y), net: netName(via.net), reason: "Via")) }
        func distance(_ anchor: Anchor) -> CGFloat {
            hypot(left + (anchor.point.x - bounds.minX) * scale - location.x,
                  top + (anchor.point.y - bounds.minY) * scale - location.y)
        }
        guard let nearest = candidates.min(by: { distance($0) < distance($1) }), distance(nearest) <= 22 else { return nil }
        return nearest
    }

    /// A free routing corner: the 0.5 mm grid, except that each axis lines up with the start or
    /// any pad, track end or via within 0.75 mm, so legs into off-grid pads stay straight.
    private func corner(at location: CGPoint, in size: CGSize, from start: CGPoint) -> CGPoint {
        let grid = snapped(location, in: size)
        let raw = raw(location, in: size)
        var points = [start] + pads.map { CGPoint(x: $0.x, y: $0.y) } + vias.map { CGPoint(x: $0.x, y: $0.y) }
        for track in tracks { points += [CGPoint(x: track.x0, y: track.y0), CGPoint(x: track.x1, y: track.y1)] }
        // The start comes first, so a leg away from it stays straight.
        func aligned(_ value: Double, _ axis: (CGPoint) -> CGFloat, _ fallback: Double) -> Double {
            if abs(Double(axis(start)) - value) < 0.75 { return Double(axis(start)) }
            let near: [Double] = points.map { Double(axis($0)) }.filter { abs($0 - value) < 0.75 }
            let closest: Double? = near.min { (a: Double, b: Double) -> Bool in abs(a - value) < abs(b - value) }
            return closest ?? fallback
        }
        return CGPoint(x: aligned(raw.x, \.x, grid.x), y: aligned(raw.y, \.y, grid.y))
    }

    private func place(at location: CGPoint, in size: CGSize) {
        guard (editable || tool == .inspect), bounds.width > 0, bounds.height > 0 else { return }
        if radialIndex != nil || radialCopper != nil { radialIndex = nil; radialCopper = nil; return }
        if tool == .track, let onRoute {
            let point = routerPoint(location, in: size)
            onRoute(route == nil ? .start(point, layer: copperLayer, mode: routeMode, kind: routeKind, target: tuneTarget)
                                 : .fix(point))
            return
        }
        if tool == .track {
            let hit = anchor(near: location, in: size)
            guard let start = routeStart else {
                if let hit { routeStart = (hit.point, hit.net) }
                return
            }
            let end = hit?.point ?? corner(at: location, in: size, from: start.point)
            guard hypot(end.x - start.point.x, end.y - start.point.y) > 0.01 else { return }
            onAddTrack(start.point, end, copperLayer, start.net)
            routeStart = hit == nil ? (end, start.net) : nil
            return
        }
        if tool == .via {
            if let hit = anchor(near: location, in: size) { onAddVia(hit.point, hit.net) }
            return
        }
        let point = snapped(location, in: size)
        if tool == .draw {
            draw(at: CGPoint(x: point.x, y: point.y), tapped: location, in: size)
            return
        }
        if let moveReference {
            onMoveFootprint(moveReference, point.x, point.y)
            self.moveReference = nil
            return
        }
        if let moving = moveDrawing {
            moveDrawing = nil
            let offset = CGSize(width: point.x - moving.from.x, height: point.y - moving.from.y)
            if offset != .zero { onMoveDrawing?(moving.item.uuid, offset, moving.item.isZone) }
            return
        }
        switch tool {
        case .inspect:
            selected = nearestFootprint(to: location, in: size)
            if let selected { onSelectFootprint(selected) }
        case .twoPad:
            if let placeFootprint, let onBoardEdit {
                onBoardEdit(["place", placeFootprint, partReference, String(Double(point.x)), String(Double(point.y))])
            } else {
                onAddTwoPad(partReference, partValue, point.x, point.y)
            }
        case .mountingHole: onAddHole(point.x, point.y)
        case .track, .via, .draw: break // handled above
        }
    }

    /// A Draw tool tap at a 0.5 mm grid point: a corner (tapping the first corner closes the
    /// shape), a line end (lines chain from the last end), or the text's position.
    private func draw(at point: CGPoint, tapped location: CGPoint, in size: CGSize) {
        switch drawKind {
        case .zone, .outline:
            if corners.count >= 3, let first = corners.first, screenDistance(first, location, in: size) <= 22 {
                finishShape()
            } else if corners.last != point {
                corners.append(point)
            }
        case .line:
            if let start = corners.first, start != point { onDraw?(.line(start, point, layer: drawLayer)) }
            corners = [point]
        case .text:
            onDraw?(.text(drawText, point, layer: drawLayer, size: textSize))
        case .dimension:
            // KiCad's three clicks: start, end, then the crossbar's side and distance.
            guard corners.count == 2 else { if corners.last != point { corners.append(point) }; return }
            let a = corners[0], b = corners[1], dx = b.x - a.x, dy = b.y - a.y, length = hypot(dx, dy)
            corners = []
            guard length > 0 else { return }
            let height = Double(((point.x - a.x) * -dy + (point.y - a.y) * dx) / length)
            onDraw?(.dimension(a, b, height: height, layer: drawLayer))
        }
    }

    private func finishShape() {
        guard corners.count >= 3 else { return }
        onDraw?(drawKind == .zone ? .zone(corners, layer: copperLayer, net: zoneNet) : .outline(corners))
        corners = []
    }

    private func screenDistance(_ point: CGPoint, _ location: CGPoint, in size: CGSize) -> CGFloat {
        let (scale, left, top) = layout(size)
        return hypot(left + (point.x - bounds.minX) * scale - location.x, top + (point.y - bounds.minY) * scale - location.y)
    }

    #if DEBUG
    /// For UI tests (NativeKiCadTests): the first unrouted connection's ends in window points and
    /// the copper counts, as an invisible element's label "x0 y0 x1 y1 tracks vias unrouted".
    private func testProbe(_ geometry: GeometryProxy) -> some View {
        let (scale, left, top) = layout(geometry.size)
        let origin = geometry.frame(in: .global).origin
        func window(_ point: CGPoint) -> String {
            "\(Int(origin.x + left + (point.x - bounds.minX) * scale)) \(Int(origin.y + top + (point.y - bounds.minY) * scale))"
        }
        let edge = ratsnest.first.map { "\(window($0.start)) \(window($0.end))" } ?? "0 0 0 0"
        return ZStack {
            Color.clear.frame(width: 1, height: 1)
                .allowsHitTesting(false)
                .accessibilityElement()
                .accessibilityIdentifier("ratsnestProbe")
                .accessibilityLabel("\(edge) \(tracks.count) \(vias.count) \(ratsnest.count)")
            // Board millimetres to window points: window = origin + offset + (mm - min) * scale.
            Color.clear.frame(width: 1, height: 1)
                .allowsHitTesting(false)
                .accessibilityElement()
                .accessibilityIdentifier("viewportProbe")
                // A plain String: a string literal here would be localized ("71,12" in Turkish).
                .accessibilityLabel(String("\(bounds.minX) \(bounds.minY) \(scale) \(origin.x + left) \(origin.y + top)"))
            // Every footprint "ref x y" and every unrouted connection "x0 y0 x1 y1" in board
            // millimetres, ";" between items (ShowcaseTests).
            Color.clear.frame(width: 1, height: 1)
                .allowsHitTesting(false)
                .accessibilityElement()
                .accessibilityIdentifier("footprintsProbe")
                .accessibilityLabel(footprints.map { "\($0.2) \($0.0) \($0.1)" }.joined(separator: ";"))
            Color.clear.frame(width: 1, height: 1)
                .allowsHitTesting(false)
                .accessibilityElement()
                .accessibilityIdentifier("unroutedProbe")
                .accessibilityLabel(ratsnest.map { "\($0.start.x) \($0.start.y) \($0.end.x) \($0.end.y)" }.joined(separator: ";"))
        }
    }
    #endif

    /// Where the finger asks KiCad's router to go: a pad, track end or via within reach, else the
    /// 0.5 mm grid (the router snaps to items within 1 mm itself). Tuning and dragging work on
    /// the track itself: the finger's point, unsnapped.
    private func routerPoint(_ location: CGPoint, in size: CGSize) -> CGPoint {
        if routeKind.tunes || routeKind == .drag {
            let point = raw(location, in: size)
            return CGPoint(x: point.x, y: point.y)
        }
        if let hit = anchor(near: location, in: size) { return hit.point }
        let point = snapped(location, in: size)
        return CGPoint(x: point.x, y: point.y)
    }

    private func raw(_ location: CGPoint, in size: CGSize) -> (x: Double, y: Double) {
        let (scale, left, top) = layout(size)
        guard scale > 0 else { return (0, 0) }
        return (Double((location.x - left) / scale + bounds.minX), Double((location.y - top) / scale + bounds.minY))
    }

    private func snapped(_ location: CGPoint, in size: CGSize) -> (x: Double, y: Double) {
        let point = raw(location, in: size)
        return ((point.x * 2).rounded() / 2, (point.y * 2).rounded() / 2)
    }

    private func nearestFootprint(to location: CGPoint, in size: CGSize) -> Int? {
        guard bounds.width > 0, bounds.height > 0 else { return nil }
        let (scale, left, top) = layout(size)
        func distance(_ index: Int) -> CGFloat {
            hypot(left + (CGFloat(footprints[index].0) - bounds.minX) * scale - location.x,
                  top + (CGFloat(footprints[index].1) - bounds.minY) * scale - location.y)
        }
        guard let nearest = footprints.indices.min(by: { distance($0) < distance($1) }) else { return nil }
        return distance(nearest) <= 30 ? nearest : nil
    }

    /// The via, track segment or drawn item under `location` (vias first, zones last).
    private func copperHit(at location: CGPoint, in size: CGSize) -> HeldItem? {
        let (scale, left, top) = layout(size)
        func screen(_ x: Double, _ y: Double) -> CGPoint {
            CGPoint(x: left + (CGFloat(x) - bounds.minX) * scale, y: top + (CGFloat(y) - bounds.minY) * scale)
        }
        if let via = vias.first(where: { hypot(screen($0.x, $0.y).x - location.x, screen($0.x, $0.y).y - location.y) <= 14 }) {
            return HeldItem(uuid: schematicText(via.uuid), title: "Via", kind: .copper)
        }
        for track in tracks {
            let a = screen(track.x0, track.y0), b = screen(track.x1, track.y1)
            let dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy
            guard length > 0 else { continue }
            let t = min(1, max(0, ((location.x - a.x) * dx + (location.y - a.y) * dy) / length))
            if hypot(a.x + t * dx - location.x, a.y + t * dy - location.y) <= 12 {
                return HeldItem(uuid: schematicText(track.uuid), title: "Track · \(schematicText(track.layer))",
                                kind: .track(width: track.width, layer: schematicText(track.layer)))
            }
        }
        if let text = texts.first(where: { hypot(screen($0.x, $0.y).x - location.x, screen($0.x, $0.y).y - location.y) <= 20 }) {
            return HeldItem(uuid: schematicText(text.uuid), title: "Text · \(schematicText(text.layer))",
                            kind: .text(schematicText(text.text), text.size))
        }
        for line in graphics {
            let a = screen(line.x0, line.y0), b = screen(line.x1, line.y1)
            let dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy
            guard length > 0 else { continue }
            let t = min(1, max(0, ((location.x - a.x) * dx + (location.y - a.y) * dy) / length))
            if hypot(a.x + t * dx - location.x, a.y + t * dy - location.y) <= 12 {
                return HeldItem(uuid: schematicText(line.uuid), title: "Line · \(schematicText(line.layer))", kind: .line)
            }
        }
        // Inside a zone's outline (even-odd), last: zones lie under everything else.
        for zone in zones where !zone.filled && !zone.uuid.isEmpty {
            let points = zone.points.map { screen(Double($0.x), Double($0.y)) }
            var inside = false
            for (i, a) in points.enumerated() {
                let b = points[(i + 1) % points.count]
                if (a.y > location.y) != (b.y > location.y),
                   location.x < (b.x - a.x) * (location.y - a.y) / (b.y - a.y) + a.x { inside.toggle() }
            }
            if inside { return HeldItem(uuid: zone.uuid, title: "Zone · \(zone.layer)", kind: .zone) }
        }
        return nil
    }

    private func hold(at location: CGPoint, in size: CGSize) {
        guard editable else { return }
        guard let index = nearestFootprint(to: location, in: size) else {
            if let copper = copperHit(at: location, in: size) {
                tool = .inspect
                radialHover = nil
                radialCopper = copper
                let point = snapped(location, in: size)
                heldAt = CGPoint(x: point.x, y: point.y)
                radialLocation = OrbitMenu.clamped(location, in: size)
            }
            return
        }
        tool = .inspect
        radialCopper = nil
        selected = index
        onSelectFootprint(index)
        radialHover = nil
        radialIndex = index
        radialLocation = OrbitMenu.clamped(location, in: size)
    }
}

/// KiCad's zone properties for one copper zone (NATIVE_KICAD): net, priority, clearance, minimum
/// width, pad connection with thermal relief, and fill mode. Saving refills the zones with
/// KiCad's filler in the same undo step.
struct ZonePropertiesSheet: View {
    let zone: ZoneShape
    /// The zone's current net (empty: none).
    let netName: String
    let nets: [String]
    let onSave: ([String]) -> Void
    let onCancel: () -> Void
    @State private var net = ""
    @State private var priority = 0
    @State private var clearance = 0.5
    @State private var minimum = 0.25
    @State private var connection = 1
    @State private var gap = 0.5
    @State private var spoke = 0.5
    @State private var hatched = false

    private static let connections = ["solid", "thermal", "none", "tht_thermal"]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        MonoLabel("Zone properties · KiCad", color: Corv.gold)
                        Text(zone.layer).font(.corvDisplay(24)).foregroundStyle(Corv.text)
                    }
                    Spacer()
                    Button("Cancel", action: onCancel).buttonStyle(.corvGhost)
                    Button("Save") {
                        let number = { (value: Double) in String(format: "%g", locale: Locale(identifier: "en_US_POSIX"), value) }
                        onSave(["zone", zone.uuid, net, String(priority), number(clearance), number(minimum),
                                Self.connections[connection], number(gap), number(spoke), hatched ? "hatched" : "solid"])
                    }
                    .buttonStyle(.corvPrimary)
                    .accessibilityIdentifier("saveZone")
                }
                Picker("Net", selection: $net) {
                    Text("No net").tag("")
                    ForEach(nets, id: \.self) { Text($0).tag($0) }
                }
                .pickerStyle(.menu).tint(Corv.gold)
                .accessibilityIdentifier("zoneNetPicker")
                Stepper(value: $priority, in: 0...1000) {
                    HStack { MonoLabel("Priority", small: true); Spacer(); Text("\(priority)").font(.corvReadout).foregroundStyle(Corv.gold) }
                }
                .accessibilityIdentifier("zonePriority")
                HStack(spacing: 12) {
                    NumberField(title: "Clearance mm", value: $clearance)
                    NumberField(title: "Min width mm", value: $minimum)
                }
                MonoLabel("Pad connection", color: Corv.textFaint, small: true)
                Picker("Pad connection", selection: $connection) {
                    Text("Solid").tag(0)
                    Text("Thermal").tag(1)
                    Text("THT thermal").tag(3)
                    Text("None").tag(2)
                }
                .pickerStyle(.segmented)
                .accessibilityIdentifier("zoneConnection")
                if connection == 1 || connection == 3 {
                    HStack(spacing: 12) {
                        NumberField(title: "Thermal gap mm", value: $gap)
                        NumberField(title: "Spoke width mm", value: $spoke)
                    }
                }
                Picker("Fill", selection: $hatched) {
                    Text("Solid fill").tag(false)
                    Text("Hatched").tag(true)
                }
                .pickerStyle(.segmented)
                CautionNote(text: "KiCad 10.0.6's zone settings, saved in the board; KiCad's filler refills the zones in the same undo step.")
            }
            .padding(24)
        }
        .background(Corv.space)
        .presentationDetents([.medium, .large])
        .onAppear {
            // One-time seed from the board file: the sheet edits a copy until Save.
            let p = zone.properties
            net = netName
            priority = Int(p.priority)
            clearance = p.clearance
            minimum = p.min_thickness
            connection = Int(p.connection)
            gap = p.thermal_gap
            spoke = p.spoke_width
            hatched = p.hatched != 0
        }
    }
}
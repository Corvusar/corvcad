import SwiftUI

/// Static home page: never scrolls. Type, spacing and the project grid are sized
/// from the available space, reflowing between landscape (side by side) and portrait (stacked).
struct HomeView: View {
    let projects: [URL]
    /// Title of the design already loaded in this session, if any.
    let resumeTitle: String?
    let onNew: () -> Void
    let onImportBoard: () -> Void
    let onImportSchematic: () -> Void
    let onOpen: (URL) -> Void
    let onResume: () -> Void
    /// Long press on a project card: rename (folder and KiCad files) or delete (confirmed).
    var onRename: (URL, String) -> Void = { _, _ in }
    var onDelete: (URL) -> Void = { _ in }
    /// Tools that need no project: the Gerber viewer and the PCB calculator.
    var onGerbers: () -> Void = {}
    var onCalculator: () -> Void = {}
    /// KiCad's importers (NATIVE_KICAD): Altium, Eagle, EasyEDA and other CAD files into a new
    /// project; nil hides the action.
    var onImportOther: (() -> Void)? = nil

    @State private var showLicenses = false
    @State private var renaming: URL?
    @State private var newName = ""
    @State private var deleting: URL?

    private static let gap: CGFloat = 14
    private static let minCard = CGSize(width: 200, height: 136)

    var body: some View {
        ZStack {
            VoidBackground()
            layout
        }
        .alert("Rename project", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newName)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
            Button("Rename") {
                if let renaming { onRename(renaming, newName) }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        } message: {
            Text("Renames the project folder and its KiCad files. Letters, numbers, spaces, hyphens or underscores.")
        }
        .confirmationDialog("Delete \(deleting?.lastPathComponent ?? "project")?",
                            isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Delete project", role: .destructive) {
                if let deleting { onDelete(deleting) }
                deleting = nil
            }
            Button("Cancel", role: .cancel) { deleting = nil }
        } message: {
            Text("Its folder and every file in it are removed from this iPad. This cannot be undone.")
        }
    }

    private var layout: some View {
        GeometryReader { geometry in
            let size = geometry.size
            let short = min(size.width, size.height)
            let pad = min(max(short * 0.05, 18), 56)
            let title = min(max(short * 0.1, 40), 84)
            let landscape = size.width > size.height * 1.15
            Group {
                if landscape {
                    HStack(alignment: .top, spacing: pad) {
                        hero(title: title, rings: .below)
                            .frame(width: (size.width - pad * 3) * 0.42, alignment: .topLeading)
                            .frame(maxHeight: .infinity, alignment: .top)
                        projectsPanel
                    }
                } else {
                    VStack(alignment: .leading, spacing: pad * 0.8) {
                        hero(title: title, rings: size.width > 560 ? .beside : .none)
                        projectsPanel
                    }
                }
            }
            .padding(pad)
            .frame(width: size.width, height: size.height, alignment: .topLeading)
        }
    }

    // MARK: Hero

    private enum RingPlacement { case below, beside, none }

    private func hero(title: CGFloat, rings: RingPlacement) -> some View {
        VStack(alignment: .leading, spacing: title * 0.26) {
            HStack(alignment: .center, spacing: 0) {
                VStack(alignment: .leading, spacing: title * 0.22) {
                    Text("CorvCAD")
                        .font(.corvDisplay(title)).tracking(-title * 0.033)
                        .foregroundStyle(LinearGradient(colors: [.white, .white.opacity(0.7)],
                                                        startPoint: .top, endPoint: .bottom))
                        .lineLimit(1).minimumScaleFactor(0.6)
                    Text("Sketch a KiCad-compatible schematic and board by touch, then shape an enclosure around it.")
                        .font(.system(size: min(max(title * 0.25, 15), 21)))
                        .foregroundStyle(Corv.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: 520, alignment: .leading)
                }
                if rings == .beside {
                    Spacer(minLength: 16)
                    OrbitRings().frame(width: title * 2.4, height: title * 2.4)
                }
            }
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 10) { primaryAction; importActions }
                VStack(alignment: .leading, spacing: 10) {
                    primaryAction
                    HStack(spacing: 10) { importActions }
                }
                VStack(alignment: .leading, spacing: 10) { primaryAction; importActions }
            }
            HStack(spacing: 10) {
                Button(action: onGerbers) { Label("Gerber viewer", systemImage: "square.3.layers.3d.down.right") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("openGerberViewer")
                Button(action: onCalculator) { Label("PCB calculator", systemImage: "function") }
                    .buttonStyle(.corvGhost)
                    .accessibilityIdentifier("openCalculator")
            }
            if let resumeTitle { resume(resumeTitle) }
            if rings == .below {
                OrbitRings()
                    .aspectRatio(1, contentMode: .fit)
                    .frame(maxWidth: 340, maxHeight: .infinity)
                    .frame(maxWidth: .infinity)
                    .layoutPriority(-1)
            }
        }
    }

    private var primaryAction: some View {
        Button(action: onNew) { Label("New project", systemImage: "plus") }
            .buttonStyle(.corvPrimary)
    }

    @ViewBuilder private var importActions: some View {
        Button(action: onImportBoard) { Label("Import board", systemImage: "square.and.arrow.down") }
            .buttonStyle(.corvGhost)
        Button(action: onImportSchematic) { Label("Schematic", systemImage: "square.and.arrow.down") }
            .buttonStyle(.corvGhost)
        if let onImportOther {
            Button(action: onImportOther) { Label("Altium, Eagle…", systemImage: "arrow.down.doc") }
                .buttonStyle(.corvGhost)
                .accessibilityIdentifier("importOtherCAD")
        }
    }

    private func resume(_ title: String) -> some View {
        Button(action: onResume) {
            HStack(spacing: 14) {
                Circle().fill(Corv.gold).frame(width: 8, height: 8).shadow(color: Corv.gold, radius: 6)
                VStack(alignment: .leading, spacing: 3) {
                    MonoLabel("Continue", color: Corv.gold, small: true)
                    Text(title).font(.headline).foregroundStyle(Corv.text).lineLimit(1)
                }
                Spacer()
                Image(systemName: "arrow.right").foregroundStyle(Corv.gold)
            }
            .padding(.horizontal, 18).padding(.vertical, 14)
            .frame(maxWidth: 460)
            .corvPanel(radius: 18)
            .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(Corv.gold.opacity(0.35)))
        }
        .buttonStyle(CardPressStyle())
    }

    // MARK: Projects

    private var projectsPanel: some View {
        VStack(alignment: .leading, spacing: Self.gap) {
            HStack {
                Rectangle().fill(Corv.gold).frame(width: 10, height: 1.5)
                MonoLabel("Local projects", color: Corv.gold.opacity(0.9))
                Spacer()
                MonoLabel("\(projects.count)", color: Corv.textFaint)
            }
            GeometryReader { geometry in projectGrid(in: geometry.size) }
            HStack(spacing: 8) {
                Image(systemName: "folder").foregroundStyle(Corv.textFaint)
                Text("Project folders are also in the Files app, under On My iPad / CorvCAD / Projects.")
                    .font(.footnote).foregroundStyle(Corv.textFaint)
                    .lineLimit(1).minimumScaleFactor(0.75)
                Spacer(minLength: 8)
                Button { showLicenses = true } label: { MonoLabel("Licences", color: Corv.textFaint, small: true) }
                    .accessibilityIdentifier("licences")
            }
        }
        .sheet(isPresented: $showLicenses) { LicensesView { showLicenses = false } }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    @ViewBuilder private func projectActions(_ url: URL) -> some View {
        Button {
            newName = url.lastPathComponent
            renaming = url
        } label: { Label("Rename", systemImage: "character.cursor.ibeam") }
        Button(role: .destructive) { deleting = url } label: { Label("Delete project", systemImage: "trash") }
    }

    /// Fills the area with as many cards as fit at their minimum size. When projects
    /// overflow, the last slots become a "more" menu and the new-project card.
    private func projectGrid(in size: CGSize) -> some View {
        let gap = Self.gap
        let columns = max(1, Int((size.width + gap) / (Self.minCard.width + gap)))
        let rows = max(1, Int((size.height + gap) / (Self.minCard.height + gap)))
        let slots = columns * rows
        let overflow = projects.count + 1 > slots
        let shown = overflow ? Array(projects.prefix(max(0, slots - 2))) : projects
        let hidden = Array(projects.dropFirst(shown.count))
        let cellWidth = (size.width - gap * CGFloat(columns - 1)) / CGFloat(columns)
        let cellHeight = min((size.height - gap * CGFloat(rows - 1)) / CGFloat(rows), cellWidth * 0.95)

        return LazyVGrid(columns: Array(repeating: GridItem(.fixed(cellWidth), spacing: gap), count: columns),
                         alignment: .leading, spacing: gap) {
            ForEach(shown, id: \.path) { url in
                Button { onOpen(url) } label: { ProjectCard(url: url) }
                    .buttonStyle(CardPressStyle())
                    .frame(height: cellHeight)
                    .contextMenu { projectActions(url) }
            }
            if !hidden.isEmpty {
                Menu {
                    ForEach(hidden, id: \.path) { url in
                        Menu(url.lastPathComponent) {
                            Button("Open") { onOpen(url) }
                            projectActions(url)
                        }
                    }
                } label: {
                    VStack(spacing: 8) {
                        Text("+\(hidden.count)").font(.system(size: 28, weight: .semibold, design: .monospaced))
                            .foregroundStyle(Corv.cyan)
                        MonoLabel("More projects")
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .corvPanel(radius: 18)
                }
                .frame(height: cellHeight)
            }
            Button(action: onNew) {
                VStack(spacing: 10) {
                    Image(systemName: "plus").font(.system(size: 22, weight: .light))
                    MonoLabel(projects.isEmpty ? "Create your first project" : "New project")
                        .multilineTextAlignment(.center)
                }
                .foregroundStyle(Corv.textDim)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(RoundedRectangle(cornerRadius: 18)
                    .strokeBorder(Corv.lineStrong, style: StrokeStyle(lineWidth: 1, dash: [5, 5])))
                .contentShape(.rect)
            }
            .buttonStyle(CardPressStyle())
            .frame(height: cellHeight)
        }
    }
}
struct ProjectCard: View {
    let url: URL

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ZStack {
                LinearGradient(colors: [Corv.solderMask, Corv.void], startPoint: .topLeading, endPoint: .bottomTrailing)
                Canvas { context, size in
                    var grid = Path()
                    for x in stride(from: 0, through: size.width, by: 14) {
                        grid.move(to: CGPoint(x: x, y: 0)); grid.addLine(to: CGPoint(x: x, y: size.height))
                    }
                    for y in stride(from: 0, through: size.height, by: 14) {
                        grid.move(to: CGPoint(x: 0, y: y)); grid.addLine(to: CGPoint(x: size.width, y: y))
                    }
                    context.stroke(grid, with: .color(.white.opacity(0.05)), lineWidth: 0.5)
                }
                RoundedRectangle(cornerRadius: 6).strokeBorder(.white.opacity(0.8), lineWidth: 1.2)
                    .aspectRatio(1.5, contentMode: .fit)
                    .padding(18)
                    .frame(maxWidth: 110)
                Circle().fill(Corv.gold).frame(width: 6, height: 6).shadow(color: Corv.gold, radius: 6)
            }
            .frame(maxWidth: .infinity, minHeight: 56, maxHeight: .infinity)
            .clipShape(UnevenRoundedRectangle(topLeadingRadius: 17, topTrailingRadius: 17))
            VStack(alignment: .leading, spacing: 6) {
                Text(url.lastPathComponent).font(.headline).foregroundStyle(Corv.text).lineLimit(1)
                MonoLabel(modified, color: Corv.textFaint, small: true)
            }
            .padding(14)
        }
        .frame(maxHeight: .infinity)
        .corvPanel(radius: 18)
        .hoverEffect(.lift)
    }

    private var modified: String {
        let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
        return (ProjectEngines.load(url).box != nil ? "Enclosure" : "KiCad 10") + (date.map { " · " + $0.formatted(date: .abbreviated, time: .omitted) } ?? "")
    }
}

struct CardPressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .brightness(configuration.isPressed ? 0.04 : 0)
            .animation(.spring(response: 0.25, dampingFraction: 0.7), value: configuration.isPressed)
    }
}

/// One part of a new project, switched on or off by tapping; a locked card stays on.
private struct EngineCard: View {
    let title: String
    let engine: String
    let detail: String
    let icon: String
    @Binding var on: Bool
    var locked = false

    var body: some View {
        Button { if !locked { withAnimation(Corv.spring) { on.toggle() } } } label: {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Image(systemName: icon).font(.system(size: 18, weight: .medium))
                        .foregroundStyle(on ? Corv.gold : Corv.textFaint)
                    Spacer()
                    Image(systemName: locked ? "lock.fill" : on ? "checkmark.circle.fill" : "circle")
                        .font(.system(size: locked ? 13 : 18))
                        .foregroundStyle(on ? Corv.gold : Corv.textFaint)
                }
                Text(title).font(.system(size: 16, weight: .semibold)).foregroundStyle(on ? Corv.text : Corv.textDim)
                MonoLabel(engine, color: on ? Corv.cyan : Corv.textFaint, small: true)
                Text(detail).font(.caption).foregroundStyle(Corv.textDim).fixedSize(horizontal: false, vertical: true)
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .topLeading)
            .background(on ? Corv.gold.opacity(0.07) : Color.white.opacity(0.03), in: .rect(cornerRadius: 16))
            .overlay(RoundedRectangle(cornerRadius: 16).strokeBorder(on ? Corv.gold.opacity(0.55) : Corv.line))
            .contentShape(.rect(cornerRadius: 16))
        }
        .buttonStyle(.plain)
        .sensoryFeedback(.selection, trigger: on)
        .accessibilityLabel(title)
        .accessibilityValue(locked ? "Always on" : on ? "On" : "Off")
        .accessibilityAddTraits(on ? .isSelected : [])
        .accessibilityIdentifier("engine-\(title)")
    }
}

struct NewProjectSheet: View {
    @Binding var name: String
    @Binding var width: Double
    @Binding var height: Double
    @Binding var enclosure: Bool
    /// Off: a mechanical-only project (FreeCAD edition), an enclosure around a typed space.
    @Binding var electronics: Bool
    @Binding var boxHeight: Double
    let error: String?
    let onCancel: () -> Void
    let onCreate: () -> Void
    let onDismissError: () -> Void

    private var trimmed: String { name.trimmingCharacters(in: .whitespacesAndNewlines) }

    var body: some View {
        ScrollView {
            form.padding(28)
        }
        .scrollBounceBehavior(.basedOnSize)
        .background(VoidBackground(bloom: true))
        .presentationBackground(Corv.space)
        .presentationCornerRadius(28)
        .alert("Could not create project", isPresented: Binding(
            get: { error != nil },
            set: { if !$0 { onDismissError() } }
        )) {
            Button("OK", action: onDismissError)
        } message: { Text(error ?? "") }
    }

    private var form: some View {
        VStack(alignment: .leading, spacing: 24) {
            HStack {
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("New project", color: Corv.gold)
                    Text("Start a project").font(.corvDisplay(30)).foregroundStyle(Corv.text)
                }
                Spacer()
                Button("Cancel", action: onCancel).buttonStyle(.corvGhost)
            }
            VStack(alignment: .leading, spacing: 10) {
                MonoLabel("Parts", color: Corv.textFaint, small: true)
                HStack(alignment: .top, spacing: 12) {
                    EngineCard(title: "Electronics", engine: cc_board_native_backend() != nil ? "KiCad 10.0.6" : "KiCad 10 files",
                               detail: MechanicalSession.enabled ? "Schematic and board. Turn off for an enclosure on its own."
                                                                 : "Schematic and board. Required in this edition.",
                               icon: "cpu", on: $electronics, locked: !MechanicalSession.enabled)
                    EngineCard(title: "Enclosure", engine: MechanicalSession.enabled ? "FreeCAD" : "Parametric preview",
                               detail: electronics ? "A tray and lid shaped around the board. Can be added later from Fabricate."
                                                   : "A tray and lid around the space you type in. Required without electronics.",
                               icon: "cube.transparent", on: electronics ? $enclosure : .constant(true), locked: !electronics)
                }
            }
            preview
            VStack(alignment: .leading, spacing: 6) {
                MonoLabel("Name", color: Corv.textFaint, small: true)
                TextField("Sensor board", text: $name).autocorrectionDisabled().corvField()
                Text("Letters, numbers, spaces, hyphens or underscores.")
                    .font(.caption).foregroundStyle(Corv.textFaint)
            }
            HStack(spacing: 12) {
                NumberField(title: "Width mm", value: $width)
                NumberField(title: electronics ? "Height mm" : "Depth mm", value: $height)
                if !electronics { NumberField(title: "Inside height mm", value: $boxHeight) }
            }
            Text(electronics ? "Creates KiCad 10 schematic and board files with a rectangular Edge.Cuts outline and local part libraries."
                             : "Creates a FreeCAD enclosure whose inside is this size; add openings, vents, screws and sketches in Enclosure.")
                .font(.caption).foregroundStyle(Corv.textFaint)
            Button(action: onCreate) {
                Text("Create project").frame(maxWidth: .infinity)
            }
            .buttonStyle(.corvPrimary)
            .disabled(trimmed.isEmpty)
        }
    }

    /// Live outline preview scaled to the entered board size.
    private var preview: some View {
        let w = max(width, 1), h = max(height, 1)
        let fit = min(300 / w, 150 / h)
        return ZStack {
            RoundedRectangle(cornerRadius: 3)
                .fill(Corv.solderMask)
                .overlay(RoundedRectangle(cornerRadius: 3).strokeBorder(.white.opacity(0.9), lineWidth: 1.5))
                .shadow(color: Corv.cyan.opacity(0.25), radius: 12)
                .frame(width: w * fit, height: h * fit)
                .overlay(alignment: .bottom) {
                    MonoLabel(String(format: "%.0f mm", width), color: Corv.cyan, small: true).offset(y: 20)
                }
                .overlay(alignment: .trailing) {
                    MonoLabel(String(format: "%.0f mm", height), color: Corv.cyan, small: true)
                        .fixedSize().rotationEffect(.degrees(90)).offset(x: 30)
                }
        }
        .frame(maxWidth: .infinity, minHeight: 190)
        .animation(Corv.spring, value: width)
        .animation(Corv.spring, value: height)
    }
}

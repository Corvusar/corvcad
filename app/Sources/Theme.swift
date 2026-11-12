import SwiftUI

/// CorvCAD design tokens. Dark only: void black, white structure, gold for
/// action and selection, cyan for live data (snap, HUD, grid feedback).
enum Corv {
    static let void = Color(hex: 0x030303)
    static let space = Color(hex: 0x07070A)
    static let surface = Color(hex: 0x0E0E13)
    static let raised = Color(hex: 0x16161D)
    static let line = Color.white.opacity(0.08)
    static let lineStrong = Color.white.opacity(0.18)
    static let text = Color.white
    static let textDim = Color.white.opacity(0.62)
    static let textFaint = Color.white.opacity(0.38)
    static let gold = Color(hex: 0xFFB300)
    static let goldSoft = Color(hex: 0xFFD27A)
    static let cyan = Color(hex: 0x00F3FF)
    static let red = Color(hex: 0xFF3366)
    static let solderMask = Color(hex: 0x0A1A16)

    static let spring = Animation.spring(response: 0.38, dampingFraction: 0.82)
}

extension Color {
    init(hex: UInt32, opacity: Double = 1) {
        self.init(red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255, opacity: opacity)
    }
}

extension UIColor {
    convenience init(hex: UInt32) {
        self.init(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}

// MARK: - Type

extension Font {
    static func corvDisplay(_ size: CGFloat) -> Font { .system(size: size, weight: .semibold) }
    static let corvMono = Font.system(size: 11, weight: .medium, design: .monospaced)
    static let corvMonoSmall = Font.system(size: 9.5, weight: .medium, design: .monospaced)
    static let corvReadout = Font.system(size: 13, weight: .medium, design: .monospaced)
}

/// Uppercase monospaced technical label: the brand's HUD voice.
struct MonoLabel: View {
    let text: String
    var color: Color = Corv.textDim
    var small = false
    init(_ text: String, color: Color = Corv.textDim, small: Bool = false) {
        self.text = text; self.color = color; self.small = small
    }
    var body: some View {
        Text(text).font(small ? .corvMonoSmall : .corvMono)
            .tracking(small ? 1.2 : 1.6).textCase(.uppercase)
            .foregroundStyle(color)
    }
}

// MARK: - Surfaces

/// Floating glass panel over the void.
struct CorvPanel: ViewModifier {
    var radius: CGFloat = 18
    func body(content: Content) -> some View {
        content
            .background(.ultraThinMaterial, in: .rect(cornerRadius: radius))
            .background(Corv.space.opacity(0.72), in: .rect(cornerRadius: radius))
            .overlay(
                RoundedRectangle(cornerRadius: radius)
                    .strokeBorder(LinearGradient(colors: [Corv.lineStrong, Corv.line],
                                                 startPoint: .top, endPoint: .bottom), lineWidth: 1)
            )
            .shadow(color: .black.opacity(0.5), radius: 18, y: 8)
    }
}

extension View {
    func corvPanel(radius: CGFloat = 18) -> some View { modifier(CorvPanel(radius: radius)) }

    /// Dark inset field for text input.
    func corvField() -> some View {
        self.font(.corvReadout).foregroundStyle(Corv.text)
            .padding(.horizontal, 12).padding(.vertical, 10)
            .background(Corv.void.opacity(0.6), in: .rect(cornerRadius: 10))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
    }
}

struct CorvButtonStyle: ButtonStyle {
    enum Kind { case primary, ghost, danger }
    var kind = Kind.ghost
    @Environment(\.isEnabled) private var isEnabled

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 15, weight: .semibold))
            .padding(.horizontal, 18).padding(.vertical, 12)
            .foregroundStyle(kind == .primary ? Corv.void : kind == .danger ? Corv.red : Corv.text)
            .background {
                switch kind {
                case .primary:
                    Capsule().fill(Corv.gold)
                        .shadow(color: Corv.gold.opacity(!isEnabled ? 0 : configuration.isPressed ? 0.2 : 0.45), radius: 14)
                case .ghost, .danger:
                    Capsule().fill(Color.white.opacity(configuration.isPressed ? 0.1 : 0.04))
                        .overlay(Capsule().strokeBorder(kind == .danger ? Corv.red.opacity(0.5) : Corv.lineStrong))
                }
            }
            .opacity(isEnabled ? 1 : 0.38)
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .animation(.easeOut(duration: 0.15), value: configuration.isPressed)
    }
}

extension ButtonStyle where Self == CorvButtonStyle {
    static var corvPrimary: CorvButtonStyle { CorvButtonStyle(kind: .primary) }
    static var corvGhost: CorvButtonStyle { CorvButtonStyle(kind: .ghost) }
    static var corvDanger: CorvButtonStyle { CorvButtonStyle(kind: .danger) }
}

/// Void background with a faint gold bloom and a seeded starfield.
struct VoidBackground: View {
    var bloom = true
    var body: some View {
        ZStack {
            Corv.void
            if bloom {
                RadialGradient(colors: [Corv.gold.opacity(0.10), .clear],
                               center: .init(x: 0.78, y: 0.18), startRadius: 0, endRadius: 520)
                RadialGradient(colors: [Corv.cyan.opacity(0.05), .clear],
                               center: .init(x: 0.1, y: 0.95), startRadius: 0, endRadius: 600)
            }
            Canvas { context, size in
                var seed: UInt64 = 0x9E3779B97F4A7C15
                func next() -> CGFloat {
                    seed = seed &* 6364136223846793005 &+ 1442695040888963407
                    return CGFloat(seed >> 33) / CGFloat(1 << 31)
                }
                for _ in 0..<140 {
                    let point = CGPoint(x: next() * size.width, y: next() * size.height)
                    let radius = 0.4 + next() * 0.9
                    context.fill(Path(ellipseIn: CGRect(x: point.x, y: point.y, width: radius, height: radius)),
                                 with: .color(.white.opacity(0.15 + next() * 0.35)))
                }
            }
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }
}

/// Decorative orbital rings: a slow dotted orbit around a glowing gold core.
struct OrbitRings: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var spin = false

    var body: some View {
        ZStack {
            Circle().stroke(Corv.lineStrong, lineWidth: 1)
            Circle().stroke(Corv.line, lineWidth: 1).scaleEffect(0.72)
            Circle().trim(from: 0.05, to: 0.45)
                .stroke(Corv.gold.opacity(0.85), style: StrokeStyle(lineWidth: 1.5, lineCap: .round))
                .scaleEffect(0.86)
                .rotationEffect(.degrees(spin ? 360 : 0))
            Circle().trim(from: 0.55, to: 0.95)
                .stroke(Corv.gold.opacity(0.6), style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [0.1, 9]))
                .scaleEffect(0.86)
                .rotationEffect(.degrees(spin ? -360 : 0))
            Rectangle().fill(LinearGradient(colors: [.clear, Corv.gold.opacity(0.5), .clear],
                                            startPoint: .leading, endPoint: .trailing))
                .frame(height: 1)
            Circle().fill(Corv.gold).frame(width: 14, height: 14)
                .shadow(color: Corv.gold, radius: 12).shadow(color: Corv.gold.opacity(0.6), radius: 30)
        }
        .animation(reduceMotion ? nil : .linear(duration: 90).repeatForever(autoreverses: false), value: spin)
        .onAppear { spin = true }
        .accessibilityHidden(true)
    }
}

// MARK: - Workspace chrome

/// Vertical floating tool rail with a sliding gold selection.
struct ToolRail<Tool: Hashable>: View {
    struct Item { let tool: Tool; let title: String; let icon: String }
    let items: [Item]
    @Binding var selection: Tool
    var disabled = false
    @Namespace private var highlight

    var body: some View {
        VStack(spacing: 6) {
            ForEach(items, id: \.tool) { item in
                let selected = item.tool == selection
                Button {
                    withAnimation(Corv.spring) { selection = item.tool }
                } label: {
                    VStack(spacing: 4) {
                        Image(systemName: item.icon).font(.system(size: 18, weight: .medium))
                        Text(item.title).font(.corvMonoSmall).tracking(0.8).textCase(.uppercase)
                    }
                    .frame(width: 58, height: 54)
                    .foregroundStyle(selected ? Corv.void : Corv.textDim)
                    .background {
                        if selected {
                            RoundedRectangle(cornerRadius: 13).fill(Corv.gold)
                                .shadow(color: Corv.gold.opacity(0.45), radius: 10)
                                .matchedGeometryEffect(id: "tool", in: highlight)
                        }
                    }
                    .contentShape(.rect)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(item.title)
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
        .padding(6)
        .corvPanel(radius: 19)
        .opacity(disabled ? 0.4 : 1)
        .disabled(disabled)
        .sensoryFeedback(.selection, trigger: selection)
    }
}

/// Bottom heads-up readout: status message plus live millimetre coordinates.
struct HUDBar: View {
    let status: String
    let coordinate: CGPoint?
    var snap: String?
    var snapColor: Color = Corv.cyan

    var body: some View {
        HStack(spacing: 14) {
            Circle().fill(Corv.gold).frame(width: 6, height: 6).shadow(color: Corv.gold, radius: 4)
            Text(status).font(.footnote).foregroundStyle(Corv.textDim).lineLimit(1)
            if let coordinate {
                Divider().frame(height: 14).overlay(Corv.lineStrong)
                readout("X", coordinate.x)
                readout("Y", coordinate.y)
                if let snap { MonoLabel(snap, color: snapColor, small: true) }
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .corvPanel(radius: 14)
        .animation(.easeOut(duration: 0.12), value: coordinate == nil)
    }

    private func readout(_ axis: String, _ value: CGFloat) -> some View {
        HStack(spacing: 5) {
            MonoLabel(axis, color: Corv.textFaint, small: true)
            Text(String(format: "%7.2f", value)).font(.corvReadout).foregroundStyle(Corv.cyan)
                .monospacedDigit()
        }
    }
}

/// Small floating capsule used for contextual inputs and actions above the canvas.
struct ChipBar<Content: View>: View {
    @ViewBuilder let content: Content
    var body: some View {
        HStack(spacing: 10) { content }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .corvPanel(radius: 16)
    }
}

// MARK: - Canvas drawing

extension GraphicsContext {
    /// Crosshair reticle echoing the brand mark: ring, dotted orbit, beam ticks, core.
    func reticle(at center: CGPoint, color: Color, label: String?) {
        let ring = Path(ellipseIn: CGRect(x: center.x - 11, y: center.y - 11, width: 22, height: 22))
        stroke(ring, with: .color(color), lineWidth: 1.5)
        let orbit = Path(ellipseIn: CGRect(x: center.x - 19, y: center.y - 19, width: 38, height: 38))
        stroke(orbit, with: .color(color.opacity(0.55)), style: StrokeStyle(lineWidth: 1.5, lineCap: .round, dash: [0.1, 5]))
        var ticks = Path()
        for (dx, dy) in [(1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0)] {
            ticks.move(to: CGPoint(x: center.x + dx * 15, y: center.y + dy * 15))
            ticks.addLine(to: CGPoint(x: center.x + dx * 26, y: center.y + dy * 26))
        }
        stroke(ticks, with: .color(color), lineWidth: 1)
        fill(Path(ellipseIn: CGRect(x: center.x - 2.5, y: center.y - 2.5, width: 5, height: 5)), with: .color(color))
        if let label {
            draw(Text(label.uppercased()).font(.corvMonoSmall).tracking(1).foregroundStyle(color),
                 at: CGPoint(x: center.x + 24, y: center.y - 20), anchor: .bottomLeading)
        }
    }

    /// Gold selection halo.
    func halo(at center: CGPoint, radius: CGFloat) {
        let rect = CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)
        var glow = self
        glow.addFilter(.blur(radius: 8))
        glow.stroke(Path(ellipseIn: rect), with: .color(Corv.gold.opacity(0.7)), lineWidth: 4)
        stroke(Path(ellipseIn: rect), with: .color(Corv.gold), lineWidth: 1.5)
    }

    func dot(_ point: CGPoint, radius: CGFloat, color: Color) {
        fill(Path(ellipseIn: CGRect(x: point.x - radius, y: point.y - radius,
                                    width: radius * 2, height: radius * 2)), with: .color(color))
    }
}

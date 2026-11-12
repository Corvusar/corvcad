import SwiftUI
import UIKit

/// A canvas gesture surface that keeps the press location and a live finger preview.
/// After a hold fires, the same touch keeps reporting through `onHoldDrag`/`onHoldEnd`,
/// so press → hold → slide to an orbit item → release is one gesture.
struct TouchCanvasSurface: UIViewRepresentable {
    let onTap: (CGPoint) -> Void
    let onHold: (CGPoint) -> Void
    let onPreview: (CGPoint?) -> Void
    var onHoldDrag: (CGPoint) -> Void = { _ in }
    var onHoldEnd: (CGPoint) -> Void = { _ in }
    /// Two-finger view changes: a zoom factor about an anchor, and a pan, both incremental.
    var onZoom: (CGFloat, CGPoint) -> Void = { _, _ in }
    var onPan: (CGSize) -> Void = { _ in }

    func makeUIView(context: Context) -> TouchCaptureView {
        let view = TouchCaptureView()
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isMultipleTouchEnabled = true
        // VoiceOver passes touches straight through to the drawing surface; UI tests find it by id.
        view.isAccessibilityElement = true
        view.accessibilityLabel = "Drawing canvas"
        view.accessibilityTraits = .allowsDirectInteraction
        view.accessibilityIdentifier = "canvas"
        let pinch = UIPinchGestureRecognizer(target: view, action: #selector(TouchCaptureView.pinched(_:)))
        let pan = UIPanGestureRecognizer(target: view, action: #selector(TouchCaptureView.panned(_:)))
        pan.minimumNumberOfTouches = 2
        for recognizer in [pinch, pan] as [UIGestureRecognizer] {
            recognizer.delegate = view
            view.addGestureRecognizer(recognizer)
        }
        return view
    }

    func updateUIView(_ view: TouchCaptureView, context: Context) {
        view.onTap = onTap
        view.onHold = onHold
        view.onPreview = onPreview
        view.onHoldDrag = onHoldDrag
        view.onHoldEnd = onHoldEnd
        view.onZoom = onZoom
        view.onPan = onPan
    }
}

final class TouchCaptureView: UIView, UIGestureRecognizerDelegate {
    var onTap: ((CGPoint) -> Void)?
    var onHold: ((CGPoint) -> Void)?
    var onPreview: ((CGPoint?) -> Void)?
    var onHoldDrag: ((CGPoint) -> Void)?
    var onHoldEnd: ((CGPoint) -> Void)?
    var onZoom: ((CGFloat, CGPoint) -> Void)?
    var onPan: ((CGSize) -> Void)?
    private var holdTimer: Timer?
    private var start: CGPoint?
    private var held = false

    // Recognized pinches and pans cancel the single-finger touch (cancelsTouchesInView).
    @objc func pinched(_ pinch: UIPinchGestureRecognizer) {
        guard pinch.state == .changed else { return }
        onZoom?(pinch.scale, pinch.location(in: self))
        pinch.scale = 1
    }

    @objc func panned(_ pan: UIPanGestureRecognizer) {
        guard pan.state == .changed else { return }
        let moved = pan.translation(in: self)
        onPan?(CGSize(width: moved.x, height: moved.y))
        pan.setTranslation(.zero, in: self)
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }

    private func cancelTimer() {
        holdTimer?.invalidate()
        holdTimer = nil
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        // A second finger means pan or zoom, never a tap or hold.
        if (event?.allTouches?.count ?? 1) > 1 {
            if !held { touchesCancelled(touches, with: event) }
            return
        }
        guard let touch = touches.first else { return }
        let location = touch.location(in: self)
        start = location
        held = false
        onPreview?(location)
        cancelTimer()
        holdTimer = Timer.scheduledTimer(withTimeInterval: 0.42, repeats: false) { [weak self] _ in
            guard let self, let start = self.start else { return }
            self.held = true
            UIImpactFeedbackGenerator(style: .medium).impactOccurred()
            self.onPreview?(nil)
            self.onHold?(start)
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first, let start else { return }
        let location = touch.location(in: self)
        if held { onHoldDrag?(location); return }
        if hypot(location.x - start.x, location.y - start.y) > 12 { cancelTimer() }
        onPreview?(location)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        cancelTimer()
        if let touch = touches.first, start != nil {
            let location = touch.location(in: self)
            if held { onHoldEnd?(location) } else { onTap?(location) }
        }
        start = nil
        held = false
        onPreview?(nil)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        cancelTimer()
        start = nil
        held = false
        onPreview?(nil)
    }
}

/// Two-finger view changes on a millimetre viewport drawn aspect-fit and centred in a canvas.
enum Viewport {
    /// Zooms about `anchor` (millimetres), which stays under the fingers; 5 mm to 5 m wide.
    static func zoom(_ rect: CGRect, by factor: CGFloat, about anchor: CGPoint) -> CGRect {
        guard factor > 0, rect.width > 0 else { return rect }
        let width = min(max(rect.width / factor, 5), 5000)
        let applied = rect.width / width
        return CGRect(x: anchor.x - (anchor.x - rect.minX) / applied,
                      y: anchor.y - (anchor.y - rect.minY) / applied,
                      width: width, height: rect.height / applied)
    }

    /// Moves the content with the fingers by `moved` screen points.
    static func pan(_ rect: CGRect, by moved: CGSize, in size: CGSize) -> CGRect {
        let scale = min(size.width / rect.width, size.height / rect.height)
        guard scale > 0 else { return rect }
        return rect.offsetBy(dx: -moved.width / scale, dy: -moved.height / scale)
    }
}

/// Hold menu styled after the brand mark: a glowing core naming the item, with
/// action nodes on an upper orbit. Tap a node, or keep the finger down after the
/// hold and release over one. Tapping outside closes it.
struct OrbitMenu: View {
    struct Item: Identifiable {
        let id: String
        let icon: String
        var destructive = false
        let perform: () -> Void
    }

    let title: String
    let items: [Item]
    /// Index under the held finger, driven by the canvas while dragging.
    var highlighted: Int?

    static let radius: CGFloat = 88
    static let nodeSize: CGFloat = 54
    /// Space the menu needs around its centre, for clamping inside the canvas.
    static let clearance: CGFloat = radius + nodeSize / 2 + 26

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var appeared = false

    /// Node offsets from the core, fanned across the upper arc.
    static func offset(_ index: Int, of count: Int) -> CGSize {
        let spread = min(Double.pi * 1.1, Double(max(count - 1, 1)) * 0.95)
        let angle = -Double.pi / 2 - spread / 2 + spread * Double(index) / Double(max(count - 1, 1))
        let a = count == 1 ? -Double.pi / 2 : angle
        return CGSize(width: CGFloat(cos(a)) * radius, height: CGFloat(sin(a)) * radius)
    }

    /// Keeps the menu's upper orbit and labels inside a canvas of `size`.
    static func clamped(_ point: CGPoint, in size: CGSize) -> CGPoint {
        let side = radius + nodeSize / 2 + 34, top = radius + nodeSize / 2 + 10, bottom: CGFloat = 70
        return CGPoint(x: min(max(point.x, side), max(side, size.width - side)),
                       y: min(max(point.y, top), max(top, size.height - bottom)))
    }

    /// The node under `point`, given in the same space as `center`.
    static func item(at point: CGPoint, center: CGPoint, count: Int) -> Int? {
        (0..<count).min { a, b in
            distance(point, center, offset(a, of: count)) < distance(point, center, offset(b, of: count))
        }.flatMap { distance(point, center, offset($0, of: count)) < nodeSize * 0.8 ? $0 : nil }
    }

    private static func distance(_ point: CGPoint, _ center: CGPoint, _ offset: CGSize) -> CGFloat {
        hypot(point.x - center.x - offset.width, point.y - center.y - offset.height)
    }

    var body: some View {
        ZStack {
            ZStack {
                Circle().fill(RadialGradient(colors: [Corv.void.opacity(0.85), Corv.void.opacity(0)],
                                             center: .center, startRadius: 30, endRadius: Self.radius + 50))
                    .frame(width: (Self.radius + 50) * 2, height: (Self.radius + 50) * 2)
                Circle().stroke(Corv.lineStrong, lineWidth: 1)
                    .frame(width: Self.radius * 2, height: Self.radius * 2)
                Circle().trim(from: 0.55, to: 0.95)
                    .stroke(Corv.gold.opacity(0.7), style: StrokeStyle(lineWidth: 2, lineCap: .round, dash: [0.1, 7]))
                    .frame(width: Self.radius * 2 - 28, height: Self.radius * 2 - 28)
                    .rotationEffect(.degrees(appeared ? 0 : -50))
                Rectangle().fill(LinearGradient(colors: [.clear, Corv.gold.opacity(0.5), .clear],
                                                startPoint: .leading, endPoint: .trailing))
                    .frame(width: Self.radius * 2 + 30, height: 1)
            }
            .allowsHitTesting(false)
            core
            ForEach(Array(items.enumerated()), id: \.element.id) { index, item in
                node(item, active: highlighted == index)
                    .offset(appeared ? Self.offset(index, of: items.count) : .zero)
                    .scaleEffect(appeared ? 1 : 0.3)
                    .opacity(appeared ? 1 : 0)
            }
        }
        .frame(width: Self.clearance * 2, height: Self.clearance * 2)
        .onAppear {
            withAnimation(reduceMotion ? .easeOut(duration: 0.12) : .spring(response: 0.34, dampingFraction: 0.72)) {
                appeared = true
            }
        }
        .sensoryFeedback(.selection, trigger: highlighted)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Actions for \(title)")
    }

    private var core: some View {
        VStack(spacing: 2) {
            Circle().fill(Corv.gold).frame(width: 8, height: 8)
                .shadow(color: Corv.gold, radius: 6)
            Text(title).font(.system(size: 13, weight: .semibold)).foregroundStyle(Corv.text)
                .lineLimit(1).frame(maxWidth: 76)
        }
        .frame(width: 92, height: 92)
        .background(Circle().fill(Corv.space))
        .overlay(Circle().strokeBorder(Corv.gold.opacity(0.6), lineWidth: 1.5))
        .shadow(color: Corv.gold.opacity(0.35), radius: 16)
        .allowsHitTesting(false)
    }

    private func node(_ item: Item, active: Bool) -> some View {
        let tint = item.destructive ? Corv.red : Corv.gold
        return Button(action: item.perform) {
            Image(systemName: item.icon)
                .font(.system(size: 19, weight: .semibold))
                .foregroundStyle(active ? Corv.void : item.destructive ? Corv.red : Corv.text)
                .frame(width: Self.nodeSize, height: Self.nodeSize)
                .background(Circle().fill(active ? AnyShapeStyle(tint) : AnyShapeStyle(Corv.raised)))
                .overlay(Circle().strokeBorder(tint.opacity(active ? 1 : 0.45), lineWidth: 1.2))
                .shadow(color: tint.opacity(active ? 0.7 : 0), radius: 14)
                .scaleEffect(active ? 1.14 : 1)
                .overlay(alignment: .bottom) {
                    MonoLabel(item.id, color: active ? tint : Corv.textDim, small: true)
                        .fixedSize().offset(y: 18)
                }
        }
        .buttonStyle(.plain)
        .animation(.spring(response: 0.25, dampingFraction: 0.7), value: active)
        .accessibilityLabel(item.id)
    }
}

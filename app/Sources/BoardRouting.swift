import SwiftUI

/// A route in progress with KiCad's interactive router (NATIVE_KICAD edition): the router's
/// preview after the last finger move, in board millimetres.
struct BoardRoute {
    struct Track {
        let layer: String
        let width: Double
        let points: [CGPoint]
    }

    var tracks: [Track] = []
    var vias: [(center: CGPoint, diameter: Double)] = []
    /// Board tracks and vias the preview replaces (shoved), by UUID.
    var hidden: Set<String> = []
    var end: CGPoint?
    var layer = "F.Cu"
    var placingVia = false
    /// Why the router could not follow or fix, shown in the HUD.
    var message: String?
    /// Length tuning: the tuned length (or skew) in millimetres and "short", "long" or "tuned".
    var tuning: (length: Double, status: String)?
    var dragging = false

    /// cc_board_native_route_move's lines.
    mutating func read(_ text: String) {
        tracks = []; vias = []; hidden = []; message = nil
        for line in text.split(separator: "\n") {
            let fields = line.split(separator: " ")
            guard let kind = fields.first else { continue }
            let numbers = fields.dropFirst().compactMap { Double($0) }
            switch kind {
            case "end" where fields.count >= 4 && numbers.count >= 2:
                end = CGPoint(x: numbers[0], y: numbers[1])
                layer = String(fields[3])
            case "fail":
                message = fields.dropFirst().joined(separator: " ")
            case "track" where fields.count >= 7:
                let values = fields.dropFirst(2).compactMap { Double($0) }
                guard values.count >= 5 else { continue }
                let points = stride(from: 1, to: values.count - 1, by: 2).map { CGPoint(x: values[$0], y: values[$0 + 1]) }
                tracks.append(Track(layer: String(fields[1]), width: values[0], points: points))
            case "via" where numbers.count >= 3:
                vias.append((CGPoint(x: numbers[0], y: numbers[1]), numbers[2]))
            case "hide" where fields.count >= 2:
                hidden.insert(String(fields[1]))
            case "tuning" where fields.count >= 3 && !numbers.isEmpty:
                tuning = (numbers[0], String(fields[2]))
            default:
                continue
            }
        }
    }
}

/// What the Track tool routes: KiCad's Route Single Track, Route Differential Pair, Tune
/// Length, Tune Differential Pair Length, Tune Skew, or a drag of a track or via.
enum RouteKind: Int, CaseIterable {
    case track, pair, tune, tunePair, skew, drag

    var title: String {
        ["Track", "Pair", "Tune", "Tune pair", "Skew", "Drag"][rawValue]
    }

    /// Tuning kinds need a target (length, or skew for .skew) in millimetres.
    var tunes: Bool { self == .tune || self == .tunePair || self == .skew }
}

/// What the Track tool asks of the router.
enum RouteAction {
    /// Start at a point on a layer, in a mode (0 highlight collisions, 1 shove, 2 walk around),
    /// of a kind, with the tuning target in millimetres.
    case start(CGPoint, layer: String, mode: Int, kind: RouteKind, target: Double)
    case move(CGPoint)
    case fix(CGPoint)
    case toggleVia
    case finish
    case cancel
}

extension Design {
    /// KiCad's router runs where KiCad's board code is linked (NATIVE_KICAD).
    static var hasKiCadRouter: Bool { cc_board_native_backend() != nil }

    /// Drives KiCad's router on the project board; a finished route is saved as one undoable step.
    /// ponytail: main thread, like DRC; KiCad's router moves in milliseconds on demo boards.
    @MainActor
    func route(_ action: RouteAction, board url: URL) {
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let message = { String(cString: buffer) }
        switch action {
        case let .start(point, layer, mode, kind, target):
            let started = kind == .drag
                ? cc_board_native_drag_start(url.path, point.x, point.y, Int32(mode), &buffer, capacity)
                : cc_board_native_route_start(url.path, point.x, point.y, layer, Int32(mode), Int32(kind.rawValue),
                                              target, &buffer, capacity)
            if started == 1 {
                routing = BoardRoute(layer: layer, dragging: kind == .drag)
            } else {
                routing = nil
                error = message()
            }
        case let .move(point):
            guard routing != nil else { return }
            guard let text = cc_board_native_route_move(point.x, point.y, &buffer, capacity) else {
                routing?.message = message()
                return
            }
            defer { cc_free_text(text) }
            routing?.read(String(cString: text))
        case let .fix(point):
            guard routing != nil else { return }
            switch cc_board_native_route_fix(point.x, point.y, &buffer, capacity) {
            case 1: route(.finish, board: url)
            case 0:
                routing?.placingVia = false
                route(.move(point), board: url)
            default: routing?.message = message()
            }
        case .toggleVia:
            routing?.placingVia = cc_board_native_route_toggle_via() == 1
        case .finish:
            let text = cc_board_native_route_finish(&buffer, capacity)
            routing = nil
            // Nothing fixed: nothing to save, and no error worth showing.
            if text != nil { saveAuthored(text, errorBuffer: buffer, to: url) }
        case .cancel:
            cc_board_native_route_cancel()
            routing = nil
        }
    }
}

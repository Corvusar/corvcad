import CryptoKit
import Foundation

/// One PartDesign feature of the enclosure document, as the session reports it.
struct MechanicalFeature: Decodable, Identifiable, Equatable {
    let body: String
    let name: String
    let type: String
    let label: String
    let suppressed: Bool
    let profile: String?
    let expressions: [String: String]
    var id: String { name }
}

/// A box through one side wall of the tray (session "cutouts"), board millimetres. `side` is the
/// board edge in the PCB view: front = top edge (y min), back = bottom edge, left, right; `center`
/// runs along that edge.
struct WallCutout: Codable, Equatable {
    var side: String
    var center: Double
    var width: Double
    var bottom: Double
    var height: Double

    var edgeName: String {
        ["front": "Top edge", "back": "Bottom edge", "left": "Left edge", "right": "Right edge"][side] ?? side
    }
}

/// A row of vent slots (session "openings"), board millimetres. On the lid: slots along y,
/// `center` the row's x and `across` its y. On a side wall: vertical slots, `center` along the
/// wall and `across` their bottom, `length` their height. `count` slots `pitch` apart.
struct VentRow: Codable, Equatable {
    var face: String
    var center: Double
    var across: Double
    var count: Int
    var width: Double
    var length: Double
    var pitch: Double

    static let faceNames = ["lid": "Lid", "front": "Top wall", "back": "Bottom wall", "left": "Left wall", "right": "Right wall"]
    var faceName: String { Self.faceNames[face] ?? face }
}

/// Lid screws: corner posts with pilot (or heat-set insert) holes and lid clearance holes.
struct LidScrews: Codable, Equatable {
    var screw: String
    var insert: Bool
    /// The session's key for this size's post (the board margin it needs).
    var postKey: String { insert ? "\(screw)-insert" : screw }
}

/// A closed sketch profile in face millimetres (session "sketches"): a rectangle by its low corner,
/// a circle by its centre, a polygon by its corners and, per corner, the bulge of the edge to the
/// next corner (0: straight; else an arc, tan(sweep / 4), positive counterclockwise, at most a half
/// circle; the session's convention). Face coordinates: the lid's are board x, y;
/// the front and back walls' x, z; the left and right walls' y, z (z up from the tray's base).
enum SketchShape: Equatable {
    case rect(x: Double, y: Double, width: Double, height: Double)
    case circle(x: Double, y: Double, diameter: Double)
    case polygon([CGPoint], [Double])

    /// (u0, v0, u1, v1): the shape's box in face coordinates (arcs from their chords, for display).
    var extent: CGRect {
        switch self {
        case let .rect(x, y, width, height): return CGRect(x: x, y: y, width: width, height: height)
        case let .circle(x, y, diameter): return CGRect(x: x - diameter / 2, y: y - diameter / 2, width: diameter, height: diameter)
        case let .polygon(points, bulges):
            let outline = Self.flatten(points, bulges)
            let xs = outline.map(\.x), ys = outline.map(\.y)
            return CGRect(x: xs.min() ?? 0, y: ys.min() ?? 0, width: (xs.max() ?? 0) - (xs.min() ?? 0),
                          height: (ys.max() ?? 0) - (ys.min() ?? 0))
        }
    }

    func moved(by delta: CGSize) -> SketchShape {
        switch self {
        case let .rect(x, y, width, height): return .rect(x: x + delta.width, y: y + delta.height, width: width, height: height)
        case let .circle(x, y, diameter): return .circle(x: x + delta.width, y: y + delta.height, diameter: diameter)
        case let .polygon(points, bulges):
            return .polygon(points.map { CGPoint(x: $0.x + delta.width, y: $0.y + delta.height) }, bulges)
        }
    }

    /// The arc from `a` to `b` with `bulge` (session inputs.arc): centre, radius, start angle, sweep.
    static func arc(_ a: CGPoint, _ b: CGPoint, _ bulge: Double) -> (center: CGPoint, radius: Double, start: Double, sweep: Double) {
        let dx = Double(b.x - a.x), dy = Double(b.y - a.y), chord = hypot(dx, dy)
        let offset = chord * (1 - bulge * bulge) / (4 * bulge)
        let center = CGPoint(x: Double(a.x + b.x) / 2 - dy / chord * offset, y: Double(a.y + b.y) / 2 + dx / chord * offset)
        return (center, chord * (1 + bulge * bulge) / (4 * abs(bulge)), Double(atan2(a.y - center.y, a.x - center.x)), 4 * atan(bulge))
    }

    /// The edge from `a` toward `b`: `a`, then points along its arc (none when straight); `b` excluded.
    static func edge(_ a: CGPoint, _ b: CGPoint, _ bulge: Double) -> [CGPoint] {
        guard bulge != 0, a != b else { return [a] }
        let (center, radius, start, sweep) = arc(a, b, bulge)
        let count = max(2, Int((abs(sweep) / (.pi / 32)).rounded(.up)))
        return [a] + (1..<count).map { k in
            let angle = start + sweep * Double(k) / Double(count)
            return CGPoint(x: Double(center.x) + radius * cos(angle), y: Double(center.y) + radius * sin(angle))
        }
    }

    static func flatten(_ points: [CGPoint], _ bulges: [Double]) -> [CGPoint] {
        points.indices.flatMap { edge(points[$0], points[($0 + 1) % points.count], bulges[$0]) }
    }
}

extension SketchShape: Codable {
    /// Corners as the session writes them: [u, v], or [u, v, bulge] before an arc.
    static func corners(_ points: [CGPoint], _ bulges: [Double]) -> [[Double]] {
        points.indices.map { bulges[$0] == 0 ? [points[$0].x, points[$0].y] : [points[$0].x, points[$0].y, bulges[$0]] }
    }

    private enum Keys: String, CodingKey { case kind, x, y, width, height, diameter, points }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: Keys.self)
        switch try c.decode(String.self, forKey: .kind) {
        case "rect":
            self = .rect(x: try c.decode(Double.self, forKey: .x), y: try c.decode(Double.self, forKey: .y),
                         width: try c.decode(Double.self, forKey: .width), height: try c.decode(Double.self, forKey: .height))
        case "circle":
            self = .circle(x: try c.decode(Double.self, forKey: .x), y: try c.decode(Double.self, forKey: .y),
                           diameter: try c.decode(Double.self, forKey: .diameter))
        default:
            let corners = try c.decode([[Double]].self, forKey: .points)  // [u, v] or [u, v, bulge]
            guard corners.allSatisfy({ $0.count == 2 || $0.count == 3 }) else {
                throw DecodingError.dataCorruptedError(forKey: .points, in: c, debugDescription: "Expected [u, v] or [u, v, bulge]")
            }
            self = .polygon(corners.map { CGPoint(x: $0[0], y: $0[1]) }, corners.map { $0.count == 3 ? $0[2] : 0 })
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: Keys.self)
        switch self {
        case let .rect(x, y, width, height):
            try c.encode("rect", forKey: .kind)
            try c.encode(x, forKey: .x); try c.encode(y, forKey: .y)
            try c.encode(width, forKey: .width); try c.encode(height, forKey: .height)
        case let .circle(x, y, diameter):
            try c.encode("circle", forKey: .kind)
            try c.encode(x, forKey: .x); try c.encode(y, forKey: .y); try c.encode(diameter, forKey: .diameter)
        case let .polygon(points, bulges):
            try c.encode("polygon", forKey: .kind)
            try c.encode(Self.corners(points, bulges), forKey: .points)
        }
    }
}

/// A sketch on the lid's top face or a side wall's outer face, extruded outward or cut inward by
/// `depth` (nil for a cut: through the lid or wall). Each is a FreeCAD Sketcher sketch and a Pad
/// or Pocket.
struct UserSketch: Codable, Equatable {
    var face: String
    var operation: String
    var depth: Double?
    var shapes: [SketchShape]

    var cut: Bool { operation == "cut" }
    var faceName: String { VentRow.faceNames[face] ?? face }

    private enum CodingKeys: String, CodingKey { case face, operation, depth, shapes }
    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(face, forKey: .face)
        try c.encode(operation, forKey: .operation)
        try c.encode(depth, forKey: .depth)  // null: through
        try c.encode(shapes, forKey: .shapes)
    }
}

/// Only C values cross this boundary. FreeCAD and Python remain process-scoped in the adapter.
final class MechanicalSession {
    static var enabled: Bool {
        #if NATIVE_FREECAD
        true
        #else
        false
        #endif
    }

    let documentURL: URL
    private let handle: OpaquePointer
    #if CORVCAD_DIAGNOSTICS
    private(set) static var diagnosticRuntimeInitialized = false
    #endif

    init(project: URL, input: MechanicalBoardInput, initial: cc_settings) throws {
        guard Self.enabled, cc_mechanical_available() == 1 else {
            throw DesignError.message("This edition does not include the enclosure engine.")
        }
        guard let runtime = Bundle.main.resourceURL?.appendingPathComponent("FreeCADRuntime"),
              FileManager.default.fileExists(atPath: runtime.appendingPathComponent("harness").path) else {
            throw DesignError.message("The bundled enclosure resources are missing.")
        }
        let user = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("FreeCAD", isDirectory: true)
        try FileManager.default.createDirectory(at: user, withIntermediateDirectories: true)
        try Self.checked { error, capacity in
            runtime.path.withCString { root in
                user.path.withCString { cc_mechanical_initialize(root, $0, error, capacity) }
            }
        }
        #if CORVCAD_DIAGNOSTICS
        Self.diagnosticRuntimeInitialized = true
        #endif
        let document = project.appendingPathComponent("enclosure.FCStd")
        documentURL = document
        var message = [CChar](repeating: 0, count: 4096)
        let capacity = message.count
        let opened = document.path.withCString { path in
            input.json.withCString { cc_mechanical_open(path, $0, initial, &message, capacity) }
        }
        guard let opened else { throw Self.failure(message) }
        handle = opened
    }

    deinit { cc_mechanical_close(handle) }

    var settings: cc_settings {
        get throws {
            var value = cc_default_settings()
            try Self.checked { cc_mechanical_get_settings(handle, &value, $0, $1) }
            return value
        }
    }

    var canUndo: Bool { cc_mechanical_can_undo(handle) != 0 }
    var canRedo: Bool { cc_mechanical_can_redo(handle) != 0 }

    func set(_ name: String, value: Double) throws {
        try Self.checked { error, capacity in
            name.withCString { cc_mechanical_set_parameter(handle, $0, value, error, capacity) }
        }
    }

    func undo() throws { try Self.checked { cc_mechanical_undo(handle, $0, $1) } }
    func redo() throws { try Self.checked { cc_mechanical_redo(handle, $0, $1) } }
    func save() throws { try Self.checked { cc_mechanical_save(handle, $0, $1) } }

    func triangles(part: Int32) throws -> [cc_triangle] {
        var message = [CChar](repeating: 0, count: 4096)
        let capacity = message.count
        let count = cc_mechanical_triangles(handle, part, nil, 0, &message, capacity)
        guard count > 0 else { throw Self.failure(message) }
        var result = [cc_triangle](repeating: cc_triangle(), count: count)
        let written = result.withUnsafeMutableBufferPointer {
            cc_mechanical_triangles(handle, part, $0.baseAddress, $0.count, &message, capacity)
        }
        guard written == count else { throw Self.failure(message) }
        return result
    }

    func exportSTL(part: Int32) throws -> URL {
        try export(part: part, extension: "stl") { cc_mechanical_export_stl(handle, part, $0, $1, $2) }
    }

    /// The exact solid as STEP. Shown in Fabricate since the 2026-10-31 STEP gate: the iPad's
    /// files reimported as valid exact solids in pinned desktop FreeCAD 1.1.4
    /// (integrations/freecad_core/device/evidence/ipad-step-gate-20261031.json).
    func exportSTEP(part: Int32) throws -> URL {
        try export(part: part, extension: "step") { cc_mechanical_export_step(handle, part, $0, $1, $2) }
    }

    /// The bodies' PartDesign features in tree order (cc_mechanical_call_json "features").
    func features() throws -> [MechanicalFeature] {
        struct Reply: Decodable { let features: [MechanicalFeature] }
        return try JSONDecoder().decode(Reply.self, from: callJSON("features")).features
    }

    struct Openings: Decodable {
        var cutouts: [WallCutout]
        var vents: [VentRow]
        var screws: LidScrews?
        /// The board margin each screw size's corner posts need, by LidScrews.postKey.
        var screw_posts: [String: Double]
        var tray_height: Double
        var sketches: [UserSketch]
    }

    /// The tray's wall cutouts, vent rows and lid screws, and what they depend on.
    func openings() throws -> Openings {
        try JSONDecoder().decode(Openings.self, from: callJSON("openings"))
    }

    private struct OpeningsRequest: Encodable {
        var cutouts: [WallCutout]
        var vents: [VentRow]
        var screws: LidScrews?
        var sketches: [UserSketch]
        var margin: Double?
        enum Keys: String, CodingKey { case cutouts, vents, screws, sketches, settings }
        func encode(to encoder: Encoder) throws {
            var container = encoder.container(keyedBy: Keys.self)
            try container.encode(cutouts, forKey: .cutouts)
            try container.encode(vents, forKey: .vents)
            try container.encode(sketches, forKey: .sketches)
            try container.encode(screws, forKey: .screws)  // null: no lid screws
            if let margin { try container.encode(["margin": margin], forKey: .settings) }
        }
    }

    /// Replaces all openings (and the board margin, when given) as one undoable step; the session
    /// refuses invalid ones unchanged.
    func setOpenings(cutouts: [WallCutout], vents: [VentRow], screws: LidScrews?, sketches: [UserSketch],
                     margin: Double? = nil) throws {
        let request = OpeningsRequest(cutouts: cutouts, vents: vents, screws: screws, sketches: sketches, margin: margin)
        _ = try callJSON("set_openings", String(decoding: try JSONEncoder().encode(request), as: UTF8.self))
    }

    /// The polygon with edge `edge` (from corner `edge` to the next) set to `length` by FreeCAD's
    /// Sketcher: horizontal and vertical edges stay so, and only corners tied to the edge's end move.
    func solveSketch(points: [CGPoint], bulges: [Double], edge: Int, length: Double) throws -> SketchShape {
        struct Request: Encodable { let points: [[Double]]; let edge: Int; let length: Double }
        struct Reply: Decodable { let points: [[Double]] }
        let request = Request(points: SketchShape.corners(points, bulges), edge: edge, length: length)
        let reply = try JSONDecoder().decode(Reply.self, from: callJSON("solve_sketch",
                                                                       String(decoding: try JSONEncoder().encode(request), as: UTF8.self)))
        return .polygon(reply.points.map { CGPoint(x: $0[0], y: $0[1]) }, reply.points.map { $0.count == 3 ? $0[2] : 0 })
    }

    private func callJSON(_ method: String, _ arguments: String = "{}") throws -> Data {
        var message = [CChar](repeating: 0, count: 4096)
        let text = message.withUnsafeMutableBufferPointer {
            cc_mechanical_call_json(handle, method, arguments, $0.baseAddress!, $0.count)
        }
        guard let text else { throw Self.failure(message) }
        defer { cc_free_text(text) }
        return Data(String(cString: text).utf8)
    }

    private func export(part: Int32, extension pathExtension: String,
                        _ write: (UnsafePointer<CChar>, UnsafeMutablePointer<CChar>, Int) -> Int32) throws -> URL {
        // The adapter refuses unknown overwrite. Every export gets its own private directory.
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let url = directory.appendingPathComponent((part == 0 ? "CorvCAD-Tray." : "CorvCAD-Lid.") + pathExtension)
        do {
            try Self.checked { error, capacity in url.path.withCString { write($0, error, capacity) } }
        } catch {
            try? FileManager.default.removeItem(at: directory)
            throw error
        }
        return url
    }

    #if CORVCAD_DIAGNOSTICS
    func diagnosticRecompute() throws {
        try Self.checked { cc_mechanical_recompute(handle, $0, $1) }
    }

    func diagnosticExportSTEP(part: Int32, to url: URL) throws {
        try Self.checked { error, capacity in
            url.path.withCString { cc_mechanical_export_step(handle, part, $0, error, capacity) }
        }
    }
    #endif

    private static func checked(_ operation: (UnsafeMutablePointer<CChar>, Int) -> Int32) throws {
        var message = [CChar](repeating: 0, count: 4096)
        let ok = message.withUnsafeMutableBufferPointer { operation($0.baseAddress!, $0.count) }
        guard ok == 1 else { throw failure(message) }
    }

    private static func failure(_ message: [CChar]) -> DesignError {
        let text = String(cString: message)
        return .message(text.isEmpty ? "The enclosure operation did not complete." : text)
    }
}

/// F1 supports a single axis-aligned rectangular board. Never replace another outline with its bounds.
struct MechanicalBoardInput {
    let json: String
    let identity: Data
    let thickness: Double

    init(board: OpaquePointer, source: Data, name: String, outline: [CGPoint]) throws {
        var x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0
        cc_board_bounds(board, &x0, &y0, &x1, &y1)
        let corners = [CGPoint(x: x0, y: y0), CGPoint(x: x1, y: y0),
                       CGPoint(x: x1, y: y1), CGPoint(x: x0, y: y1)]
        let epsilon: CGFloat = 0.000001
        guard outline.count == 4, x1 > x0, y1 > y0,
              corners.allSatisfy({ corner in
                  outline.filter { abs($0.x-corner.x) < epsilon && abs($0.y-corner.y) < epsilon }.count == 1
              }), outline.indices.allSatisfy({ index in
                  let a = outline[index], b = outline[(index+1) % outline.count]
                  return abs(a.x-b.x) < epsilon || abs(a.y-b.y) < epsilon
              }) else {
            throw DesignError.message("Enclosures currently require a rectangular board with four straight edges.")
        }
        thickness = cc_board_thickness(board)
        guard thickness.isFinite, thickness > 0 else {
            throw DesignError.message("The board must specify a positive thickness before creating an enclosure.")
        }
        let holes: [[String: Any]] = (0..<cc_hole_count(board)).map { index in
            var hole = cc_hole_at(board, index)
            let reference = withUnsafeBytes(of: &hole.reference) {
                String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)
            }
            return ["reference": reference, "x": hole.x, "y": hole.y, "diameter": hole.diameter]
        }
        let mechanical: [String: Any] = ["bounds": [x0, y0, x1, y1], "board_thickness": thickness,
                                         "mounting_holes": holes]
        identity = try JSONSerialization.data(withJSONObject: mechanical, options: [.sortedKeys])
        var document = mechanical
        document["schema"] = 1
        document["units"] = "mm"
        document["coordinate_system"] = "KiCad XY unchanged; enclosure Z up"
        document["source"] = ["name": name,
                              "sha256": SHA256.hash(data: source).map { String(format: "%02x", $0) }.joined(),
                              "kicad_version": "compatible parser"]
        json = String(decoding: try JSONSerialization.data(withJSONObject: document, options: [.sortedKeys]), as: UTF8.self)
    }

    /// A mechanical-only project's input: its inside space as a board of that size and height,
    /// with no mounting holes, from the project file.
    init(box: BoxSize) throws {
        guard [box.width, box.depth, box.height].allSatisfy({ $0.isFinite && $0 >= 1 }) else {
            throw DesignError.message("The box needs a width, depth and height of at least 1 mm.")
        }
        thickness = box.height
        let mechanical: [String: Any] = ["bounds": [0.0, 0.0, box.width, box.depth], "board_thickness": box.height,
                                         "mounting_holes": [[String: Any]]()]
        identity = try JSONSerialization.data(withJSONObject: mechanical, options: [.sortedKeys])
        var document = mechanical
        document["schema"] = 1
        document["units"] = "mm"
        document["coordinate_system"] = "KiCad XY unchanged; enclosure Z up"
        document["source"] = ["name": ProjectEngines.file,
                              "sha256": SHA256.hash(data: identity).map { String(format: "%02x", $0) }.joined(),
                              "kicad_version": "none"]
        json = String(decoding: try JSONSerialization.data(withJSONObject: document, options: [.sortedKeys]), as: UTF8.self)
    }

    /// Board appearance uses the measured thickness; native solids supply the enclosure meshes.
    func boardTriangles(bounds: CGRect, settings: cc_settings) -> [cc_triangle] {
        let z = settings.floor + settings.standoff_height
        let vertices: [(Float, Float, Float)] = [
            (Float(bounds.minX), Float(bounds.minY), Float(z)),
            (Float(bounds.maxX), Float(bounds.minY), Float(z)),
            (Float(bounds.maxX), Float(bounds.maxY), Float(z)),
            (Float(bounds.minX), Float(bounds.maxY), Float(z)),
            (Float(bounds.minX), Float(bounds.minY), Float(z+thickness)),
            (Float(bounds.maxX), Float(bounds.minY), Float(z+thickness)),
            (Float(bounds.maxX), Float(bounds.maxY), Float(z+thickness)),
            (Float(bounds.minX), Float(bounds.maxY), Float(z+thickness)),
        ]
        return [[0,2,1], [0,3,2], [4,5,6], [4,6,7], [0,1,5], [0,5,4],
                [1,2,6], [1,6,5], [2,3,7], [2,7,6], [3,0,4], [3,4,7]].map { face in
            let a = vertices[face[0]], b = vertices[face[1]], c = vertices[face[2]]
            return cc_triangle(ax: a.0, ay: a.1, az: a.2, bx: b.0, by: b.1, bz: b.2,
                               cx: c.0, cy: c.1, cz: c.2)
        }
    }
}

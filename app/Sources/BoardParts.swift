import SceneKit
import SwiftUI
import simd

/// How many of the board's parts are generated and how many are placeholders.
struct PartsStatus: View {
    let parts: BoardParts

    var body: some View {
        #if NATIVE_FREECAD
        Menu {
            Section("Cache \(ByteCountFormatter.string(fromByteCount: Int64(parts.cachedBytes), countStyle: .file))") {
                Button("Clear generated parts", systemImage: "trash", role: .destructive) { parts.clearCache() }
                    .accessibilityIdentifier("clearGeneratedParts")
            }
        } label: { panel }
        .accessibilityIdentifier("partsStatus")
        #else
        panel.accessibilityIdentifier("partsStatus")
        #endif
    }

    private var panel: some View {
        HStack(spacing: 8) {
            MonoLabel("Parts", color: Corv.textFaint, small: true)
            Text(summary).font(.corvReadout).foregroundStyle(Corv.cyan)
        }
        .padding(.horizontal, 14).padding(.vertical, 10)
        .corvPanel(radius: 14)
        .accessibilityElement(children: .combine)
    }

    private var summary: String {
        var items: [String] = []
        if parts.generated > 0 { items.append("\(parts.generated) generated") }
        if parts.placeholders > 0 { items.append("\(parts.placeholders) placeholder\(parts.placeholders == 1 ? "" : "s")") }
        if parts.generating { items.append("generating…") }
        return items.isEmpty ? "none" : items.joined(separator: " · ")
    }
}

/// The board's components in the 3D view (docs/3D_MODELS.md gate 3). In the FreeCAD edition a
/// footprint whose 3D model the generator covers gets its generated part, built in the background
/// and kept in memory per model path; a footprint with any other model gets a marked placeholder
/// box over its pads, and one without a model shows nothing. Other editions show placeholders only.
@Observable
final class BoardParts {
    private(set) var generated = 0
    private(set) var placeholders = 0
    private(set) var generating = false
    private(set) var cachedBytes = 0
    /// Generated parts' reach above the board's top face and below its bottom face (leads of a
    /// through-hole part, or a back-side part), mm, keyed by reference.
    private(set) var heights: [String: (above: Double, below: Double)] = [:]
    /// Footprints with a 3D model the generator doesn't cover: their heights are unknown.
    private(set) var unknownHeights: [String] = []

    @ObservationIgnored private var geometries: [String: SCNGeometry] = [:]
    /// Each generated part's box in its model frame.
    @ObservationIgnored private var boxes: [String: (SIMD3<Float>, SIMD3<Float>)] = [:]
    /// Model paths the generator has no part for (or failed on), with why.
    @ObservationIgnored private(set) var unavailable: [String: String] = [:]
    @ObservationIgnored private var root = SCNNode()
    @ObservationIgnored private var placements: [cc_placement] = []
    @ObservationIgnored private var top: Float = 0
    @ObservationIgnored private var bottom: Float = 0

    /// A new node of the board's parts; it fills in by itself as parts finish generating.
    func node(_ placements: [cc_placement], top: Float, bottom: Float) -> SCNNode {
        (self.placements, self.top, self.bottom) = (placements, top, bottom)
        root = SCNNode()
        root.name = "parts"
        fill()
        return root
    }

    private func fill() {
        root.childNodes.forEach { $0.removeFromParentNode() }
        var (made, marked) = (0, 0)
        var wanted = Set<String>()
        var extents: [String: (above: Double, below: Double)] = [:], unknown: [String] = []
        for p in placements {
            let path = schematicText(p.model)
            if path.isEmpty { continue }  // like KiCad's 3D viewer: no model, nothing shown
            let footprint = Self.toScene * Self.footprintTransform(p, top: top, bottom: bottom)
            if let geometry = geometries[path] {
                let node = SCNNode(geometry: geometry)
                node.simdTransform = footprint * Self.modelTransform(p)
                root.addChildNode(node)
                made += 1
                if let box = boxes[path] {
                    // The model's reach along the footprint's normal (its frame's z after offset,
                    // rotation and scale): outward from its side, and through the board (leads).
                    let model = Self.modelTransform(p), ends = [box.0, box.1]
                    let z = ends.flatMap { a in ends.flatMap { b in ends.map { c in (model * SIMD4(a.x, b.y, c.z, 1)).z } } }
                    let (outward, through) = (Double(z.max() ?? 0), Double(-(z.min() ?? 0)))
                    extents[schematicText(p.reference)] = p.back != 0 ? (through, outward) : (outward, through)
                }
            } else {
                if unavailable[path] == nil { wanted.insert(path) }
                else { unknown.append(schematicText(p.reference)) }
                if let box = Self.placeholder(p, footprint) { root.addChildNode(box); marked += 1 }
            }
        }
        (generated, placeholders) = (made, marked)
        (heights, unknownHeights) = (extents, unknown.sorted())
        cachedBytes = Self.cacheBytes
        generate(wanted)
    }

    private func generate(_ paths: Set<String>) {
        #if NATIVE_FREECAD
        guard !paths.isEmpty, !generating else { return }
        generating = true
        Task.detached(priority: .userInitiated) { [weak self] in
            var built: [(String, [cc_part_triangle]?, String)] = []
            for path in paths.sorted() {
                var key = [CChar](repeating: 0, count: 128)
                let keyCapacity = key.count
                guard cc_parts3d_key(path, &key, keyCapacity) > 0 else {
                    built.append((path, nil, "No generator for this model.")); continue
                }
                let file = Self.cacheDirectory.appendingPathComponent(String(cString: key) + ".mesh")
                if let cached = Self.read(file) { built.append((path, cached, "")); continue }
                var error = [CChar](repeating: 0, count: 512)
                let capacity = error.count
                let count = cc_parts3d_triangles(path, nil, 0, &error, capacity)
                var triangles = [cc_part_triangle](repeating: cc_part_triangle(), count: count)
                if count > 0 {
                    _ = triangles.withUnsafeMutableBufferPointer { cc_parts3d_triangles(path, $0.baseAddress, $0.count, nil, 0) }
                    Self.write(triangles, to: file)
                }
                built.append((path, count > 0 ? triangles : nil, String(cString: error)))
            }
            await MainActor.run {
                guard let self else { return }
                for (path, triangles, error) in built {
                    if let triangles {
                        self.geometries[path] = Self.geometry(triangles)
                        self.boxes[path] = Self.bounds(triangles)
                    }
                    else { self.unavailable[path] = error }
                }
                self.generating = false
                self.fill()
            }
        }
        #else
        for path in paths { unavailable[path] = "Generated parts need the FreeCAD edition." }
        #endif
    }

    // MARK: Cache (docs/3D_MODELS.md choice 4)

    /// Generated meshes, one file per cache key (generator version, family, parameter hash). iOS may
    /// purge it; nothing generated goes into a project folder.
    static let cacheDirectory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("GeneratedParts", isDirectory: true)

    /// Bytes the cache holds.
    static var cacheBytes: Int {
        let files = (try? FileManager.default.contentsOfDirectory(at: cacheDirectory, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        return files.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
    }

    /// Deletes the cache and every generated part in memory; the board's parts generate again.
    func clearCache() {
        try? FileManager.default.removeItem(at: Self.cacheDirectory)
        #if NATIVE_FREECAD
        cc_parts3d_clear()
        #endif
        geometries = [:]
        unavailable = [:]
        fill()
    }

    private static func read(_ file: URL) -> [cc_part_triangle]? {
        let stride = MemoryLayout<cc_part_triangle>.stride
        guard let data = try? Data(contentsOf: file), !data.isEmpty, data.count % stride == 0 else { return nil }
        var triangles = [cc_part_triangle](repeating: cc_part_triangle(), count: data.count / stride)
        _ = triangles.withUnsafeMutableBytes { data.copyBytes(to: $0) }
        return triangles
    }

    private static func write(_ triangles: [cc_part_triangle], to file: URL) {
        try? FileManager.default.createDirectory(at: cacheDirectory, withIntermediateDirectories: true)
        try? triangles.withUnsafeBytes { Data($0) }.write(to: file, options: .atomic)
    }

    // MARK: Transforms (KiCad's 3D viewer conventions)

    /// KiCad 3D (x right, y up the board, z up) to the scene the enclosure preview uses
    /// (board x, height, board y down): (x, y, z) -> (x, z, y).
    private static let toScene = simd_float4x4(rows: [SIMD4(1, 0, 0, 0), SIMD4(0, 0, 1, 0),
                                                       SIMD4(0, 1, 0, 0), SIMD4(0, 0, 0, 1)])

    /// The footprint's anchor on the board's top (or bottom, turned over) at its rotation.
    private static func footprintTransform(_ p: cc_placement, top: Float, bottom: Float) -> simd_float4x4 {
        var m = translation(Float(p.x), -Float(p.y), p.back != 0 ? bottom : top)
            * rotation(Float(p.rotation), axis: SIMD3(0, 0, 1))
        // KiCad (render_3d_opengl.cpp) turns a flipped footprint 180 degrees about Y then Z: 180 about X.
        if p.back != 0 { m *= rotation(180, axis: SIMD3(1, 0, 0)) }
        return m
    }

    /// The footprint's (model (offset) (rotate) (scale)).
    private static func modelTransform(_ p: cc_placement) -> simd_float4x4 {
        let o = p.offset, r = p.rotate, s = p.scale
        return translation(Float(o.0), Float(o.1), Float(o.2))
            * rotation(-Float(r.2), axis: SIMD3(0, 0, 1)) * rotation(-Float(r.1), axis: SIMD3(0, 1, 0))
            * rotation(-Float(r.0), axis: SIMD3(1, 0, 0))
            * simd_float4x4(diagonal: SIMD4(Float(s.0), Float(s.1), Float(s.2), 1))
    }

    private static func translation(_ x: Float, _ y: Float, _ z: Float) -> simd_float4x4 {
        var m = matrix_identity_float4x4
        m.columns.3 = SIMD4(x, y, z, 1)
        return m
    }

    private static func rotation(_ degrees: Float, axis: SIMD3<Float>) -> simd_float4x4 {
        simd_float4x4(simd_quatf(angle: degrees * .pi / 180, axis: axis))
    }

    // MARK: Geometry

    private static func bounds(_ triangles: [cc_part_triangle]) -> (SIMD3<Float>, SIMD3<Float>) {
        var lo = SIMD3<Float>(repeating: .greatestFiniteMagnitude), hi = -lo
        for t in triangles {
            for p in [SIMD3(t.ax, t.ay, t.az), SIMD3(t.bx, t.by, t.bz), SIMD3(t.cx, t.cy, t.cz)] {
                lo = simd_min(lo, p); hi = simd_max(hi, p)
            }
        }
        return (lo, hi)
    }

    /// One geometry per part: flat-shaded triangles, one element per solid colour.
    private static func geometry(_ triangles: [cc_part_triangle]) -> SCNGeometry {
        var vertices: [SCNVector3] = [], normals: [SCNVector3] = []
        var byColor: [SIMD3<Float>: [UInt32]] = [:]
        vertices.reserveCapacity(triangles.count * 3)
        normals.reserveCapacity(triangles.count * 3)
        for t in triangles {
            let a = SIMD3(t.ax, t.ay, t.az), b = SIMD3(t.bx, t.by, t.bz), c = SIMD3(t.cx, t.cy, t.cz)
            let n = simd_normalize(simd_cross(b - a, c - a))
            let normal = n.x.isFinite ? SCNVector3(n) : SCNVector3(0, 0, 1)
            let first = UInt32(vertices.count)
            vertices += [SCNVector3(a), SCNVector3(b), SCNVector3(c)]
            normals += [normal, normal, normal]
            byColor[SIMD3(t.r, t.g, t.b), default: []] += [first, first + 1, first + 2]
        }
        var elements: [SCNGeometryElement] = [], materials: [SCNMaterial] = []
        for (color, indices) in byColor.sorted(by: { $0.value[0] < $1.value[0] }) {
            elements.append(SCNGeometryElement(indices: indices, primitiveType: .triangles))
            let material = SCNMaterial()
            material.diffuse.contents = UIColor(red: CGFloat(color.x), green: CGFloat(color.y), blue: CGFloat(color.z), alpha: 1)
            material.lightingModel = .physicallyBased
            let metal = color.x > 0.6 && abs(color.x - color.z) < 0.08 && color.y > 0.6  // the generator's grey pins
            material.metalness.contents = metal ? 0.8 : 0.1
            material.roughness.contents = metal ? 0.35 : 0.6
            material.isDoubleSided = true  // the board side's mirror reverses the winding
            materials.append(material)
        }
        let geometry = SCNGeometry(sources: [SCNGeometrySource(vertices: vertices), SCNGeometrySource(normals: normals)],
                                   elements: elements)
        geometry.materials = materials
        return geometry
    }

    /// A translucent box over the footprint's pads, 1 mm tall, marking a part without a model.
    private static func placeholder(_ p: cc_placement, _ footprint: simd_float4x4) -> SCNNode? {
        let e = p.extent
        let width = Float(e.2 - e.0), depth = Float(e.3 - e.1)
        guard width > 0, depth > 0 else { return nil }
        let height: Float = 1
        let box = SCNBox(width: CGFloat(width), height: CGFloat(depth), length: CGFloat(height), chamferRadius: 0)
        let material = SCNMaterial()
        material.diffuse.contents = UIColor(white: 0.75, alpha: 1)
        material.lightingModel = .constant
        box.materials = [material]
        let node = SCNNode(geometry: box)
        node.opacity = 0.28
        node.name = "placeholder"
        // Footprint coordinates are y down; KiCad's 3D frame is y up.
        node.simdTransform = footprint * translation(Float(e.0 + e.2) / 2, -Float(e.1 + e.3) / 2, height / 2)
        return node
    }
}

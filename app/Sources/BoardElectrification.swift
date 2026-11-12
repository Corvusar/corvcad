import SceneKit
import SwiftUI

/// The simulation on the 3D board: each net's copper glows with its voltage over the run, and
/// dots move along tracks where the current in the copper can be worked out from the pins.
@Observable
final class Electrification {
    var enabled = false
    var playing = false
    /// 0...1 through the run.
    private(set) var fraction = 0.0
    private(set) var summary = ""
    private(set) var currentTime = ""

    private struct Glow { let material: SCNMaterial; let volts: [Float] }
    private struct Flow {
        struct Edge { let a, b: SCNVector3; let dots: Int; var phase: Float = 0 }
        var edges: [Edge]
        /// edges x pins, as cc_net_flow_unit.
        let unit: [Float]
        /// Per pin, the current entering the net at each point of the run.
        let entering: [[Float]]
    }
    @ObservationIgnored private var root: SCNNode?
    @ObservationIgnored private var dots = SCNNode()
    @ObservationIgnored private var dotMaterial = SCNMaterial()
    @ObservationIgnored private var glows: [Glow] = []
    @ObservationIgnored private var flows: [Flow] = []
    @ObservationIgnored private var times: [Double] = []
    @ObservationIgnored private var unit = "s"
    @ObservationIgnored private var peakVolts: Float = 1
    @ObservationIgnored private var peakAmps: Float = 1
    @ObservationIgnored private var timer: Timer?
    @ObservationIgnored private var lastSample = -1

    deinit { timer?.invalidate() }

    func stop() {
        enabled = false
        playing = false
        timer?.invalidate()
        timer = nil
        root?.removeFromParentNode()
        root = nil
        glows = []
        flows = []
    }

    /// Builds the copper from the board and starts the view on this scene.
    func start(design: Design, run: SimulationResult, autoplay: Bool) {
        stop()
        guard run.animatable else { return }
        times = run.scale?.real ?? []
        unit = run.xUnit
        let bottom = Float(design.settings.floor + design.settings.standoff_height)
        let top = bottom + Float(MechanicalSession.enabled ? design.thickness ?? 1.6 : 1.6)
        func height(_ layer: String) -> Float {
            layer == "F.Cu" ? top + 0.02 : layer == "B.Cu" ? bottom - 0.02 : (top + bottom) / 2
        }
        let group = SCNNode()
        let points = max(times.count, 1)
        var simulated = 0
        var peak: Float = 0
        var withVoltage: [(code: Int32, material: SCNMaterial, volts: [Float])] = []
        var nodeOf: [Int32: String] = [:]

        for net in design.nets where net.code != 0 {
            let name = schematicText(net.name)
            nodeOf[net.code] = SpiceNames.node(name)
            let material = SCNMaterial()
            material.lightingModel = .constant
            material.isDoubleSided = true
            material.diffuse.contents = Self.unsimulated
            var triangles: [SCNVector3] = []
            for track in design.tracks where track.net == net.code {
                Self.addTrack(&triangles, track, height(schematicText(track.layer)))
            }
            for via in design.vias where via.net == net.code {
                for y in [top + 0.02, bottom - 0.02] {
                    Self.addEllipse(&triangles, x: via.x, y: via.y, rx: via.size / 2, ry: via.size / 2, height: y)
                }
            }
            for pad in design.pads where pad.net == net.code {
                let layers = schematicText(pad.layers)
                let all = layers.contains("*.Cu")
                if all || layers.contains("F.Cu") { Self.addPad(&triangles, pad, top + 0.02) }
                if all || layers.contains("B.Cu") { Self.addPad(&triangles, pad, bottom - 0.02) }
            }
            if !triangles.isEmpty {
                let geometry = Self.geometry(triangles)
                geometry.materials = [material]
                group.addChildNode(SCNNode(geometry: geometry))
            }
            for zone in design.zones where zone.net == net.code && zone.filled {
                if let node = Self.zoneNode(zone, material: material, height: height(zone.layer)) { group.addChildNode(node) }
            }
            if SpiceNames.isGround(name) {
                simulated += 1
                withVoltage.append((net.code, material, [Float](repeating: 0, count: points)))
            } else if let vector = run.vectors[SpiceNames.node(name)], !vector.real.isEmpty {
                simulated += 1
                let volts = vector.real.map { Float($0) }
                peak = max(peak, volts.map(abs).max() ?? 0)
                withVoltage.append((net.code, material, volts))
            }
        }
        glows = withVoltage.map { Glow(material: $0.material, volts: $0.volts) }
        peakVolts = max(peak, 1e-6)

        flows = []
        var amps: Float = 0
        for (code, _, _) in withVoltage {
            guard let node = nodeOf[code], let flow = Self.flow(net: code, node: node, design: design, run: run,
                                                               height: height) else { continue }
            amps = max(amps, flow.entering.joined().map(abs).max() ?? 0)
            flows.append(flow)
        }
        peakAmps = max(amps, 1e-12)

        dotMaterial = SCNMaterial()
        dotMaterial.lightingModel = .constant
        dotMaterial.diffuse.contents = UIColor.white
        dots = SCNNode()
        group.addChildNode(dots)
        design.scene.rootNode.addChildNode(group)
        root = group
        let total = design.nets.filter { $0.code != 0 }.count
        summary = "±\(siText(Double(peakVolts), unit: "V")) · \(simulated) of \(total) nets simulated · current on \(flows.count)"
        enabled = true
        lastSample = -1
        render(elapsed: 0)
        guard autoplay else { return }
        playing = times.count > 1
        var last = Date()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30, repeats: true) { [weak self] _ in
            let now = Date()
            self?.tick(now.timeIntervalSince(last))
            last = now
        }
    }

    /// The time slider.
    func scrub(_ value: Double) {
        fraction = value
        render(elapsed: 0)
    }

    private func tick(_ elapsed: Double) {
        if playing {
            // ponytail: the run always takes 8 seconds; a speed control if a long run needs one.
            var next = fraction + elapsed / 8
            if next >= 1 { next = 0 }
            fraction = next
        }
        render(elapsed: Float(elapsed))
    }

    private var sample: Int {
        guard times.count > 1, let first = times.first, let last = times.last else { return 0 }
        let target = first + (last - first) * fraction
        var low = 0, high = times.count - 1
        while low < high {
            let middle = (low + high) / 2
            if times[middle] < target { low = middle + 1 } else { high = middle }
        }
        return low
    }

    private func render(elapsed: Float) {
        let index = sample
        SCNTransaction.begin()
        SCNTransaction.disableActions = true
        if index != lastSample {
            lastSample = index
            for glow in glows where index < glow.volts.count {
                glow.material.diffuse.contents = Self.color(volts: glow.volts[index], peak: peakVolts)
            }
            if times.count > 1 {
                let text = siText(times[min(index, times.count - 1)], unit: unit)
                if text != currentTime { currentTime = text }
            }
        }
        renderDots(index: index, elapsed: elapsed)
        SCNTransaction.commit()
    }

    private func renderDots(index: Int, elapsed: Float) {
        var vertices: [SCNVector3] = []
        for f in flows.indices {
            let pins = flows[f].entering.count
            for e in flows[f].edges.indices {
                var current: Float = 0
                for pin in 0..<pins where index < flows[f].entering[pin].count {
                    current += flows[f].unit[e * pins + pin] * flows[f].entering[pin][index]
                }
                let ratio = abs(current) / peakAmps
                guard ratio > 0.001 else { continue }
                let edge = flows[f].edges[e]
                // Square root, so small currents still visibly move.
                let speed = (0.03 + 0.5 * sqrt(ratio)) * (current >= 0 ? 1 : -1)
                let phase = (edge.phase + elapsed * speed).truncatingRemainder(dividingBy: 1)
                flows[f].edges[e].phase = phase
                for dot in 0..<edge.dots {
                    let t = (phase + Float(dot) / Float(edge.dots) + 1).truncatingRemainder(dividingBy: 1)
                    vertices.append(SCNVector3(edge.a.x + (edge.b.x - edge.a.x) * t, edge.a.y + 0.03,
                                               edge.a.z + (edge.b.z - edge.a.z) * t))
                }
            }
        }
        guard !vertices.isEmpty else { dots.geometry = nil; return }
        let indices = (0..<UInt32(vertices.count)).map { $0 }
        let element = SCNGeometryElement(data: indices.withUnsafeBytes { Data($0) }, primitiveType: .point,
                                         primitiveCount: vertices.count, bytesPerIndex: MemoryLayout<UInt32>.size)
        element.pointSize = 5
        element.minimumPointScreenSpaceRadius = 2
        element.maximumPointScreenSpaceRadius = 5
        let geometry = SCNGeometry(sources: [SCNGeometrySource(vertices: vertices)], elements: [element])
        geometry.materials = [dotMaterial]
        dots.geometry = geometry
    }

    /// Current in one net's copper, when the run gives the current at every pin on the net.
    private static func flow(net: Int32, node: String, design: Design, run: SimulationResult,
                             height: (String) -> Float) -> Flow? {
        var pins: [Int] = []
        var entering: [[Float]] = []
        var seen = Set<String>()
        for (index, pad) in design.pads.enumerated() where pad.net == net && schematicText(pad.type) != "np_thru_hole" {
            let reference = schematicText(pad.reference)
            guard seen.insert(reference).inserted else { continue } // a device's other pad on this net
            guard let current = run.injection(reference: reference, node: node) else { return nil }
            pins.append(index)
            entering.append(current.map { Float($0) })
        }
        // What enters a net leaves it. A net that doesn't sum to zero has a current this view
        // can't account for, so it shows no flow rather than a wrong one.
        let points = entering.map(\.count).min() ?? 0
        var peak: Float = 0, residual: Float = 0
        for point in 0..<points {
            var sum: Float = 0
            for pin in entering.indices {
                sum += entering[pin][point]
                peak = max(peak, abs(entering[pin][point]))
            }
            residual = max(residual, abs(sum))
        }
        guard pins.count >= 2, residual <= 0.05 * peak, let solved = design.netFlow(net: net, pins: pins) else { return nil }
        let edges = solved.edges.map { edge -> Flow.Edge in
            let y = height(schematicText(edge.layer))
            return Flow.Edge(a: point(edge.x0, edge.y0, y), b: point(edge.x1, edge.y1, y),
                             dots: min(8, max(1, Int(hypot(edge.x1 - edge.x0, edge.y1 - edge.y0) / 1.5))))
        }
        return Flow(edges: edges, unit: solved.unit, entering: entering)
    }

    // MARK: Colours and geometry

    private static let unsimulated = UIColor(hex: 0x2B2622)

    /// Cyan for positive, red for negative, on square root of the share of the run's peak.
    private static func color(volts: Float, peak: Float) -> UIColor {
        let share = CGFloat(min(1, sqrt(abs(volts) / peak)))
        let base: (CGFloat, CGFloat, CGFloat) = (0.20, 0.15, 0.11)
        let hot: (CGFloat, CGFloat, CGFloat) = volts >= 0 ? (0, 0.95, 1) : (1, 0.2, 0.4)
        return UIColor(red: base.0 + (hot.0 - base.0) * share, green: base.1 + (hot.1 - base.1) * share,
                       blue: base.2 + (hot.2 - base.2) * share, alpha: 1)
    }

    private static func geometry(_ triangles: [SCNVector3]) -> SCNGeometry {
        let indices = (0..<UInt32(triangles.count)).map { $0 }
        let element = SCNGeometryElement(data: indices.withUnsafeBytes { Data($0) }, primitiveType: .triangles,
                                         primitiveCount: triangles.count / 3, bytesPerIndex: MemoryLayout<UInt32>.size)
        return SCNGeometry(sources: [SCNGeometrySource(vertices: triangles)], elements: [element])
    }

    /// SceneKit triangulates the filled polygon (fractured, so even-odd fills its holes).
    private static func zoneNode(_ zone: ZoneShape, material: SCNMaterial, height: Float) -> SCNNode? {
        guard zone.points.count >= 3, zone.points.count < 30000 else { return nil }
        let path = UIBezierPath()
        path.move(to: zone.points[0])
        for point in zone.points.dropFirst() { path.addLine(to: point) }
        path.close()
        path.usesEvenOddFillRule = true
        let shape = SCNShape(path: path, extrusionDepth: 0)
        shape.materials = [material]
        let node = SCNNode(geometry: shape)
        node.eulerAngles.x = -.pi / 2 // (x, y, 0) to (x, 0, -y), as the board's own mesh
        node.position.y = height
        return node
    }

    private static func point(_ x: Double, _ y: Double, _ height: Float) -> SCNVector3 {
        SCNVector3(Float(x), height, Float(-y))
    }

    private static func addFan(_ triangles: inout [SCNVector3], _ corners: [SCNVector3]) {
        guard corners.count >= 3 else { return }
        for i in 1..<(corners.count - 1) { triangles += [corners[0], corners[i], corners[i + 1]] }
    }

    private static func addEllipse(_ triangles: inout [SCNVector3], x: Double, y: Double, rx: Double, ry: Double, height: Float) {
        addFan(&triangles, (0..<12).map {
            let angle = Double($0) * .pi / 6
            return point(x + rx * cos(angle), y + ry * sin(angle), height)
        })
    }

    private static func addTrack(_ triangles: inout [SCNVector3], _ track: cc_track, _ height: Float) {
        let dx = track.x1 - track.x0, dy = track.y1 - track.y0, length = hypot(dx, dy)
        guard length > 0 else { return }
        let nx = -dy / length * track.width / 2, ny = dx / length * track.width / 2
        addFan(&triangles, [point(track.x0 + nx, track.y0 + ny, height), point(track.x1 + nx, track.y1 + ny, height),
                            point(track.x1 - nx, track.y1 - ny, height), point(track.x0 - nx, track.y0 - ny, height)])
        // Round ends close the corners between segments.
        for (x, y) in [(track.x0, track.y0), (track.x1, track.y1)] {
            addEllipse(&triangles, x: x, y: y, rx: track.width / 2, ry: track.width / 2, height: height)
        }
    }

    private static func addPad(_ triangles: inout [SCNVector3], _ pad: cc_pad, _ height: Float) {
        let shape = schematicText(pad.shape)
        if shape == "circle" || shape == "oval" {
            addEllipse(&triangles, x: pad.x, y: pad.y, rx: pad.size_x / 2, ry: pad.size_y / 2, height: height)
            return
        }
        let angle = pad.rotation * .pi / 180, c = cos(angle), s = sin(angle)
        let halfX = pad.size_x / 2, halfY = pad.size_y / 2
        addFan(&triangles, [(-halfX, -halfY), (halfX, -halfY), (halfX, halfY), (-halfX, halfY)].map { dx, dy in
            point(pad.x + dx * c - dy * s, pad.y + dx * s + dy * c, height)
        })
    }
}

/// The controls over the 3D view: on and off, the time slider and the legend.
struct ElectrifyBar: View {
    @Bindable var electrification: Electrification
    let design: Design
    let run: SimulationResult
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                Button {
                    if electrification.enabled {
                        electrification.stop()
                    } else {
                        design.showLid = false // it would tint the copper
                        electrification.start(design: design, run: run, autoplay: !reduceMotion)
                    }
                } label: {
                    Label(electrification.enabled ? "Stop" : "Electrify", systemImage: "bolt.fill")
                }
                .buttonStyle(.corvPrimary).fixedSize()
                .accessibilityIdentifier("electrify")
                if electrification.enabled, run.scale != nil {
                    Button { electrification.playing.toggle() } label: {
                        Image(systemName: electrification.playing ? "pause.fill" : "play.fill").frame(width: 28, height: 28)
                    }
                    .buttonStyle(.plain).foregroundStyle(Corv.gold)
                    .disabled(reduceMotion)
                    .accessibilityLabel(electrification.playing ? "Pause" : "Play")
                    Slider(value: Binding(get: { electrification.fraction }, set: { electrification.scrub($0) }), in: 0...1)
                        .tint(Corv.gold)
                        .accessibilityLabel("Time in the simulation")
                    Text(electrification.currentTime).font(.corvReadout).foregroundStyle(Corv.cyan)
                        .frame(minWidth: 76, alignment: .trailing)
                }
            }
            if electrification.enabled {
                MonoLabel(electrification.summary, color: Corv.textDim, small: true)
                    .accessibilityIdentifier("electrifySummary")
                Text("Voltages are the simulator's. Current in the copper is calculated from pin currents, shown only on nets where every pin's current is known; dot speed is relative.")
                    .font(.caption2).foregroundStyle(Corv.textFaint)
            }
        }
        .padding(14)
        .frame(maxWidth: 620)
        .corvPanel(radius: 16)
    }
}

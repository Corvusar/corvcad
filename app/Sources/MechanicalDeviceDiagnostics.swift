#if CORVCAD_DIAGNOSTICS
import CryptoKit
import Foundation

/// Hidden, development-only CorvProbe command. It never changes the user's open project.
/// Files remain under a fresh Gate5 request directory; the app's own STEP export is Fabricate's.
enum MechanicalDeviceDiagnostics {
    private struct Request: Decodable {
        let schema: Int
        let board: String
    }

    static func info() -> [String: Any] {
        var report: [String: Any] = ["schema": 1, "f5_accepted": false,
                                    "process_id": ProcessInfo.processInfo.processIdentifier,
                                    "native_available": MechanicalSession.enabled && cc_mechanical_available() == 1,
                                    "runtime_initialized": MechanicalSession.diagnosticRuntimeInitialized]
        #if targetEnvironment(simulator)
        report["hardware_class"] = "simulator"
        #elseif os(iOS) && !targetEnvironment(macCatalyst)
        report["hardware_class"] = "physical-device"
        #else
        report["hardware_class"] = "unsupported"
        #endif
        return report
    }
    static func run(requestPath: String) -> [String: Any] {
        var report = info()
        report["passed"] = false
        report["gate"] = "F5-preparation"
        do {
            guard Thread.isMainThread, MechanicalSession.enabled else {
                throw DesignError.message("Mechanical diagnostics require the native edition on its main thread.")
            }
            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                .resolvingSymlinksInPath().standardizedFileURL
            let requestURL = URL(fileURLWithPath: requestPath).resolvingSymlinksInPath().standardizedFileURL
            let prefix = documents.appendingPathComponent("Gate5", isDirectory: true).path + "/"
            guard requestURL.path.hasPrefix(prefix), requestURL.lastPathComponent == "request.json" else {
                throw DesignError.message("Expected a request below Documents/Gate5.")
            }
            let request = try JSONDecoder().decode(Request.self, from: Data(contentsOf: requestURL))
            guard request.schema == 1, request.board.hasSuffix(".kicad_pcb"),
                  request.board.range(of: "^[A-Za-z0-9_.-]+$", options: .regularExpression) != nil,
                  request.board != ".", request.board != ".." else {
                throw DesignError.message("Unsupported mechanical diagnostic request.")
            }
            let directory = requestURL.deletingLastPathComponent()
            let board = directory.appendingPathComponent(request.board).resolvingSymlinksInPath()
            guard board.deletingLastPathComponent().standardizedFileURL == directory else {
                throw DesignError.message("The input board must remain inside its request directory.")
            }
            let output = directory.appendingPathComponent("outputs", isDirectory: true)
            // Refuse retries at an existing path, including failed/partial runs.
            guard !FileManager.default.fileExists(atPath: output.path) else {
                throw DesignError.message("The diagnostic output already exists.")
            }
            try FileManager.default.createDirectory(at: output, withIntermediateDirectories: false)
            guard try FileManager.default.contentsOfDirectory(atPath: output.path).isEmpty else {
                throw DesignError.message("The diagnostic output already exists.")
            }
            // createDirectory may succeed for an existing directory; reserve this run exclusively.
            let marker = output.appendingPathComponent("run.started")
            try Data("F5 preparation; no automatic acceptance\n".utf8).write(to: marker, options: .withoutOverwriting)
            let runtimeWasInitialized = MechanicalSession.diagnosticRuntimeInitialized
            report["runtime_initialized_before"] = runtimeWasInitialized
            report["open_measurement"] = runtimeWasInitialized ? "warm-runtime parse/open/preview" : "initialization plus parse/open/preview"
            let design = Design()
            design.setMechanicalProject(output)
            let openStart = DispatchTime.now()
            design.load(board)
            report["open_preview_ms"] = milliseconds(since: openStart)
            guard design.mechanicalReady, let input = design.mechanicalInput else {
                throw DesignError.message(design.mechanicalError ?? design.error ?? "Native enclosure open failed.")
            }
            report["input"] = try JSONSerialization.jsonObject(with: Data(input.json.utf8))
            try Data(input.json.utf8).write(to: output.appendingPathComponent("input.json"), options: .withoutOverwriting)
            let baseline = try design.mechanical!.settings
            let changes: [(String, Double)] = [("wall", 2.6), ("floor", 2.7), ("standoff_height", 5.1),
                ("standoff_diameter", 6.8), ("margin", 4), ("board_clearance", 3.1),
                ("lid_thickness", 2.6), ("lid_gap", 0.5)]
            let initial = settings(baseline)
            var timings: [[String: Any]] = []
            for (name, value) in changes {
                let start = DispatchTime.now()
                try design.mechanical!.set(name, value: value)
                let elapsed = milliseconds(since: start)
                let restored = DispatchTime.now()
                try design.mechanical!.set(name, value: initial[name]!)
                timings.append(["parameter": name, "value_mm": value, "edit_recompute_ms": elapsed,
                                "restore_recompute_ms": milliseconds(since: restored)])
            }
            report["parameter_timings"] = timings
            let recompute = DispatchTime.now()
            try design.mechanical!.diagnosticRecompute()
            report["recompute_validate_ms"] = milliseconds(since: recompute)
            try design.mechanical!.set("wall", value: baseline.wall + 0.5)
            try design.mechanical!.undo()
            guard try settings(design.mechanical!.settings) == initial, design.mechanical!.canRedo else {
                throw DesignError.message("Mechanical undo did not restore the baseline.")
            }
            try design.mechanical!.redo()
            guard try design.mechanical!.settings.wall == baseline.wall + 0.5 else {
                throw DesignError.message("Mechanical redo did not restore the edit.")
            }
            try design.mechanical!.undo()
            let beforeRejected = (design.mechanical!.canUndo, design.mechanical!.canRedo)
            var rejected = false
            do { try design.mechanical!.set("wall", value: -1) } catch { rejected = true }
            guard rejected, try settings(design.mechanical!.settings) == initial,
                  design.mechanical!.canUndo == beforeRejected.0, design.mechanical!.canRedo == beforeRejected.1 else {
                throw DesignError.message("Rejected mechanical edit changed settings or history.")
            }
            var previews: [String: Any] = [:]
            for (name, part) in [("tray", Int32(0)), ("lid", Int32(1))] {
                let start = DispatchTime.now()
                let triangles = try design.mechanical!.triangles(part: part)
                previews[name] = ["triangles": triangles.count, "tessellation_ms": milliseconds(since: start)]
            }
            report["preview"] = previews
            let saving = DispatchTime.now()
            try design.mechanical!.save()
            report["save_ms"] = milliseconds(since: saving)
            var exports: [String: Any] = [:]
            for (name, part) in [("tray", Int32(0)), ("lid", Int32(1))] {
                let stepStart = DispatchTime.now()
                try design.mechanical!.diagnosticExportSTEP(part: part, to: output.appendingPathComponent(name + ".step"))
                let stepMS = milliseconds(since: stepStart)
                let stlStart = DispatchTime.now()
                let temporary = try design.mechanical!.exportSTL(part: part)
                try FileManager.default.copyItem(at: temporary, to: output.appendingPathComponent(name + ".stl"))
                exports[name] = ["exact_step_ms": stepMS, "stl_ms": milliseconds(since: stlStart)]
            }
            report["exports"] = exports
            let persisted = try Data(contentsOf: output.appendingPathComponent("enclosure.FCStd"))
            design.setMechanicalProject(nil) // close before reopening; no second owner of the path
            let reopened = Design()
            reopened.setMechanicalProject(output)
            let reopening = DispatchTime.now()
            reopened.load(board)
            report["reopen_preview_ms"] = milliseconds(since: reopening)
            guard reopened.mechanicalReady, settings(reopened.settings) == initial,
                  try Data(contentsOf: output.appendingPathComponent("enclosure.FCStd")) == persisted else {
                throw DesignError.message("FCStd reopen changed document bytes or parameters.")
            }
            report["settings_mm"] = initial
            report["operations"] = ["parameter_restore": true, "undo": true, "redo": true,
                                     "rejected_edit_rollback": true, "save_reopen_byte_preservation": true,
                                     "step": true, "stl": true]
            report["kicad_board_backend"] = cc_board_native_backend().map { String(cString: $0) } ?? "unavailable"
            if let runtime = Bundle.main.resourceURL?.appendingPathComponent("FreeCADRuntime") {
                var sources: [String: Any] = [:]
                for name in ["upstream.json", "session_runtime.py", "model.py", "inputs.py", "checks.py"] {
                    let relative = "harness/" + name
                    let data = try Data(contentsOf: runtime.appendingPathComponent(relative))
                    sources[relative] = ["bytes": data.count, "sha256": digest(data)]
                }
                report["runtime_sources"] = sources
                let manifest = runtime.appendingPathComponent("runtime-manifest.json")
                if FileManager.default.fileExists(atPath: manifest.path) {
                    report["runtime_manifest_sha256"] = digest(try Data(contentsOf: manifest))
                }
            }
            var files: [String: Any] = [:]
            for name in ["input.json", "enclosure.FCStd", "tray.step", "lid.step", "tray.stl", "lid.stl"] {
                let data = try Data(contentsOf: output.appendingPathComponent(name))
                guard !data.isEmpty else { throw DesignError.message("An expected diagnostic output is empty.") }
                files[name] = ["bytes": data.count, "sha256": digest(data)]
            }
            report["files"] = files
            report["passed"] = true
            try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys, .prettyPrinted])
                .write(to: output.appendingPathComponent("device-report.json"), options: .withoutOverwriting)
        } catch {
            report["passed"] = false
            report["error"] = error.localizedDescription
        }
        return report
    }

    private static func milliseconds(since start: DispatchTime) -> Double {
        Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    }

    private static func digest(_ data: Data) -> String {
        SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    private static func settings(_ value: cc_settings) -> [String: Double] {
        ["margin": value.margin, "wall": value.wall, "floor": value.floor,
         "board_clearance": value.board_clearance, "standoff_height": value.standoff_height,
         "standoff_diameter": value.standoff_diameter, "lid_thickness": value.lid_thickness, "lid_gap": value.lid_gap]
    }
}
#endif

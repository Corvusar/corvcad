#if CORVCAD_DIAGNOSTICS
import Foundation

/// Development-only measurement hook for the LiveContainer probe tweak (tools/corvprobe). The
/// tweak lives in the same process and posts `org.corvcad.diagnostics` on the main thread with
/// `command`, `path` (relative to Documents) and a `reply` NSMutableDictionary this fills in.
/// Compiled only with the CORVCAD_DIAGNOSTICS condition (the gate 4 measurement build).
enum Diagnostics {
    private static var installed = false
    static func install() {
        guard !installed else { return }
        installed = true
        NotificationCenter.default.addObserver(forName: Notification.Name("org.corvcad.diagnostics"),
                                               object: nil, queue: nil) { note in
            guard let info = note.userInfo, let reply = info["reply"] as? NSMutableDictionary,
                  let command = info["command"] as? String else { return }
            let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            let path = documents.appendingPathComponent(info["path"] as? String ?? "").path
            switch command {
            case "mechanical-info": reply.addEntries(from: MechanicalDeviceDiagnostics.info())
            case "mechanical": reply.addEntries(from: MechanicalDeviceDiagnostics.run(requestPath: path))
            case "schematic": reply.addEntries(from: schematic(path))
            case "board": reply.addEntries(from: board(path))
            // KiCad's board writers (NATIVE_KICAD): files land in Documents/Diagnostics for /ls and /get.
            case "fab": reply.addEntries(from: written(path, "fab", [{ cc_board_native_fab_outputs(path, $0, 0, $1, $2) }]))
            case "board-plot": reply.addEntries(from: written(path, "plot", [{ cc_board_native_plot(path, $0, 0, $1, $2) },
                                                                             { cc_board_native_plot(path, $0, 1, $1, $2) }]))
            case "board-setup": reply.addEntries(from: written(path, nil, [{ _, e, c in cc_board_native_board_setup(path, e, c) }]))
            case "ratsnest": reply.addEntries(from: written(path, nil, [{ _, e, c in cc_board_native_ratsnest(path, e, c) }]))
            default: reply["error"] = "unknown command \(command)"
            }
            reply["footprint_mb"] = footprintMegabytes()
        }
        #if DEBUG && targetEnvironment(simulator)
        // Owned-simulator rehearsal of the same notification path, without the LAN tweak
        // or invented footprint measurements. Normal device/release builds have no hook.
        if let relative = ProcessInfo.processInfo.environment["CORVCAD_MECHANICAL_REQUEST"] {
            DispatchQueue.main.async {
                let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
                    .resolvingSymlinksInPath().standardizedFileURL
                let request = documents.appendingPathComponent(relative).resolvingSymlinksInPath().standardizedFileURL
                guard request.path.hasPrefix(documents.appendingPathComponent("Gate5").path + "/"),
                      request.lastPathComponent == "request.json" else {
                    NSLog("Mechanical simulator rehearsal rejected its request path")
                    return
                }
                let reply = NSMutableDictionary()
                NotificationCenter.default.post(name: Notification.Name("org.corvcad.diagnostics"), object: nil,
                    userInfo: ["command": "mechanical", "path": relative, "reply": reply])
                reply["rehearsal_only"] = true
                reply["physical_footprint_measured"] = false
                do {
                    try JSONSerialization.data(withJSONObject: reply, options: [.sortedKeys, .prettyPrinted])
                        .write(to: request.deletingLastPathComponent().appendingPathComponent("simulator-reply.json"),
                               options: .withoutOverwriting)
                } catch { NSLog("Mechanical simulator rehearsal could not retain its reply: %@", error.localizedDescription) }
            }
        }
        #endif
    }

    private static func milliseconds(since start: DispatchTime) -> Double {
        Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    }

    private static func text(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { cc_free_text(pointer) }
        return String(cString: pointer)
    }

    /// The app's physical memory footprint, as Xcode's memory gauge and jetsam count it.
    private static func footprintMegabytes() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        return status == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }

    /// Fresh Documents/Diagnostics/<file stem>-<kind>, or nil without a kind.
    private static func outputDirectory(for path: String, _ kind: String?) -> URL? {
        guard let kind else { return nil }
        let stem = URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent
        let directory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Diagnostics/\(stem)-\(kind)")
        try? FileManager.default.removeItem(at: directory)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        return directory
    }

    /// Files under a diagnostics directory (relative to Documents) with their sizes.
    private static func listing(_ directory: URL) -> [String: Int] {
        let parent = directory.deletingLastPathComponent().lastPathComponent + "/" + directory.lastPathComponent
        var files: [String: Int] = [:]
        let walk = FileManager.default.enumerator(at: directory, includingPropertiesForKeys: [.fileSizeKey])
        while let url = walk?.nextObject() as? URL {
            if let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize { files[parent + "/" + url.lastPathComponent] = size }
        }
        return files
    }

    /// Runs native writers (directory, error, capacity) -> text in order, each timed; with a kind
    /// they write into a fresh diagnostics directory, which is listed.
    private static func written(_ path: String, _ kind: String?,
                                _ writers: [(String, UnsafeMutablePointer<CChar>, Int) -> UnsafeMutablePointer<CChar>?]) -> [String: Any] {
        guard cc_board_native_backend() != nil else { return ["error": "KiCad's board code is not linked"] }
        let directory = outputDirectory(for: path, kind)
        var buffer = [CChar](repeating: 0, count: 512)
        var reply: [String: Any] = [:]
        for (index, write) in writers.enumerated() {
            let start = DispatchTime.now()
            let output = buffer.withUnsafeMutableBufferPointer { text(write(directory?.path ?? "", $0.baseAddress!, $0.count)) }
            reply["\(index)_ms"] = milliseconds(since: start)
            reply["\(index)"] = output ?? NSNull()
            if output == nil { reply["\(index)_error"] = String(cString: buffer) }
        }
        if let directory { reply["files"] = listing(directory) }
        return reply
    }

    /// Opens the project's schematic with the app's session backend (KiCad's own model in the
    /// NATIVE_KICAD edition), then asks it for KiCad's netlist and ERC report.
    private static func schematic(_ path: String) -> [String: Any] {
        var result = cc_sch_result()
        defer { cc_sch_result_clear(&result) }
        var reply: [String: Any] = [:]
        let start = DispatchTime.now()
        guard let session = cc_sch_session_open(path, &result) else {
            return ["error": result.message.map { String(cString: $0) } ?? "open failed"]
        }
        defer { cc_sch_session_free(session) }
        reply["open_ms"] = milliseconds(since: start)
        reply["backend"] = String(cString: cc_sch_session_backend(session))
        let netlistStart = DispatchTime.now()
        reply["netlist"] = text(cc_sch_session_netlist(session, &result)) ?? NSNull()
        reply["netlist_ms"] = milliseconds(since: netlistStart)
        let ercStart = DispatchTime.now()
        reply["erc"] = text(cc_sch_session_erc_json(session, &result)) ?? NSNull()
        reply["erc_ms"] = milliseconds(since: ercStart)
        let bomStart = DispatchTime.now()
        reply["bom"] = text(cc_sch_session_bom_csv(session, &result)) ?? NSNull()
        reply["bom_ms"] = milliseconds(since: bomStart)
        // KiCad's plot (CC_SCH_PLOT) into Documents/Diagnostics/<stem>-schplot: <stem>.pdf and the SVGs.
        if cc_sch_session_capabilities(session) & CC_SCH_PLOT.rawValue != 0,
           let directory = outputDirectory(for: path, "schplot") {
            let pdf = directory.appendingPathComponent(URL(fileURLWithPath: path).deletingPathExtension().lastPathComponent + ".pdf")
            for (format, output, key) in [(Int32(0), pdf.path, "plot_pdf"), (1, directory.path, "plot_svg")] {
                let plotStart = DispatchTime.now()
                reply[key] = cc_sch_session_plot(session, format, output, &result) == 1
                reply["\(key)_ms"] = milliseconds(since: plotStart)
            }
            reply["files"] = listing(directory)
        }
        if let message = result.message { reply["last_error"] = String(cString: message) }
        return reply
    }

    /// The portable board parser on a .kicad_pcb, as the PCB workspace loads it, then KiCad's
    /// DRC without and with a zone refill when the edition links KiCad's board code.
    private static func board(_ path: String) -> [String: Any] {
        guard let source = try? String(contentsOfFile: path, encoding: .utf8) else { return ["error": "cannot read \(path)"] }
        var buffer = [CChar](repeating: 0, count: 512)
        let capacity = buffer.count
        let start = DispatchTime.now()
        guard let board = source.withCString({ cc_board_parse($0, &buffer, capacity) }) else {
            return ["error": String(cString: buffer)]
        }
        defer { cc_board_free(board) }
        var reply: [String: Any] = ["parse_ms": milliseconds(since: start), "bytes": source.utf8.count,
                                    "footprints": cc_placement_count(board), "pads": cc_pad_count(board),
                                    "tracks": cc_track_count(board), "vias": cc_via_count(board), "nets": cc_net_count(board)]
        guard let backend = cc_board_native_backend() else { return reply }
        reply["native_backend"] = String(cString: backend)
        for (key, refill) in [("drc", Int32(0)), ("drc_refilled", Int32(1))] {
            let drcStart = DispatchTime.now()
            let report = text(cc_board_native_drc(path, refill, &buffer, capacity))
            reply["\(key)_ms"] = milliseconds(since: drcStart)
            reply[key] = report ?? NSNull()
            if report == nil { reply["\(key)_error"] = String(cString: buffer) }
        }
        return reply
    }
}
#endif

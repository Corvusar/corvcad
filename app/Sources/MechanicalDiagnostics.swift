#if DEBUG
import CryptoKit
import Foundation

/// Simulator assertions inspect actual session and output state without adding production controls.
extension Design {
    var mechanicalDiagnostic: String {
        func size(_ url: URL?) -> Int {
            guard let url, let attributes = try? FileManager.default.attributesOfItem(atPath: url.path) else { return 0 }
            return (attributes[.size] as? NSNumber)?.intValue ?? 0
        }
        let saved = mechanicalSavedURL.flatMap { try? Data(contentsOf: $0) }
        let value: [String: Any] = [
            "native": MechanicalSession.enabled, "ready": mechanicalReady,
            "error": mechanicalError ?? "", "dirty": mechanicalDirty,
            "canUndo": mechanicalCanUndo, "canRedo": mechanicalCanRedo,
            "trayTriangles": nativeTriangleCounts[0], "lidTriangles": nativeTriangleCounts[1],
            "margin": settings.margin, "wall": settings.wall, "floor": settings.floor,
            "board_clearance": settings.board_clearance, "standoff_height": settings.standoff_height,
            "standoff_diameter": settings.standoff_diameter, "lid_thickness": settings.lid_thickness,
            "lid_gap": settings.lid_gap, "boardThickness": mechanicalInput?.thickness ?? 0,
            "traySTLBytes": size(exportURL), "lidSTLBytes": size(lidExportURL),
            "documentBytes": saved?.count ?? 0,
            "documentSHA256": saved.map { SHA256.hash(data: $0).map { String(format: "%02x", $0) }.joined() } ?? "",
        ]
        return (try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]))
            .map { String(decoding: $0, as: UTF8.self) } ?? "{}"
    }
}
#endif

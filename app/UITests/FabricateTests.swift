import XCTest

/// The Fabricate workspace on FABRICATE_PROJECT (default Demo, the RoundTrip fixture), in both
/// orientations: specs, mesh exports in every edition, KiCad's outputs where KiCad is linked,
/// no placeholder for unbuilt outputs (drawings, slicer) and no STEP export outside FreeCAD. Pass SNAPSHOT_DIR to write PNGs.
final class FabricateTests: XCTestCase {
    func testFabricateSheet() throws {
        let project = ProcessInfo.processInfo.environment["FABRICATE_PROJECT"] ?? "Demo"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let side = orientation == .portrait ? "portrait" : "landscape"
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "fabricate"]
            app.launch()
            XCTAssertTrue(app.buttons["Fabricate"].firstMatch.waitForExistence(timeout: 30))
            XCTAssertFalse(app.buttons["Show inspector"].exists, "Fabricate is its own inspector")
            XCTAssertTrue(app.descendants(matching: .any)["enclosureMeasured"].waitForExistence(timeout: 30),
                          "enclosure measured")
            try capture("fabricate-\(side)")

            try export("traySTL", in: app, timeout: 30)
            try export("obj", in: app, timeout: 30)
            XCTAssertFalse(app.buttons["sendToSlicer"].exists, "no placeholder for unbuilt outputs")
            XCTAssertFalse(app.buttons["export-drawing"].exists, "no placeholder for unbuilt outputs")
            // The FreeCAD document tile is last; the lazy grid creates it once it is on screen.
            for _ in 0..<4 { app.swipeUp() }
            if app.buttons["export-fcstd"].exists {
                try export("step", in: app, timeout: 60)
                XCTAssertEqual(app.buttons["share-step"].label, "2 files", "tray and lid STEP")
            } else {
                XCTAssertFalse(app.buttons["export-step"].exists, "STEP needs FreeCAD's exact solids")
            }
            if app.buttons["export-fab"].exists {
                try export("fab", in: app, timeout: 120)
            }
            if app.buttons["export-schPDF"].exists {
                try export("schPDF", in: app, timeout: 60)
            }
            try capture("fabricate-\(side)-exported")
        }
    }

    /// Scrolls to an output tile, exports it and waits for its share button.
    private func export(_ id: String, in app: XCUIApplication, timeout: TimeInterval) throws {
        let button = app.buttons["export-\(id)"]
        for _ in 0..<6 where !button.isHittable { app.swipeUp() }
        XCTAssertTrue(button.waitForExistence(timeout: 5), "\(id) tile")
        button.tap()
        XCTAssertTrue(app.buttons["share-\(id)"].waitForExistence(timeout: timeout), "\(id) exported")
    }

    private func capture(_ name: String) throws {
        let shot = XCUIScreen.main.screenshot()
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent("\(name).png"))
        }
        let attachment = XCTAttachment(screenshot: shot)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}

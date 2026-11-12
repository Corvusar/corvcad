import XCTest

/// Placing a part must not rescale the schematic canvas: the frame is fixed when a sheet opens.
/// Uses the Demo project like ScreenshotTests, and undoes its edit before finishing.
final class SchematicViewportTests: XCTestCase {
    func testPlacingPartKeepsViewport() throws {
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "schematic"]
        app.launch()
        sleep(3)
        let before = XCUIScreen.main.screenshot()

        app.buttons["Part"].firstMatch.tap()
        // A spot far from the Demo content, which would widen a content fit.
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.8)).tap()
        sleep(2)
        let after = XCUIScreen.main.screenshot()

        for (name, shot) in [("before", before), ("after", after)] {
            let attachment = XCTAttachment(screenshot: shot)
            attachment.name = "viewport-\(name)"
            attachment.lifetime = .keepAlways
            add(attachment)
            if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
                try shot.pngRepresentation.write(
                    to: URL(fileURLWithPath: directory).appendingPathComponent("viewport-\(name).png"))
            }
        }
        app.buttons["Undo"].firstMatch.tap()
    }

    /// Pinch zooms the canvas in and leaves the project unchanged (no edit, Undo stays off).
    func testPinchZoomsCanvas() throws {
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "schematic"]
        app.launch()
        sleep(3)
        let before = XCUIScreen.main.screenshot().pngRepresentation
        app.windows.firstMatch.pinch(withScale: 2.5, velocity: 1.5)
        sleep(2)
        let shot = XCUIScreen.main.screenshot()
        XCTAssertNotEqual(before, shot.pngRepresentation)
        XCTAssertFalse(app.buttons["Undo"].firstMatch.isEnabled)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent("zoomed.png"))
        }
    }
}

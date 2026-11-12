import XCTest

/// Hold a placed part → Edit → change Value and Footprint → Save; reopening shows the saved
/// fields. Uses the Demo project and undoes both edits before finishing.
final class SymbolFieldsTests: XCTestCase {
    func testEditFieldsFromOrbitMenu() throws {
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "schematic"]
        app.launch()
        sleep(3)
        let spot = app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.8))
        app.buttons["Part"].firstMatch.tap()
        spot.tap()
        sleep(2)
        app.buttons["Select"].firstMatch.tap()

        spot.press(forDuration: 0.8)
        app.buttons["Edit"].firstMatch.tap()
        let value = app.textFields["10k"].firstMatch
        XCTAssertTrue(value.waitForExistence(timeout: 3))
        XCTAssertEqual(value.value as? String, "10k")
        value.tap()
        value.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 6) + "4k7")
        app.buttons["saveFields"].tap()
        sleep(2)

        spot.press(forDuration: 0.8)
        app.buttons["Edit"].firstMatch.tap()
        XCTAssertTrue(app.textFields["10k"].firstMatch.waitForExistence(timeout: 3))
        XCTAssertEqual(app.textFields["10k"].firstMatch.value as? String, "4k7")
        app.buttons["Cancel"].firstMatch.tap()
        sleep(1)

        // Rotate turns the part onto a vertical axis; the screenshot is for review.
        spot.press(forDuration: 0.8)
        app.buttons["Rotate"].firstMatch.tap()
        sleep(2)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation.write(
                to: URL(fileURLWithPath: directory).appendingPathComponent("rotated.png"))
        }

        for _ in 0..<3 {
            app.buttons["Undo"].firstMatch.tap()
            sleep(1)
        }
    }
}

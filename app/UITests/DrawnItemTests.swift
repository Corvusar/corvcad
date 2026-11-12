import XCTest

/// Draw a silkscreen text, then hold it → Edit (string) and hold → Move → tap elsewhere; the hold
/// menu follows the text to its new place. Uses the Demo project and undoes all three steps.
final class DrawnItemTests: XCTestCase {
    func testEditAndMoveDrawnText() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "pcb"]
        app.launch()
        let window = app.windows.firstMatch
        let first = window.coordinate(withNormalizedOffset: CGVector(dx: 0.35, dy: 0.86))
        let second = window.coordinate(withNormalizedOffset: CGVector(dx: 0.65, dy: 0.86))

        let draw = app.buttons["Draw"].firstMatch
        XCTAssertTrue(draw.waitForExistence(timeout: 5))
        draw.tap()
        if !app.segmentedControls["drawKind"].waitForExistence(timeout: 3) { draw.tap() }
        app.segmentedControls["drawKind"].buttons["Text"].tap()
        first.tap()
        sleep(2)
        app.buttons["Select"].firstMatch.tap()

        first.press(forDuration: 0.8)
        XCTAssertTrue(app.buttons["Edit"].firstMatch.waitForExistence(timeout: 3), "a drawn text offers Edit")
        app.buttons["Edit"].firstMatch.tap()
        let field = app.alerts.textFields.element(boundBy: 0)
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        XCTAssertEqual(field.value as? String, "CorvCAD")
        field.tap()
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 7) + "Rev B")
        app.alerts.buttons["Save"].tap()
        sleep(2)

        first.press(forDuration: 0.8)
        XCTAssertTrue(app.buttons["Move"].firstMatch.waitForExistence(timeout: 3))
        app.buttons["Move"].firstMatch.tap()
        XCTAssertTrue(app.buttons["cancelMoveDrawing"].waitForExistence(timeout: 3), "move mode shows its chip")
        second.tap()
        sleep(2)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation.write(
                to: URL(fileURLWithPath: directory).appendingPathComponent("drawn-text-moved.png"))
        }

        second.press(forDuration: 0.8)
        XCTAssertTrue(app.buttons["Edit"].firstMatch.waitForExistence(timeout: 3), "the text is at its new place")
        app.buttons["Edit"].firstMatch.tap()
        XCTAssertEqual(app.alerts.textFields.element(boundBy: 0).value as? String, "Rev B")
        app.alerts.buttons["Cancel"].tap()
        sleep(1)
        first.press(forDuration: 0.8)
        XCTAssertFalse(app.buttons["Edit"].firstMatch.waitForExistence(timeout: 2), "nothing left at the old place")
        second.tap() // closes a menu, if any

        for _ in 0..<3 {
            app.buttons["Undo"].firstMatch.tap()
            sleep(1)
        }
    }
}

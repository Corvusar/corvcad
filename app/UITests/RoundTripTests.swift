import XCTest

/// Builds the KiCad round-trip project by touch only: three parts (one rotated), a wire the
/// router detours around both part bodies, a tee onto another wire, two NET1 labels, three
/// board footprints (one rotated) and two M3 holes, then Update from schematic sets pad nets. A "RoundTrip" project must not exist yet.
/// `integrations/kicad_cli/round_trip.py` then checks the files with KiCad.
final class RoundTripTests: XCTestCase {
    /// Taps at KiCad millimetres, mapped the way the editors fit `box` into the canvas.
    private func at(_ canvas: XCUIElement, _ box: CGRect, _ x: Double, _ y: Double) -> XCUICoordinate {
        let size = canvas.frame.size
        let scale = min(size.width / box.width, size.height / box.height)
        let dx = (size.width - box.width * scale) / 2 + (x - box.minX) * scale
        let dy = (size.height - box.height * scale) / 2 + (y - box.minY) * scale
        return canvas.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: dx, dy: dy))
    }

    private func snapshot(_ name: String) throws {
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation.write(
                to: URL(fileURLWithPath: directory).appendingPathComponent("roundtrip-\(name).png"))
        }
    }

    func testAuthorRoundTripProject() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launch()
        sleep(2)
        app.buttons["New project"].firstMatch.tap()
        let name = app.textFields["Sensor board"].firstMatch
        XCTAssertTrue(name.waitForExistence(timeout: 3))
        name.tap()
        name.typeText("RoundTrip")
        app.buttons["Create project"].tap()

        let canvas = app.otherElements["canvas"].firstMatch
        XCTAssertTrue(canvas.waitForExistence(timeout: 5))
        sleep(1)
        // A blank sheet is fitted to 160 x 120 mm and stays fixed while editing.
        let sheet = CGRect(x: 0, y: 0, width: 160, height: 120)
        func tap(_ x: Double, _ y: Double) { at(canvas, sheet, x, y).tap(); sleep(1) }

        app.buttons["Part"].firstMatch.tap()
        tap(50.8, 50.8)   // R1, pins at x 45.72 and 55.88
        tap(101.6, 50.8)  // R2, pins at x 96.52 and 106.68
        tap(76.2, 25.4)   // R3
        app.buttons["Select"].firstMatch.tap()
        at(canvas, sheet, 76.2, 25.4).press(forDuration: 0.8)
        app.buttons["Rotate"].firstMatch.tap()  // R3 pins now at y 20.32 and 30.48
        sleep(1)

        app.buttons["Wire"].firstMatch.tap()
        tap(45.72, 50.8); tap(106.68, 50.8)  // straight line crosses both bodies: detoured
        tap(55.88, 50.8); tap(96.52, 50.8)
        tap(76.2, 30.48); tap(76.2, 50.8)    // ends inside the previous wire: tee junction
        app.buttons["Label"].firstMatch.tap()
        tap(76.2, 20.32)
        tap(45.72, 50.8)
        // Select the detour's long segment: its net is the NET1 group of three pins.
        app.buttons["Select"].firstMatch.tap()
        tap(91.44, 48.26)
        XCTAssertTrue(app.staticTexts["/NET1"].waitForExistence(timeout: 3))
        XCTAssertTrue(app.staticTexts["3 PINS"].exists || app.staticTexts["3 pins"].exists)
        try snapshot("schematic")

        app.buttons["PCB"].firstMatch.tap()
        sleep(2)
        let board = CGRect(x: 0, y: 0, width: 80, height: 60)
        func place(_ x: Double, _ y: Double) { at(canvas, board, x, y).tap(); sleep(1) }
        app.buttons["Part"].firstMatch.tap()
        place(20, 20); place(40, 20); place(60, 20)
        app.buttons["Select"].firstMatch.tap()
        at(canvas, board, 60, 20).press(forDuration: 0.8)
        app.buttons["Rotate"].firstMatch.tap()
        sleep(1)
        app.buttons["M3"].firstMatch.tap()
        place(8, 52); place(72, 52)
        // Update PCB from schematic: R1-R3 already exist, so only their pad nets change.
        app.buttons["updateFromSchematic"].tap()
        let done = app.alerts["Update PCB"]
        XCTAssertTrue(done.waitForExistence(timeout: 3))
        XCTAssertTrue(done.staticTexts["Pad nets now match the schematic."].exists)
        done.buttons["OK"].tap()

        // Route both nets. Pads: R1 (19.2|20.8, 20), R2 (39.2|40.8, 20), R3 at 90 deg (60, 20.8|19.2).
        // Free corners continue the track; ending on a pad or track end finishes it.
        app.buttons["Track"].firstMatch.tap()
        place(20.8, 20); place(39.2, 20)                                   // Net-(R1-Pad2), F.Cu
        place(39.2, 20); place(39.2, 25); place(60, 25); place(60, 20.8)   // on to R3 pin 1
        place(19.2, 20); place(19.2, 15); place(40.8, 15); place(40.8, 20) // /NET1 to R2 pin 2
        place(40.8, 15); place(50, 15)                                     // toward R3 pin 2...
        app.buttons["Done"].firstMatch.tap()
        app.buttons["Via"].firstMatch.tap()
        place(50, 15)                                                      // ...through B.Cu
        app.buttons["Track"].firstMatch.tap()
        app.buttons["B.Cu"].firstMatch.tap()
        place(50, 15); place(55, 15)
        app.buttons["Done"].firstMatch.tap()
        app.buttons["Via"].firstMatch.tap()
        place(55, 15)
        app.buttons["Track"].firstMatch.tap()
        app.buttons["F.Cu"].firstMatch.tap()
        place(55, 15); place(60, 15); place(60, 19.2)
        try snapshot("pcb")

        // Enclosure inspector sliders (bindings into the board model): one change, undo, redo.
        app.buttons["Enclosure"].firstMatch.tap()
        let show = app.buttons["Show inspector"]
        if show.waitForExistence(timeout: 3) { show.tap() }
        let slider = app.sliders.firstMatch
        XCTAssertTrue(slider.waitForExistence(timeout: 5))
        let before = slider.value as? String
        slider.adjust(toNormalizedSliderPosition: 0.9)
        sleep(2)
        let after = slider.value as? String
        XCTAssertNotEqual(before, after)
        app.buttons["Undo"].firstMatch.tap()
        sleep(2)
        XCTAssertEqual(slider.value as? String, before)
        app.buttons["Redo"].firstMatch.tap()
        sleep(2)
        XCTAssertEqual(slider.value as? String, after)
        try snapshot("enclosure")
    }
}

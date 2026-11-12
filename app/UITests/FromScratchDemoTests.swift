import XCTest

/// Local iMac capture rehearsals. Projects are created through the app UI; no demo files
/// are injected. Keep failures fatal so an incomplete take cannot look successful.
final class FromScratchDemoTests: XCTestCase {
    private var app = XCUIApplication()
    private let sheet = CGRect(x: 0, y: 0, width: 160, height: 120)
    override func setUpWithError() throws {
        continueAfterFailure = false
        guard ProcessInfo.processInfo.environment["CAPTURE_DEMO"] == "1" else {
            throw XCTSkip("Opt-in local capture: set TEST_RUNNER_CAPTURE_DEMO=1")
        }
    }

    override func tearDownWithError() throws {
        guard ProcessInfo.processInfo.environment["CAPTURE_DEMO"] == "1" else { return }
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            let current = XCUIApplication()
            if current.state != .notRunning {
                try current.debugDescription.write(toFile: directory + "/last-tree.txt", atomically: true, encoding: .utf8)
            }
            try capture("last-state")
        }
    }

    func testCapturePreflight() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launch()
        XCTAssertTrue(app.buttons["New project"].firstMatch.waitForExistence(timeout: 20))
        try capture("home")
        app.buttons["New project"].firstMatch.tap()
        let name = app.textFields["Sensor board"].firstMatch
        XCTAssertTrue(name.waitForExistence(timeout: 5))
        name.tap()
        name.typeText("LED Switch Rehearsal 01\n")
        Thread.sleep(forTimeInterval: 1)
        try capture("new-project-form")
        app.buttons["Create project"].tap()
        XCTAssertTrue(app.otherElements["canvas"].firstMatch.waitForExistence(timeout: 20))
        try capture("blank")
        app.buttons["Part"].firstMatch.tap()
        XCTAssertTrue(app.buttons["choosePart"].waitForExistence(timeout: 5), "Native library picker required")
        app.buttons["choosePart"].tap()
        XCTAssertTrue(app.textFields["partSearch"].waitForExistence(timeout: 5))
        app.textFields["partSearch"].tap()
        app.textFields["partSearch"].typeText("Conn_01x02 Connector_Generic")
        try capture("picker")
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try app.debugDescription.write(toFile: directory + "/preflight-tree.txt", atomically: true, encoding: .utf8)
        }
    }

    func testMotionPreflight() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launchEnvironment["CORVCAD_OPEN"] = ProcessInfo.processInfo.environment["DEMO_PROJECT"] ?? "LED Switch Rehearsal 01"
        app.launchEnvironment["CORVCAD_MODE"] = "schematic"
        app.launch()
        let canvas = app.otherElements["canvas"].firstMatch
        XCTAssertTrue(canvas.waitForExistence(timeout: 20))
        Thread.sleep(forTimeInterval: 3)
        for _ in 0..<3 {
            canvas.pinch(withScale: 1.5, velocity: 0.12)
            canvas.pinch(withScale: 1 / 1.5, velocity: -0.12)
        }
        try capture("motion-end")
    }

    func testPrepareLandscape() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        XCUIDevice.shared.press(.home)
        Thread.sleep(forTimeInterval: 2)
        try capture("landscape-springboard")
    }

    func testAuthorCircuit() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        app.launch()
        XCTAssertTrue(app.buttons["New project"].firstMatch.waitForExistence(timeout: 20))
        try capture("01-home")
        app.buttons["New project"].firstMatch.tap()
        let name = app.textFields["Sensor board"].firstMatch
        XCTAssertTrue(name.waitForExistence(timeout: 5))
        name.tap()
        let project = ProcessInfo.processInfo.environment["DEMO_PROJECT"] ?? "LED Switch Rehearsal 04"
        name.typeText(project + "\n")
        replace(app.textFields["Width mm"], with: "40\n")
        replace(app.textFields["Height mm"], with: "30\n")
        app.buttons["Create project"].tap()
        XCTAssertTrue(canvas.waitForExistence(timeout: 20))
        try capture("02-blank")

        try place("Conn_01x02", library: "Connector_Generic", at: CGPoint(x: 40.64, y: 30.48),
                  footprint: "PinHeader_1x02_P2.54mm_Vertical", value: "5V POWER")
        try place("Conn_01x02", library: "Connector_Generic", at: CGPoint(x: 40.64, y: 76.2),
                  footprint: "PinHeader_1x02_P2.54mm_Vertical", value: "LOGIC IN")
        try place("R", library: "Device", at: CGPoint(x: 66.04, y: 76.2),
                  footprint: "R_0805_2012Metric", value: "1k")
        try place("R", library: "Device", at: CGPoint(x: 109.22, y: 25.4),
                  footprint: "R_0805_2012Metric", value: "330")
        try place("LED", library: "Device", at: CGPoint(x: 109.22, y: 45.72),
                  footprint: "LED_0805_2012Metric", value: "RED")
        try place("BC547", library: "Transistor_BJT", at: CGPoint(x: 106.68, y: 76.2),
                  footprint: nil, value: "BC547")
        app.buttons["Select"].firstMatch.tap()
        try action("Move", at: CGPoint(x: 40.64, y: 30.48))
        tap(35.56, 30.48)
        try action("Move", at: CGPoint(x: 40.64, y: 76.2))
        tap(35.56, 76.2)
        try action("Rotate", at: CGPoint(x: 66.04, y: 76.2))
        try action("Rotate", at: CGPoint(x: 109.22, y: 45.72))
        try capture("03-parts-arranged")
        try finishCircuit()
    }

    func testPCBRehearsal() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        app.launchEnvironment["CORVCAD_OPEN"] = "LED Switch Rehearsal 04"
        app.launchEnvironment["CORVCAD_MODE"] = "pcb"
        app.launch()
        XCTAssertTrue(canvas.waitForExistence(timeout: 20))
        try layoutPCB()
    }

    func testRoutePCBRehearsal() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        app.launchEnvironment["CORVCAD_OPEN"] = "LED Switch Rehearsal 04"
        app.launchEnvironment["CORVCAD_MODE"] = "pcb"
        app.launch()
        XCTAssertTrue(canvas.waitForExistence(timeout: 20))
        try routePCB()
    }

    private func finishCircuit() throws {
        app.buttons["Wire"].firstMatch.tap()
        wire([(30.48,30.48), (20.32,30.48), (20.32,12.7), (109.22,12.7), (109.22,21.59)])
        wire([(109.22,29.21), (109.22,41.91)])
        wire([(109.22,49.53), (109.22,71.12)])
        wire([(30.48,76.2), (22.86,76.2), (22.86,63.5), (55.88,63.5), (55.88,76.2), (62.23,76.2)])
        wire([(69.85,76.2), (101.6,76.2)])
        wire([(30.48,33.02), (10.16,33.02), (10.16,91.44)])
        wire([(30.48,78.74), (10.16,78.74)])
        wire([(109.22,81.28), (109.22,91.44), (10.16,91.44)])
        app.buttons["Label"].firstMatch.tap()
        replace(app.textFields["NET1"].firstMatch, with: "+5V\n")
        tap(109.22, 12.7)
        replace(app.textFields["NET1"].firstMatch, with: "CTRL\n")
        tap(55.88, 63.5)
        replace(app.textFields["NET1"].firstMatch, with: "GND\n")
        tap(109.22, 91.44)
        app.buttons["Select"].firstMatch.tap()
        XCTAssertEqual(app.descendants(matching: .any)["placeholderProbe"].label, "0")
        try capture("04-schematic")
        app.buttons["Show inspector"].tap()
        let erc = app.buttons["runERC"]
        XCTAssertTrue(erc.waitForExistence(timeout: 5))
        erc.tap()
        Thread.sleep(forTimeInterval: 3)
        try capture("05-erc")
        try app.debugDescription.write(toFile: output("schematic-tree.txt"), atomically: true, encoding: .utf8)
        XCTAssertEqual(app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error:' OR label BEGINSWITH 'Warning:'")).count, 0,
                       "Resolve ERC findings before the PCB transfer")
        readyTap(app.buttons["exportNetlist"])
        Thread.sleep(forTimeInterval: 1)
        app.buttons["Hide inspector"].tap()
        app.buttons["PCB"].firstMatch.tap()
        XCTAssertTrue(app.buttons["updateFromSchematic"].waitForExistence(timeout: 10))
        app.buttons["updateFromSchematic"].tap()
        XCTAssertTrue(app.alerts["Update PCB"].waitForExistence(timeout: 20))
        try capture("06-update-report")
        app.alerts["Update PCB"].buttons["OK"].tap()
        Thread.sleep(forTimeInterval: 2)
        let probe = app.descendants(matching: .any)["ratsnestProbe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 10))
        try capture("07-new-pcb")
        try layoutPCB()
    }

    private func layoutPCB() throws {
        readyTap(app.buttons["Show inspector"])
        let scroll = app.scrollViews.firstMatch
        Thread.sleep(forTimeInterval: 1)
        for _ in 0..<5 {
            if app.textFields["X mm"].isHittable { break }
            scroll.swipeUp()
        }
        for (ref, x, y) in [("J1", "5", "6"), ("J2", "5", "23"), ("R1", "16", "22"),
                            ("R2", "28", "5"), ("D1", "28", "10"), ("Q1", "27", "21")] {
            let reference = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Reference,'")).firstMatch
            readyTap(reference)
            try app.debugDescription.write(toFile: output("pcb-reference-menu.txt"), atomically: true, encoding: .utf8)
            let choice = app.buttons[ref].firstMatch
            XCTAssertTrue(choice.waitForExistence(timeout: 5))
            readyTap(choice)
            replace(app.textFields["X mm"], with: x + "\n")
            replace(app.textFields["Y mm"], with: y + "\n")
            readyTap(app.buttons["Apply move"])
            Thread.sleep(forTimeInterval: 1)
            XCTAssertFalse(app.alerts.firstMatch.exists, app.alerts.debugDescription)
        }
        readyTap(app.buttons["Hide inspector"])
        Thread.sleep(forTimeInterval: 1)
        try capture("08-pcb-layout")
        let probe = app.descendants(matching: .any)["ratsnestProbe"]
        XCTAssertGreaterThan(numbers(probe.label).last ?? 0, 0, "Placed board must have unrouted nets")
        try routePCB()
    }

    private func routePCB() throws {
        let board = CGRect(x: 0, y: 0, width: 40, height: 30)
        try action("Move", at: CGPoint(x: 5, y: 6), box: board)
        at(CGPoint(x: 5.5, y: 6), box: board).tap()
        try action("Rotate", at: CGPoint(x: 16, y: 22), box: board)
        Thread.sleep(forTimeInterval: 1)
        try capture("09-footprint-rotation")
        readyTap(app.buttons["Undo"].firstMatch)
        readyTap(app.buttons["Track"].firstMatch)
        XCTAssertTrue(app.segmentedControls["routeMode"].waitForExistence(timeout: 5))
        let probe = app.descendants(matching: .any)["ratsnestProbe"]
        for index in 0..<16 {
            let before = numbers(probe.label)
            XCTAssertEqual(before.count, 7)
            if before[6] == 0 { break }
            for pair in [(before[0], before[1]), (before[2], before[3])] {
                app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: pair.0, dy: pair.1)).tap()
                Thread.sleep(forTimeInterval: 0.8)
                XCTAssertFalse(app.alerts.firstMatch.exists, app.alerts.debugDescription)
            }
            let after = numbers(probe.label)
            try capture("route-\(index + 1)")
            XCTAssertLessThan(after[6], before[6], "Each committed route must reduce unrouted connections")
        }
        XCTAssertEqual(numbers(probe.label).last, 0)
        readyTap(app.buttons["Select"].firstMatch)
        readyTap(app.buttons["Show inspector"])
        let drc = app.buttons["runDRC"]
        for _ in 0..<5 {
            if drc.isHittable { break }
            app.scrollViews.firstMatch.swipeDown()
        }
        readyTap(drc)
        Thread.sleep(forTimeInterval: 3)
        try capture("10-drc")
        try app.debugDescription.write(toFile: output("drc-tree.txt"), atomically: true, encoding: .utf8)
        XCTAssertEqual(app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error:' OR label BEGINSWITH 'Warning:'")).count, 0,
                       "Resolve DRC findings before the final PCB reveal")
        readyTap(app.buttons["Hide inspector"])
        at(CGPoint(x: 20, y: 15), box: board).tap()
        Thread.sleep(forTimeInterval: 2)
        try capture("11-final-pcb")
        Thread.sleep(forTimeInterval: 6)
        try "complete\n".write(toFile: output("take-complete.txt"), atomically: true, encoding: .utf8)
    }

    private var canvas: XCUIElement { app.otherElements["canvas"].firstMatch }

    private func at(_ point: CGPoint, box: CGRect? = nil) -> XCUICoordinate {
        let bounds = box ?? sheet
        let size = canvas.frame.size
        let scale = min(size.width / bounds.width, size.height / bounds.height)
        let dx = (size.width - bounds.width * scale) / 2 + (point.x - bounds.minX) * scale
        let dy = (size.height - bounds.height * scale) / 2 + (point.y - bounds.minY) * scale
        return canvas.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: dx, dy: dy))
    }

    private func tap(_ x: Double, _ y: Double) {
        at(CGPoint(x: x, y: y)).tap()
        Thread.sleep(forTimeInterval: 0.25)
        XCTAssertFalse(app.alerts.firstMatch.exists, app.alerts.debugDescription)
    }

    private func replace(_ field: XCUIElement, with text: String) {
        XCTAssertTrue(field.waitForExistence(timeout: 5))
        field.tap()
        let old = field.value as? String ?? ""
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: old.count) + text)
        Thread.sleep(forTimeInterval: 0.5)
    }

    private func action(_ name: String, at point: CGPoint, box: CGRect? = nil) throws {
        at(point, box: box).press(forDuration: 0.65)
        XCTAssertTrue(app.buttons[name].firstMatch.waitForExistence(timeout: 5))
        app.buttons[name].firstMatch.tap()
        Thread.sleep(forTimeInterval: 0.4)
    }

    private func place(_ name: String, library: String, at point: CGPoint,
                       footprint: String?, value: String) throws {
        readyTap(app.buttons["Part"].firstMatch)
        readyTap(app.buttons["choosePart"])
        let search = app.textFields["partSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        search.tap()
        search.typeText(name + " " + library)
        let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@ AND label CONTAINS[c] %@", name, library)).firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 10))
        row.tap()
        at(point).tap()
        if let footprint {
            let search = app.textFields["footprintSearch"]
            XCTAssertTrue(search.waitForExistence(timeout: 10))
            search.tap()
            search.typeText(footprint)
            let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", footprint)).firstMatch
            if !row.waitForExistence(timeout: 2) {
                app.switches.firstMatch.tap()
            }
            XCTAssertTrue(row.waitForExistence(timeout: 5))
            row.tap()
        }
        Thread.sleep(forTimeInterval: 1)
        readyTap(app.buttons["Select"].firstMatch)
        try action("Edit", at: point)
        replace(app.textFields["10k"].firstMatch, with: value + "\n")
        XCTAssertEqual(app.textFields["10k"].firstMatch.value as? String, value)
        readyTap(app.buttons["saveFields"])
        let dismissed = XCTNSPredicateExpectation(predicate: NSPredicate(format: "exists == false"), object: app.buttons["saveFields"])
        XCTAssertEqual(XCTWaiter.wait(for: [dismissed], timeout: 5), .completed)
        Thread.sleep(forTimeInterval: 1)
        XCTAssertFalse(app.alerts.firstMatch.exists, app.alerts.debugDescription)
        try capture("placed-" + name + "-" + value.replacingOccurrences(of: " ", with: "_"))
    }

    private func readyTap(_ element: XCUIElement) {
        for _ in 0..<40 {
            if element.exists && element.isHittable && element.isEnabled { element.tap(); return }
            Thread.sleep(forTimeInterval: 0.15)
        }
        XCTFail("Required control is not hittable: \(element)")
    }

    private func wire(_ points: [(Double, Double)]) {
        for index in 1..<points.count {
            tap(points[index - 1].0, points[index - 1].1)
            tap(points[index].0, points[index].1)
        }
    }

    private func numbers(_ label: String) -> [Double] {
        label.split(separator: " ").compactMap { Double($0) }
    }

    private func output(_ name: String) -> String {
        (ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] ?? NSTemporaryDirectory()) + "/" + name
    }

    private func capture(_ name: String) throws {
        let shot = XCUIScreen.main.screenshot()
        let attachment = XCTAttachment(screenshot: shot)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(name + ".png"))
        }
    }
}

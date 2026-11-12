import XCTest

/// Takes for the feature showcase video, recorded on the iMac's simulator (tools/showcase/).
/// Opt-in: TEST_RUNNER_SHOWCASE=1. Each take prints "SHOWCASE-MARK <name> <unix time>" lines that
/// the editor aligns to the music. Projects are seeded by tools/showcase/record.py --seed: KiCad's
/// kit-dev-coldfire-xilinx_5213 demo and "sim-rectifier" (KiCad's rectifier demo with the
/// engine's fixture board). Takes A-D need the NATIVE_KICAD edition, E-H the NATIVE_FREECAD one.
final class ShowcaseTests: XCTestCase {
    private var app = XCUIApplication()

    override func setUpWithError() throws {
        continueAfterFailure = false
        guard ProcessInfo.processInfo.environment["SHOWCASE"] == "1" else {
            throw XCTSkip("Opt-in showcase take: set TEST_RUNNER_SHOWCASE=1")
        }
        XCUIDevice.shared.orientation = .landscapeLeft
    }

    // MARK: A. Schematic on KiCad's ColdFire/Xilinx kit demo, then the board

    static let project = "kit-dev-coldfire-xilinx_5213"
    /// The root sheet's clear area (x 231-300, y 170-257 mm on A3) gains an analog input buffer:
    /// an LM358 non-inverting amplifier (gain 2), a series resistor to the output and decoupling.
    /// Laid out on the 2.54 mm grid relative to this origin.
    private static let dx = 200.66, dy = -50.8
    private func at(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: x + Self.dx, y: y + Self.dy) }

    func testA_SchematicToBoard() throws {
        launch(project: Self.project, mode: "schematic")
        XCTAssertTrue(canvas.waitForExistence(timeout: 60))
        pause(4)
        mark("root")
        // Down into the Xilinx CPLD sheet (its box: 332.74, 143.51, 53.34 x 31.75 mm; pins line its
        // sides), opened from the box's hold menu, then back up.
        point(359.41, 171.0).press(forDuration: 0.7)
        readyTap(app.buttons["Open"].firstMatch)
        pause(4)
        mark("sheet")
        let root = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'kit-dev' OR label == 'Root'")).firstMatch
        readyTap(root)
        pause(3)
        mark("back")
        // Closer in (about the canvas centre; the clear area stays on screen), so pin snapping is tight.
        canvas.pinch(withScale: 1.6, velocity: 0.8)
        pause(1.5)
        mark("zoomed")
        try place("LM358", library: "Amplifier_Operational", at: at(63.5, 238.76), footprint: "SOIC-8_3.9x4.9mm_P1.27mm", value: nil)
        mark("opamp")
        try place("R", library: "Device", at: at(78.74, 246.38), footprint: "R_0603_1608Metric", value: "10k")
        try place("R", library: "Device", at: at(48.26, 261.62), footprint: "R_0603_1608Metric", value: "10k")
        try place("R", library: "Device", at: at(91.44, 246.38), footprint: "R_0603_1608Metric", value: "100")
        try place("C", library: "Device", at: at(30.48, 261.62), footprint: "C_0603_1608Metric", value: "100n")
        mark("parts")
        try power("+5V", at: [at(30.48, 247.65)])
        try power("GND", at: [at(30.48, 275.59), at(48.26, 275.59)])
        mark("power")
        readyTap(app.buttons["Wire"].firstMatch)
        // Pin to pin: the Wire tool routes each at right angles around the parts by itself.
        for segment in [((71.12, 238.76), (78.74, 242.57)),   // output to the feedback resistor
                        ((78.74, 242.57), (91.44, 242.57)),   // ... and on to the series resistor
                        ((91.44, 250.19), (104.14, 250.19)),  // series resistor to AIN_BUF
                        ((55.88, 241.3), (48.26, 257.81)),    // - input to the gain resistor
                        ((78.74, 250.19), (48.26, 257.81)),   // feedback to the same node
                        ((48.26, 265.43), (48.26, 275.59)), ((30.48, 247.65), (30.48, 257.81)),
                        ((30.48, 265.43), (30.48, 275.59))] {
            let a = at(segment.0.0, segment.0.1), b = at(segment.1.0, segment.1.1)
            if tapMM(a.x, a.y) || tapMM(b.x, b.y) {
                // A refused wire leaves its start pending: reset the tool so later taps stay paired.
                readyTap(app.buttons["Select"].firstMatch)
                readyTap(app.buttons["Wire"].firstMatch)
            }
            pause(0.35)
        }
        mark("wired")
        readyTap(app.buttons["Label"].firstMatch)
        for (name, x, y) in [("AIN", 55.88, 236.22), ("AIN_BUF", 104.14, 250.19)] {
            replace(app.textFields["NET1"].firstMatch, with: name + "\n")
            let p = at(x, y)
            tapMM(p.x, p.y)
            pause(0.6)
        }
        mark("labels")
        readyTap(app.buttons["Select"].firstMatch)
        let output = at(85.09, 242.57)
        tapMM(output.x, output.y)  // the output net, highlighted
        pause(2.5)
        mark("netHighlight")
        readyTap(app.buttons["Show inspector"])
        let annotate = app.buttons["annotate"]
        reveal(annotate)
        readyTap(annotate)
        pause(2)
        if app.alerts.firstMatch.exists { app.alerts.firstMatch.buttons.firstMatch.tap() }
        mark("annotated")
        let erc = app.buttons["runERC"]
        reveal(erc, upward: false)
        readyTap(erc)
        let finding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
        _ = finding.firstMatch.waitForExistence(timeout: 120)
        pause(2)
        mark("erc")
        if finding.firstMatch.exists { finding.firstMatch.tap(); pause(2.5) }
        readyTap(app.buttons["Hide inspector"])
        pause(1)

        // Natively over to the board: the same project's PCB, updated from the schematic.
        readyTap(app.buttons["PCB"].firstMatch)
        let update = app.buttons["updateFromSchematic"]
        XCTAssertTrue(update.waitForExistence(timeout: 60))
        pause(3)
        mark("pcb")
        let before = Set(footprints().map(\.ref))
        readyTap(update)
        XCTAssertTrue(app.alerts["Update PCB"].waitForExistence(timeout: 120))
        pause(2.5)
        mark("updateReport")
        app.alerts["Update PCB"].buttons["OK"].tap()
        pause(2)
        let added = footprints().filter { !before.contains($0.ref) }.sorted { order($0.ref) < order($1.ref) }
        XCTAssertGreaterThanOrEqual(added.count, 5, "the five new parts: \(added.map(\.ref))")
        mark("updated")
        // Placed in the board's clear corner (x 186.6-203.6, y 126.9-144.9 mm).
        let spots = [CGPoint(x: 196, y: 136), CGPoint(x: 189.5, y: 131), CGPoint(x: 189.5, y: 135.5), CGPoint(x: 189.5, y: 140),
                     CGPoint(x: 200.5, y: 142)]
        print("SHOWCASE-INFO added \(added.map { "\($0.ref)@\($0.x),\($0.y)" })")
        // Placed through the board inspector's footprint picker and X/Y fields (KiCad's move).
        readyTap(app.buttons["Select"].firstMatch)
        readyTap(app.buttons["Show inspector"])
        let xField = app.textFields["X mm"]
        for _ in 0..<6 where !xField.isHittable { app.scrollViews.firstMatch.swipeUp(velocity: .slow) }
        for (part, spot) in zip(added, spots) {
            readyTap(app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Reference,'")).firstMatch)
            // The menu lists every footprint (160 here): scroll it until the new one shows.
            let item = app.buttons[part.ref].firstMatch
            for _ in 0..<30 where !item.isHittable { app.collectionViews.firstMatch.swipeUp(velocity: .slow) }
            readyTap(item)
            // The fields use the device's decimal separator (a comma on this simulator).
            let comma = (app.textFields["X mm"].value as? String ?? "").contains(",")
            func mm(_ v: Double) -> String {
                let s = String(format: "%.2f\n", v)
                return comma ? s.replacingOccurrences(of: ".", with: ",") : s
            }
            replace(app.textFields["X mm"], with: mm(spot.x))
            replace(app.textFields["Y mm"], with: mm(spot.y))
            readyTap(app.buttons["Apply move"])
            pause(1.0)
            if app.alerts.firstMatch.exists { app.alerts.firstMatch.buttons.firstMatch.tap() }
        }
        readyTap(app.buttons["Hide inspector"])
        pause(1.5)
        mark("placed")
        readyTap(app.buttons["Track"].firstMatch)
        XCTAssertTrue(app.segmentedControls["routeMode"].waitForExistence(timeout: 10))
        for _ in 0..<10 {
            let local = unrouted().first { [$0.0, $0.2].allSatisfy { 185...205 ~= $0 } && [$0.1, $0.3].allSatisfy { 126...146 ~= $0 } }
            guard let edge = local else { break }
            let count = unrouted().count
            pcbPoint(edge.0, edge.1).tap()
            pause(0.9)
            pcbPoint(edge.2, edge.3).tap()
            pause(2.0)
            if app.alerts.firstMatch.exists { app.alerts.firstMatch.buttons.firstMatch.tap(); break }
            if unrouted().count >= count { break }
        }
        mark("routed")
        readyTap(app.buttons["Select"].firstMatch)
        readyTap(app.buttons["Show inspector"])
        let drc = app.buttons["runDRC"]
        reveal(drc, upward: false)
        readyTap(drc)
        let drcFinding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
        _ = drcFinding.firstMatch.waitForExistence(timeout: 180)
        pause(2)
        mark("drc")
        if drcFinding.firstMatch.exists { drcFinding.firstMatch.tap(); pause(2.5) }
        let fill = app.buttons["fillZones"]
        if fill.exists {
            reveal(fill, upward: false)
            readyTap(fill)
            pause(8)
            mark("zonesFilled")
        }
        readyTap(app.buttons["Hide inspector"])
        pause(4)
        mark("end")
    }

    // MARK: B. KiCad's simulator, and the board electrified

    func testB_Simulation() throws {
        launch(project: "sim-rectifier", mode: "schematic")
        XCTAssertTrue(canvas.waitForExistence(timeout: 60))
        pause(3)
        mark("schematic")
        readyTap(app.buttons["Show inspector"])
        let run = app.buttons["runSimulation"]
        reveal(run)
        readyTap(run)
        mark("run")
        let onBoard = app.buttons["showOnBoard"]
        XCTAssertTrue(onBoard.waitForExistence(timeout: 180), "the run ended with a plot")
        pause(2)
        mark("plot")
        readyTap(app.buttons["Hide inspector"])
        pause(4)
        mark("plotFull")
        readyTap(onBoard)
        XCTAssertTrue(app.staticTexts["electrifySummary"].waitForExistence(timeout: 30))
        mark("electrify")
        app.descendants(matching: .any)["enclosurePreview"].pinch(withScale: 2.2, velocity: 0.5)  // the copper fills the frame
        pause(6)
        orbit(app.descendants(matching: .any)["enclosurePreview"], dx: 0.25, dy: -0.05)
        pause(8)
        orbit(app.descendants(matching: .any)["enclosurePreview"], dx: -0.35, dy: 0.08)
        pause(8)
        mark("end")
    }

    // MARK: C. Fabrication with KiCad's writers

    func testC_FabricateKiCad() throws {
        launch(project: Self.project, mode: "fabricate")
        XCTAssertTrue(app.descendants(matching: .any)["enclosureMeasured"].waitForExistence(timeout: 120))  // tiles are lazy, below
        pause(3)
        mark("fabricate")
        for id in ["fab", "boardPDF", "schPDF", "bom", "netlist", "traySTL"] {
            try export(id, timeout: 300)
            mark("exported-" + id)
            pause(1)
        }
        pause(3)
        mark("end")
    }

    // MARK: D. The toolbox: Gerber viewer, calculator

    func testD_Toolbox() throws {
        app.launchEnvironment = ["CORVCAD_GERBERS": "Gerbers"]
        app.launch()
        XCTAssertTrue(app.descendants(matching: .any)["gerberCanvas"].waitForExistence(timeout: 60))
        pause(4)
        mark("gerbers")
        let canvas = app.descendants(matching: .any)["gerberCanvas"]
        canvas.pinch(withScale: 2.2, velocity: 0.6)
        pause(3)
        canvas.pinch(withScale: 0.5, velocity: -0.6)
        pause(2)
        mark("gerbersZoom")
        if app.buttons["Done"].exists { app.buttons["Done"].tap() } else if app.buttons["Close"].exists { app.buttons["Close"].tap() }
        pause(1)
        readyTap(app.buttons["openCalculator"])
        pause(3)
        mark("calculator")
        let tabs = app.segmentedControls["calculatorTab"]
        if tabs.waitForExistence(timeout: 5) {
            for i in 1..<min(tabs.buttons.count, 5) { tabs.buttons.element(boundBy: i).tap(); pause(1.6) }
        }
        mark("end")
    }

    // MARK: E. FreeCAD edition: generated parts in 3D

    func testE_Parts3D() throws {
        launch(project: Self.project, mode: "enclosure")
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 120))
        waitFor(probe) { $0.contains("\"ready\":true") }
        mark("ready")
        readyTap(app.buttons["Lid"].firstMatch)  // lid off: the board and its parts
        let status = app.descendants(matching: .any)["partsStatus"]
        XCTAssertTrue(status.waitForExistence(timeout: 60))
        mark("generating")
        let done = NSPredicate(format: "label CONTAINS 'generated' AND NOT (label CONTAINS 'generating')")
        expectation(for: done, evaluatedWith: status)
        waitForExpectations(timeout: 900)
        mark("generated")
        let view = app.descendants(matching: .any)["enclosurePreview"]
        pause(3)
        view.pinch(withScale: 1.8, velocity: 0.5)
        pause(2)
        orbit(view, dx: 0.3, dy: 0.0)
        pause(3)
        orbit(view, dx: -0.15, dy: 0.0)
        pause(3)
        view.pinch(withScale: 2.0, velocity: 0.6)
        pause(4)
        mark("closeup")
        orbit(view, dx: -0.3, dy: 0.0)
        pause(4)
        mark("end")
    }

    // MARK: F. FreeCAD edition: the enclosure

    func testF_Enclosure() throws {
        launch(project: Self.project, mode: "enclosure")
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 120))
        waitFor(probe) { $0.contains("\"ready\":true") }
        pause(3)
        mark("ready")
        // The parts first: connector cutouts take their height from the generated bodies.
        let status = app.descendants(matching: .any)["partsStatus"]
        if status.waitForExistence(timeout: 30) {
            expectation(for: NSPredicate(format: "NOT (label CONTAINS 'generating')"), evaluatedWith: status)
            waitForExpectations(timeout: 900)
        }
        mark("partsReady")
        readyTap(app.buttons["Show inspector"])
        pause(3)
        mark("timeline")
        let cutouts = app.switches.matching(NSPredicate(format: "identifier BEGINSWITH 'cutout-'"))
        for i in 0..<min(cutouts.count, 3) {
            let toggle = cutouts.element(boundBy: i)
            reveal(toggle)
            toggle.tap()
            waitFor(probe) { $0.contains("\"busy\":false") || !$0.contains("busy") }
            pause(2.5)
        }
        mark("cutouts")
        let vents = app.buttons["addLidVents"]
        reveal(vents)
        readyTap(vents)
        XCTAssertTrue(app.buttons["feature-Vent1Pattern"].waitForExistence(timeout: 120))
        pause(2.5)
        mark("vents")
        let screws = app.segmentedControls["lidScrews"]
        reveal(screws)
        screws.buttons["M3"].tap()
        XCTAssertTrue(app.buttons["feature-ScrewPosts"].waitForExistence(timeout: 120))
        pause(2.5)
        mark("screws")
        let fix = app.buttons["fix-board_clearance"]
        if fix.waitForExistence(timeout: 30) {
            reveal(fix)
            fix.tap()
            XCTAssertTrue(fix.waitForNonExistence(timeout: 60))
            pause(2.5)
            mark("clearance")
        }
        let sketch = app.buttons["newSketch"]
        reveal(sketch)
        readyTap(sketch)
        let canvas = app.descendants(matching: .any)["sketchCanvas"]
        XCTAssertTrue(canvas.waitForExistence(timeout: 20))
        pause(1.5)
        mark("sketch")
        func p(_ x: Double, _ y: Double) -> XCUICoordinate { canvas.coordinate(withNormalizedOffset: CGVector(dx: x, dy: y)) }
        // Clear of the lid vents in the middle: a rectangle top left, an arc-edged triangle top right.
        p(0.12, 0.24).press(forDuration: 0.1, thenDragTo: p(0.26, 0.40))
        pause(1)
        app.buttons["Lines"].tap()
        for (x, y) in [(0.72, 0.26), (0.86, 0.26), (0.86, 0.42), (0.72, 0.26)] { p(x, y).tap(); pause(0.5) }
        app.buttons["Select"].tap()
        p(0.79, 0.26).tap()
        p(0.79, 0.26).press(forDuration: 0.1, thenDragTo: p(0.79, 0.21))
        pause(2)
        mark("sketchDrawn")
        app.buttons["sketchApply"].tap()
        XCTAssertTrue(app.buttons["feature-UserSketch1Cut"].waitForExistence(timeout: 120))
        pause(3)
        mark("sketchCut")
        let save = app.buttons["saveEnclosure"]
        reveal(save)
        readyTap(save)
        pause(2)
        readyTap(app.buttons["Hide inspector"])
        pause(2)
        orbit(app.descendants(matching: .any)["enclosurePreview"], dx: 0.3, dy: -0.05)
        pause(4)
        mark("end")
    }

    // MARK: G. FreeCAD edition: Fabricate with exact STEP

    func testG_FabricateFreeCAD() throws {
        launch(project: Self.project, mode: "fabricate")
        XCTAssertTrue(app.descendants(matching: .any)["enclosureMeasured"].waitForExistence(timeout: 180))
        pause(3)
        mark("fabricate")
        for id in ["step", "fcstd", "traySTL", "lidSTL"] {
            try export(id, timeout: 300)
            mark("exported-" + id)
            pause(1)
        }
        pause(3)
        mark("end")
    }

    // MARK: H. FreeCAD edition: the lid closes (finale)

    func testH_Finale() throws {
        launch(project: Self.project, mode: "enclosure")
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 120))
        waitFor(probe) { $0.contains("\"ready\":true") }
        let status = app.descendants(matching: .any)["partsStatus"]
        _ = status.waitForExistence(timeout: 60)
        expectation(for: NSPredicate(format: "label CONTAINS 'generated' AND NOT (label CONTAINS 'generating')"), evaluatedWith: status)
        waitForExpectations(timeout: 900)
        // Clearance first: the pin headers would pierce the lid at the default 2 mm.
        readyTap(app.buttons["Show inspector"])
        let fix = app.buttons["fix-board_clearance"]
        if fix.waitForExistence(timeout: 30) { reveal(fix); fix.tap(); _ = fix.waitForNonExistence(timeout: 60) }
        readyTap(app.buttons["Hide inspector"])
        pause(4)
        mark("open")
        readyTap(app.buttons["Exploded"].firstMatch)  // the lid comes down onto the tray
        pause(5)
        mark("closed")
        orbit(app.descendants(matching: .any)["enclosurePreview"], dx: 0.4, dy: 0.0)
        pause(8)
        mark("end")
    }

    // MARK: Helpers

    private var canvas: XCUIElement { app.otherElements["canvas"].firstMatch }

    private func launch(project: String, mode: String) {
        app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": mode]
        app.launch()
    }

    private func mark(_ name: String) {
        print("SHOWCASE-MARK \(name) \(String(format: "%.3f", Date().timeIntervalSince1970))")
        // The canvas's mapping at this moment (window points), for the editor's push-ins.
        let probe = app.descendants(matching: .any)["viewportProbe"].firstMatch
        if probe.exists { print("SHOWCASE-VIEWPORT \(name) \(probe.label)") }
    }

    private func pause(_ seconds: Double) { Thread.sleep(forTimeInterval: seconds) }

    /// The current canvas's millimetres-to-window mapping (viewportProbe: minX minY scale left top).
    private func viewport() -> [Double] {
        let probe = app.descendants(matching: .any)["viewportProbe"].firstMatch
        for _ in 0..<50 {
            if probe.exists {
                let values = probe.label.split(separator: " ").compactMap { Double($0) }
                if values.count == 5 { return values }
            }
            pause(0.2)
        }
        XCTFail("no canvas viewport probe")
        return [0, 0, 1, 0, 0]
    }

    private func point(_ x: Double, _ y: Double) -> XCUICoordinate {
        let v = viewport()
        return app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: v[3] + (x - v[0]) * v[2], dy: v[4] + (y - v[1]) * v[2]))
    }

    private func pcbPoint(_ x: Double, _ y: Double) -> XCUICoordinate { point(x, y) }

    /// Taps a point in millimetres. A refusal (an alert) is logged with the point and dismissed, so
    /// one run shows every step that needs adjusting.
    @discardableResult private func tapMM(_ x: Double, _ y: Double) -> Bool {
        point(x, y).tap()
        pause(0.25)
        let alert = app.alerts.firstMatch
        if alert.exists {
            let message = alert.staticTexts.allElementsBoundByIndex.map(\.label).joined(separator: " | ")
            print("SHOWCASE-ALERT at \(x) \(y): \(message)")
            alert.buttons.firstMatch.tap()
            pause(0.4)
            return true
        }
        return false
    }

    private func footprints() -> [(ref: String, x: Double, y: Double)] {
        let probe = app.descendants(matching: .any)["footprintsProbe"].firstMatch
        XCTAssertTrue(probe.waitForExistence(timeout: 10))
        return probe.label.split(separator: ";").compactMap {
            let f = $0.split(separator: " ")
            guard f.count == 3, let x = Double(f[1]), let y = Double(f[2]) else { return nil }
            return (String(f[0]), x, y)
        }
    }

    private func unrouted() -> [(Double, Double, Double, Double)] {
        let probe = app.descendants(matching: .any)["unroutedProbe"].firstMatch
        guard probe.waitForExistence(timeout: 10) else { return [] }
        return probe.label.split(separator: ";").compactMap {
            let v = $0.split(separator: " ").compactMap { Double($0) }
            return v.count == 4 ? (v[0], v[1], v[2], v[3]) : nil
        }
    }

    /// Footprints in a readable order: the op-amp, the resistors, then the capacitor.
    private func order(_ ref: String) -> String {
        (ref.hasPrefix("U") ? "0" : ref.hasPrefix("R") ? "1" : "2") + ref
    }

    private func place(_ name: String, library: String, at spot: CGPoint, footprint: String?, value: String?) throws {
        readyTap(app.buttons["Part"].firstMatch)
        readyTap(app.buttons["choosePart"])
        let search = app.textFields["partSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 10))
        search.tap()
        search.typeText(name + " " + library)
        let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@ AND label CONTAINS[c] %@", name, library)).firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 15))
        pause(0.6)
        row.tap()
        point(spot.x, spot.y).tap()
        if let footprint {
            let fsearch = app.textFields["footprintSearch"]
            XCTAssertTrue(fsearch.waitForExistence(timeout: 15))
            fsearch.tap()
            fsearch.typeText(footprint)
            let frow = app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", footprint)).firstMatch
            if !frow.waitForExistence(timeout: 3) { app.switches.firstMatch.tap() }
            XCTAssertTrue(frow.waitForExistence(timeout: 10))
            pause(0.6)
            frow.tap()
        }
        pause(1)
        if let value {
            readyTap(app.buttons["Select"].firstMatch)
            point(spot.x, spot.y).press(forDuration: 0.7)
            readyTap(app.buttons["Edit"].firstMatch)
            replace(app.textFields["10k"].firstMatch, with: value + "\n")
            readyTap(app.buttons["saveFields"])
            pause(0.8)
        }
        XCTAssertFalse(app.alerts.firstMatch.exists, app.alerts.firstMatch.debugDescription)
    }

    private func power(_ name: String, at spots: [CGPoint]) throws {
        readyTap(app.buttons["Part"].firstMatch)
        readyTap(app.buttons["choosePower"])
        readyTap(app.buttons[name].firstMatch)
        for spot in spots {
            point(spot.x, spot.y).tap()
            pause(0.8)
        }
    }

    private func replace(_ field: XCUIElement, with text: String) {
        XCTAssertTrue(field.waitForExistence(timeout: 10))
        field.tap()
        let old = field.value as? String ?? ""
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: old.count) + text)
        pause(0.4)
    }

    private func readyTap(_ element: XCUIElement) {
        for _ in 0..<200 {
            if element.exists && element.isHittable && element.isEnabled { element.tap(); return }
            pause(0.15)
        }
        XCTFail("Required control is not hittable: \(element)")
    }

    /// Scrolls the inspector until the element can be tapped.
    private func reveal(_ element: XCUIElement, upward: Bool = true) {
        for _ in 0..<12 {
            if element.exists && element.isHittable { return }
            let scroll = app.scrollViews.firstMatch
            upward ? scroll.swipeUp(velocity: .slow) : scroll.swipeDown(velocity: .slow)
        }
    }

    private func orbit(_ view: XCUIElement, dx: Double, dy: Double) {
        let start = view.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
        start.press(forDuration: 0.05, thenDragTo: view.coordinate(withNormalizedOffset: CGVector(dx: 0.5 + dx, dy: 0.5 + dy)),
                    withVelocity: .slow, thenHoldForDuration: 0.1)
    }

    private func waitFor(_ probe: XCUIElement, timeout: TimeInterval = 300, _ condition: @escaping (String) -> Bool) {
        let predicate = NSPredicate { _, _ in condition(probe.label) }
        XCTAssertEqual(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: predicate, object: nil)], timeout: timeout), .completed,
                       probe.label)
    }

    private func export(_ id: String, timeout: TimeInterval) throws {
        let button = app.buttons["export-\(id)"]
        for _ in 0..<6 where !button.isHittable { app.swipeUp(velocity: .slow) }
        guard button.waitForExistence(timeout: 5) else { return }
        button.tap()
        XCTAssertTrue(app.buttons["share-\(id)"].waitForExistence(timeout: timeout), "\(id) exported")
    }
}

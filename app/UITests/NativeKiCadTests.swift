import XCTest

/// The NATIVE_KICAD edition's KiCad features by touch, in a simulator (CircleCI native-sim).
/// Needs three projects in Documents/Projects, copied in by CI: ecc83-pp (KiCad's demo),
/// ecc83-unrouted (the same with its tracks and vias removed) and erc-roundtrip (the engine's
/// RoundTrip fixture, files named after the folder). Skipped in the compatible
/// edition. Pass SNAPSHOT_DIR to write the screenshots as PNGs.
final class NativeKiCadTests: XCTestCase {
    /// Track tool with KiCad's router: tap a pad, tap the pad at the other end of its ratsnest
    /// line; then the next connection on B.Cu through a via. The board must gain tracks and a
    /// via and lose one unrouted connection per route.
    func testRouteWithKiCadRouter() throws {
        let app = launch(project: "ecc83-unrouted", mode: "pcb")
        let probe = app.descendants(matching: .any)["ratsnestProbe"]
        XCTAssertTrue(probe.waitForExistence(timeout: 15), "debug probe on the PCB canvas")
        app.buttons["Track"].firstMatch.tap()
        guard app.segmentedControls["routeMode"].waitForExistence(timeout: 3) else {
            throw XCTSkip("Compatible edition: no KiCad router")
        }
        try capture("route-0-ratsnest")

        let first = numbers(probe.label)
        XCTAssertGreaterThan(first[6], 1, "ecc83 without tracks has unrouted connections")
        tap(app, first[0], first[1])
        sleep(1)
        tap(app, first[2], first[3])
        sleep(3)
        let routed = numbers(probe.label)
        XCTAssertGreaterThan(routed[4], first[4], "the route was saved as tracks")
        XCTAssertEqual(routed[6], first[6] - 1, "one connection fewer unrouted")
        try capture("route-1-walkaround")

        app.buttons["B.Cu"].firstMatch.tap()
        tap(app, routed[0], routed[1])
        sleep(1)
        app.buttons["routeVia"].tap()
        tap(app, (routed[0] + routed[2]) / 2, (routed[1] + routed[3]) / 2)
        sleep(1)
        try capture("route-2-via-placed")
        tap(app, routed[2], routed[3])
        sleep(3)
        let viaRouted = numbers(probe.label)
        XCTAssertGreaterThan(viaRouted[5], routed[5], "the via was saved")
        XCTAssertEqual(viaRouted[6], routed[6] - 1, "one more connection routed")
        try capture("route-3-via")
    }

    /// KiCad's simulator from the schematic inspector, then the result on the 3D board. The
    /// sim-rectifier project (KiCad's rectifier demo with ngspice/fixture_board.py's board) is
    /// copied in by the run scripts.
    func testSimulationOnBoard() throws {
        let app = launch(project: "sim-rectifier", mode: "schematic")
        guard showInspector(app, until: app.buttons["runSimulation"]) else {
            throw XCTSkip("Compatible edition, or a build without ngspice: no simulator")
        }
        app.buttons["runSimulation"].tap()
        let onBoard = app.buttons["showOnBoard"]
        XCTAssertTrue(onBoard.waitForExistence(timeout: 90), "the run ended with a plot")
        XCTAssertFalse(app.alerts.firstMatch.exists, "no simulator error")
        sleep(1)
        try capture("simulation-plot")
        app.buttons["Hide inspector"].firstMatch.tap()
        sleep(1)
        try capture("simulation-plot-full")
        onBoard.tap()
        let summary = app.staticTexts["electrifySummary"]
        XCTAssertTrue(summary.waitForExistence(timeout: 20), "the board is electrified")
        XCTAssertTrue(summary.label.lowercased().contains("nets simulated"), summary.label)
        XCTAssertFalse(summary.label.lowercased().hasSuffix("current on 0"), "current on some net: \(summary.label)")
        sleep(3)
        try capture("simulation-electrified")
    }

    /// KiCad's demo schematic draws its library symbols; placing LM358 from the part picker
    /// opens the footprint chooser, and the chosen footprint is saved in the symbol's fields.
    func testLibrarySymbolsAndFootprintChooser() throws {
        let app = launch(project: "ecc83-pp", mode: "schematic")
        sleep(3)
        try capture("schematic-library-symbols")
        app.buttons["Part"].firstMatch.tap()
        guard app.buttons["choosePart"].waitForExistence(timeout: 3) else {
            throw XCTSkip("Compatible edition: no KiCad libraries")
        }
        app.buttons["choosePart"].tap()
        let search = app.textFields["partSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 5))
        search.tap()
        search.typeText("LM358")
        let part = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'LM358'")).firstMatch
        XCTAssertTrue(part.waitForExistence(timeout: 5))
        part.tap()

        let spot = app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.85))
        spot.tap()
        let footprints = app.textFields["footprintSearch"]
        XCTAssertTrue(footprints.waitForExistence(timeout: 10), "the chooser opens for a part without a footprint")
        try capture("footprint-chooser")
        footprints.tap()
        footprints.typeText("SOIC-8_3.9x4.9mm_P1.27mm")
        let soic = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'SOIC-8_3.9x4.9mm_P1.27mm'")).firstMatch
        XCTAssertTrue(soic.waitForExistence(timeout: 5))
        soic.tap()
        sleep(2)

        app.buttons["Select"].firstMatch.tap()
        spot.press(forDuration: 0.8)
        app.buttons["Edit"].firstMatch.tap()
        let field = app.textFields["Library:Footprint"].firstMatch
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        XCTAssertEqual(field.value as? String, "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
        try capture("lm358-fields")
        app.buttons["Cancel"].firstMatch.tap()
        sleep(1)
        XCTAssertEqual(app.descendants(matching: .any)["placeholderProbe"].label, "0",
                       "every symbol, the placed LM358 included, is drawn from its library definition")
        try capture("lm358-drawn")
    }

    /// KiCad's DRC from the board inspector, run by KiCad's code in the simulator.
    func testKiCadDRC() throws {
        let app = launch(project: "ecc83-pp", mode: "pcb")
        guard showInspector(app, until: app.buttons["runDRC"]) else {
            throw XCTSkip("Compatible edition: no KiCad DRC")
        }
        app.buttons["runDRC"].tap()
        let finding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
        XCTAssertTrue(finding.firstMatch.waitForExistence(timeout: 60), "KiCad's DRC listed ecc83's findings")
        try capture("drc-native")
    }

    /// KiCad's ERC from the schematic inspector: findings listed and marked on the sheet, the
    /// netlist exported and the schematic plotted to PDF and SVG for sharing. ecc83 is ERC-clean in kicad-cli 10.0, so this uses CI's copy
    /// of engine/tests/fixtures/roundtrip (erc-roundtrip), where kicad-cli reports six warnings.
    func testKiCadERC() throws {
        let app = launch(project: "erc-roundtrip", mode: "schematic")
        guard showInspector(app, until: app.buttons["runERC"]) else {
            XCTAssertFalse(app.buttons["plotPDF"].exists || app.buttons["plotSVG"].exists, "no KiCad plot without CC_SCH_PLOT")
            throw XCTSkip("Compatible edition: no KiCad ERC")
        }
        app.buttons["runERC"].tap()
        let finding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
        XCTAssertTrue(finding.firstMatch.waitForExistence(timeout: 60), "KiCad's ERC listed the project's findings")
        finding.firstMatch.tap()
        sleep(1)
        try capture("erc-native")
        app.buttons["exportNetlist"].tap()
        XCTAssertTrue(app.buttons["Share erc-roundtrip.net"].waitForExistence(timeout: 10), "netlist ready to share")
        app.buttons["plotPDF"].tap()
        XCTAssertTrue(app.buttons["Share erc-roundtrip.pdf"].waitForExistence(timeout: 30), "KiCad's PDF plot ready to share")
        app.buttons["plotSVG"].tap()
        let svg = app.buttons["sharePlot"]
        XCTAssertTrue(svg.waitForExistence(timeout: 30) && svg.label.hasSuffix(".svg"), "one SVG for the single sheet: \(svg.label)")
    }

    /// Session API 8's tools on erc-roundtrip: a text note placed with the Text tool, a note line
    /// with the Line tool, then the note held for its menu. The no-connect flag and the mirror
    /// are checked against KiCad by the probe (board_check.py schematic and notes-parity).
    func testNotesTools() throws {
        let app = launch(project: "erc-roundtrip", mode: "schematic")
        sleep(3)
        guard app.buttons["NC"].waitForExistence(timeout: 5) else {
            XCTAssertFalse(app.buttons["Text"].exists, "no note tools without KiCad's")
            throw XCTSkip("Compatible edition: no KiCad tools")
        }
        app.buttons["Text"].firstMatch.tap()
        let field = app.textFields["noteText"]
        XCTAssertTrue(field.waitForExistence(timeout: 3))
        field.tap()
        field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 8) + "Check R1\n")
        let spot = app.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.3))
        spot.tap()
        sleep(2)
        app.buttons["Line"].firstMatch.tap()
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.25, dy: 0.36)).tap()
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.45, dy: 0.36)).tap()
        sleep(2)
        try capture("schematic-notes")
        // API 9's helpers: the Entry tool's bus-to-bus kind and the Part tool's power rails.
        app.buttons["Entry"].firstMatch.tap()
        XCTAssertTrue(app.segmentedControls["entryKind"].waitForExistence(timeout: 3))
        app.buttons["Part"].firstMatch.tap()
        let power = app.buttons["choosePower"]
        XCTAssertTrue(power.waitForExistence(timeout: 3))
        power.tap()
        app.buttons["GND"].firstMatch.tap()
        XCTAssertTrue(app.staticTexts["power:GND"].waitForExistence(timeout: 3), "GND becomes the part to place")
        app.buttons["Select"].firstMatch.tap()
        spot.press(forDuration: 0.8)
        XCTAssertTrue(app.staticTexts["Check R1"].waitForExistence(timeout: 5), "the held text note opens its menu")
        XCTAssertTrue(app.buttons["Delete"].exists)
    }

    /// KiCad's board edits: the Draw tool's dimension (three taps, as in KiCad) exists only with
    /// KiCad's board model, and placing one is an undoable edit. Flip and track edits are checked
    /// against kicad-cli by board_check.py edit and edit-parity.
    func testBoardDimension() throws {
        let app = launch(project: "ecc83-pp", mode: "pcb")
        sleep(3)
        app.buttons["Draw"].firstMatch.tap()
        let kinds = app.segmentedControls["drawKind"]
        XCTAssertTrue(kinds.waitForExistence(timeout: 5))
        guard kinds.buttons["Dim"].exists else { throw XCTSkip("Compatible edition: no KiCad board edits") }
        kinds.buttons["Dim"].tap()
        XCTAssertFalse(app.buttons["Undo"].isEnabled)
        for (x, y) in [(0.3, 0.3), (0.55, 0.3), (0.55, 0.25)] {
            app.coordinate(withNormalizedOffset: CGVector(dx: x, dy: y)).tap()
        }
        let undo = app.buttons["Undo"]
        XCTAssertTrue(undo.waitForExistence(timeout: 5))
        XCTAssertTrue(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "isEnabled == true"), object: undo)],
                                     timeout: 10) == .completed, "the dimension is an undoable edit")
        try capture("board-dimension")
    }

    /// Board Setup lists the stack-up's dielectric layers with thickness, material and Er.
    /// Applying them and zone properties are checked against kicad-cli by board_check.py setup,
    /// edit and edit-parity.
    func testBoardSetupStackup() throws {
        let app = launch(project: "ecc83-pp", mode: "pcb")
        guard showInspector(app, until: app.buttons["editBoardSetup"]) else {
            throw XCTSkip("Compatible edition: no KiCad Board Setup")
        }
        let edit = app.buttons["editBoardSetup"]
        for _ in 0..<6 where !edit.isHittable { app.scrollViews.firstMatch.swipeUp() }
        edit.tap()
        let dielectric = app.descendants(matching: .any)["dielectric-1"]
        for _ in 0..<8 where !dielectric.exists || !dielectric.isHittable { app.scrollViews.firstMatch.swipeUp() }
        XCTAssertTrue(dielectric.waitForExistence(timeout: 5), "the stack-up's first dielectric layer is listed")
        try capture("board-setup-stackup")
    }

    /// A library footprint placed with the Part tool (KiCad's libraries, NATIVE_KICAD): chosen in
    /// the footprint picker, placed with a tap as one undoable edit. Swapping is checked against
    /// kicad-cli by board_check.py edit and edit-parity.
    func testPlaceLibraryFootprint() throws {
        let app = launch(project: "ecc83-pp", mode: "pcb")
        sleep(3)
        app.buttons["Part"].firstMatch.tap()
        let choose = app.buttons["chooseBoardFootprint"]
        guard choose.waitForExistence(timeout: 5) else { throw XCTSkip("Compatible edition: no KiCad footprint libraries") }
        choose.tap()
        let search = app.textFields["footprintSearch"]
        XCTAssertTrue(search.waitForExistence(timeout: 10))
        search.tap()
        search.typeText("R_0805_2012Metric")
        let item = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'R_0805_2012Metric'")).firstMatch
        XCTAssertTrue(item.waitForExistence(timeout: 10))
        item.tap()
        XCTAssertTrue(app.staticTexts["Resistor_SMD:R_0805_2012Metric"].waitForExistence(timeout: 5), "the Part tool places it")
        app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.62)).tap()
        let undo = app.buttons["Undo"]
        XCTAssertTrue(XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "isEnabled == true"), object: undo)],
                                     timeout: 15) == .completed, "placing it is an undoable edit")
        try capture("board-library-footprint")
    }

    /// KiCad's importers in the app: the Eagle AHT20 schematic and board (KiCad's QA data, copied to
    /// Documents/Import by the run scripts) become a new project through the same path as the
    /// Home screen's "Altium, Eagle…" picker (CORVCAD_IMPORT in debug builds).
    func testImportEagleProject() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_IMPORT": "Import/Adafruit AHT20 Temperature & Humidity.sch|Import/Adafruit AHT20 Temperature & Humidity.brd"]
        app.launch()
        let alert = app.alerts["Imported with KiCad"]
        guard alert.waitForExistence(timeout: 90) else {
            if app.buttons["importOtherCAD"].exists || app.alerts.firstMatch.exists {
                try capture("import-eagle-failed")
                XCTFail("no import report; on screen: \(app.alerts.firstMatch.exists ? app.alerts.firstMatch.debugDescription : "no alert")")
                return
            }
            throw XCTSkip("Compatible edition, or the Eagle files are not seeded")
        }
        let message = alert.staticTexts.matching(NSPredicate(format: "label CONTAINS 'footprints'")).firstMatch
        XCTAssertTrue(message.exists, "the board was imported")
        XCTAssertTrue(alert.staticTexts.matching(NSPredicate(format: "label CONTAINS 'symbols'")).firstMatch.exists,
                      "the schematic was imported")
        try capture("import-eagle")
        alert.buttons["OK"].tap()
        sleep(2)
        try capture("import-eagle-open")
    }

    private func launch(project: String, mode: String) -> XCUIApplication {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": mode]
        app.launch()
        return app
    }

    /// Opens the inspector and waits for `element`. On a cold simulator the first tap can land
    /// before the workspace takes touches, so it taps once more.
    private func showInspector(_ app: XCUIApplication, until element: XCUIElement) -> Bool {
        let show = app.buttons["Show inspector"]
        for _ in 0..<2 {
            if show.waitForExistence(timeout: 10) { show.tap() }
            if element.waitForExistence(timeout: 5) { return true }
        }
        return false
    }

    /// The probe's label: x0 y0 x1 y1 tracks vias unrouted.
    private func numbers(_ label: String) -> [Double] {
        label.split(separator: " ").compactMap { Double($0) }
    }

    private func tap(_ app: XCUIApplication, _ x: Double, _ y: Double) {
        app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: x, dy: y)).tap()
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

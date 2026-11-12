import XCTest

/// Native F4 acceptance by touch. CI seeds Documents/Projects/FreeCAD-F4 with a rectangular
/// board, measured thickness and 3.2 mm mounting holes. Its conflicting legacy sidecar
/// must be ignored. These tests do not enable STEP or claim physical-device validation.
final class NativeFreeCADTests: XCTestCase {
    func testPortraitExactPreviewTransactionsSaveReopenAndSTL() throws {
        try exercise(project: "FreeCAD-F4", orientation: .portrait)
    }

    func testLandscapeExactPreviewTransactionsSaveReopenAndSTL() throws {
        try exercise(project: "FreeCAD-F4-Landscape", orientation: .landscapeLeft)
    }

    private func exercise(project: String, orientation: UIDeviceOrientation) throws {
        XCUIDevice.shared.orientation = orientation
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else {
            #if NATIVE_FREECAD
            XCTFail("Native enclosure edition did not expose its acceptance state")
            return
            #else
            throw XCTSkip("Native FreeCAD edition is not enabled")
            #endif
        }
        var state = try read(probe)
        XCTAssertEqual(state["ready"] as? Bool, true, state["error"] as? String ?? "")
        XCTAssertGreaterThan(number(state, "trayTriangles"), 0)
        XCTAssertGreaterThan(number(state, "lidTriangles"), 0)
        XCTAssertGreaterThan(number(state, "boardThickness"), 0)
        XCTAssertEqual(number(state, "wall"), 2, "a fresh native document ignores the conflicting legacy sidecar")
        XCTAssertFalse(app.buttons["STEP"].exists)
        XCTAssertTrue(app.descendants(matching: .any)["enclosurePreview"].exists)

        app.buttons["Show inspector"].tap()
        let wall = app.sliders["Wall"]
        XCTAssertTrue(wall.waitForExistence(timeout: 5))
        // The timeline lists FreeCAD's own features; a row folds and unfolds its parameters.
        for name in ["OuterPad", "Cavity", "BossPad", "MountingBore1", "LidPad"] {
            XCTAssertTrue(app.buttons["feature-\(name)"].exists, "timeline row \(name)")
        }
        let body = app.buttons["feature-OuterPad"]
        body.tap()
        XCTAssertTrue(wall.waitForNonExistence(timeout: 3), "a folded row hides its sliders")
        body.tap()
        XCTAssertTrue(wall.waitForExistence(timeout: 3))
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = "timeline-\(XCUIDevice.shared.orientation.isPortrait ? "portrait" : "landscape")"
        shot.lifetime = .keepAlways
        add(shot)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("\(shot.name ?? "timeline").png"))
        }
        let initialWall = number(state, "wall")
        wall.adjust(toNormalizedSliderPosition: initialWall < 4 ? 0.9 : 0.2)
        waitForState(probe) { number($0, "wall") != initialWall && $0["canUndo"] as? Bool == true }
        let editedWall = number(try read(probe), "wall")
        app.buttons["Undo"].tap()
        waitForState(probe) { number($0, "wall") == initialWall && $0["canRedo"] as? Bool == true }
        app.buttons["Redo"].tap()
        waitForState(probe) { number($0, "wall") == editedWall }

        reveal(app.buttons["saveEnclosure"], in: app, upward: true)
        app.buttons["saveEnclosure"].tap()
        waitForState(probe) { number($0, "documentBytes") > 0 && $0["dirty"] as? Bool == false }
        state = try read(probe)
        let savedHash = state["documentSHA256"] as? String
        XCTAssertEqual(savedHash?.count, 64)

        // Invalid boss diameter must leave parameters, disk bytes and undo/redo unchanged.
        reveal(app.sliders["Diameter"], in: app, upward: false)
        let diameter = number(state, "standoff_diameter")
        let diameterSlider = app.sliders["Diameter"]
        let diameterPosition = diameterSlider.value as? String
        // Drag the actual thumb: accessibility exposes a localized millimetre
        // value rather than a normalized percentage on this simulator.
        let thumb = diameterSlider.coordinate(withNormalizedOffset: CGVector(dx: (diameter - 3) / 9, dy: 0.5))
        thumb.press(forDuration: 0.1, thenDragTo: diameterSlider.coordinate(withNormalizedOffset: CGVector(dx: 0, dy: 0.5)))
        XCTAssertTrue(app.alerts.firstMatch.waitForExistence(timeout: 15))
        app.alerts.buttons["OK"].tap()
        state = try read(probe)
        XCTAssertEqual(number(state, "standoff_diameter"), diameter)
        XCTAssertEqual(diameterSlider.value as? String, diameterPosition, "rejected edits restore the slider too")
        XCTAssertEqual(state["documentSHA256"] as? String, savedHash)
        XCTAssertEqual(state["dirty"] as? Bool, false)
        XCTAssertEqual(state["canUndo"] as? Bool, true)
        XCTAssertEqual(state["canRedo"] as? Bool, false)

        reveal(app.buttons["exportTraySTL"], in: app, upward: true)
        app.buttons["exportTraySTL"].tap()
        waitForState(probe) { number($0, "traySTLBytes") > 84 }
        app.buttons["exportLidSTL"].tap()
        waitForState(probe) { number($0, "lidSTLBytes") > 84 }
        XCTAssertTrue(app.buttons["Share tray STL"].exists)
        XCTAssertTrue(app.buttons["Share lid STL"].exists)

        app.terminate()
        app.launch()
        XCTAssertTrue(probe.waitForExistence(timeout: 45))
        state = try read(probe)
        XCTAssertEqual(state["ready"] as? Bool, true, state["error"] as? String ?? "")
        XCTAssertEqual(number(state, "wall"), editedWall, "FCStd parameters override the legacy sidecar")
        XCTAssertEqual(state["documentSHA256"] as? String, savedHash, "opening must not rewrite FCStd")
        XCTAssertEqual(state["dirty"] as? Bool, false)
        XCTAssertFalse(state["canUndo"] as? Bool ?? true, "reopened document begins a fresh session history")
        XCTAssertGreaterThan(number(state, "trayTriangles"), 0)
        XCTAssertGreaterThan(number(state, "lidTriangles"), 0)
        app.terminate()
    }

    /// The combined edition on one project ("Both", CI's copy of the RoundTrip fixture): a board
    /// edit, a saved enclosure change and KiCad's ERC in one process, then each engine's saved
    /// state must survive the other's work, live and after a relaunch.
    func testBothEnginesOnOneProject() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Both", "CORVCAD_MODE": "pcb"]
        app.launch()
        let window = app.windows.firstMatch
        let spot = window.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.86))
        let draw = app.buttons["Draw"].firstMatch
        XCTAssertTrue(draw.waitForExistence(timeout: 10))
        draw.tap()
        if !app.segmentedControls["drawKind"].waitForExistence(timeout: 3) { draw.tap() }
        app.segmentedControls["drawKind"].buttons["Text"].tap()
        spot.tap()
        sleep(2)
        app.buttons["Select"].firstMatch.tap()

        switchMode("Enclosure", in: app)
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else {
            #if NATIVE_FREECAD
            XCTFail("Native enclosure edition did not expose its acceptance state"); return
            #else
            throw XCTSkip("Native FreeCAD edition is not enabled")
            #endif
        }
        waitForState(probe, timeout: 60) { $0["ready"] as? Bool == true }
        if app.buttons["Show inspector"].exists { app.buttons["Show inspector"].tap() }
        let wall = app.sliders["Wall"]
        XCTAssertTrue(wall.waitForExistence(timeout: 5))
        let initialWall = number(try read(probe), "wall")
        wall.adjust(toNormalizedSliderPosition: initialWall < 4 ? 0.9 : 0.2)
        waitForState(probe) { number($0, "wall") != initialWall && $0["canUndo"] as? Bool == true }
        reveal(app.buttons["saveEnclosure"], in: app, upward: true)
        app.buttons["saveEnclosure"].tap()
        waitForState(probe) { number($0, "documentBytes") > 0 && $0["dirty"] as? Bool == false }
        let saved = try read(probe)
        let savedHash = saved["documentSHA256"] as? String
        let savedWall = number(saved, "wall")

        switchMode("Schematic", in: app)
        if app.buttons["Show inspector"].exists { app.buttons["Show inspector"].tap() }
        guard app.buttons["runERC"].waitForExistence(timeout: 10) else {
            throw XCTSkip("Not the combined edition: no KiCad ERC")
        }
        app.buttons["runERC"].tap()
        let finding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
        XCTAssertTrue(finding.firstMatch.waitForExistence(timeout: 60), "KiCad's ERC runs after FreeCAD in the same process")

        switchMode("PCB", in: app)
        if app.buttons["Hide inspector"].exists { app.buttons["Hide inspector"].tap() }
        app.buttons["Select"].firstMatch.tap()
        spot.press(forDuration: 0.8)
        XCTAssertTrue(app.buttons["Edit"].firstMatch.waitForExistence(timeout: 3), "the board edit survived the enclosure work")
        window.coordinate(withNormalizedOffset: CGVector(dx: 0.9, dy: 0.95)).tap()

        switchMode("Enclosure", in: app)
        XCTAssertTrue(probe.waitForExistence(timeout: 20))
        var state = try read(probe)
        XCTAssertEqual(state["documentSHA256"] as? String, savedHash, "KiCad's work left FCStd untouched")
        XCTAssertEqual(state["dirty"] as? Bool, false)

        app.terminate()
        app.launchEnvironment = ["CORVCAD_OPEN": "Both", "CORVCAD_MODE": "enclosure"]
        app.launch()
        XCTAssertTrue(probe.waitForExistence(timeout: 45))
        waitForState(probe, timeout: 60) { $0["ready"] as? Bool == true }
        state = try read(probe)
        XCTAssertEqual(number(state, "wall"), savedWall, "the saved enclosure reopens with the board")
        XCTAssertEqual(state["documentSHA256"] as? String, savedHash)
        switchMode("PCB", in: app)
        if app.buttons["Hide inspector"].exists { app.buttons["Hide inspector"].tap() }
        app.buttons["Select"].firstMatch.tap()
        spot.press(forDuration: 0.8)
        XCTAssertTrue(app.buttons["Edit"].firstMatch.waitForExistence(timeout: 3), "the board edit reopens with the enclosure")
        app.terminate()
    }

    /// Taps a workspace mode until it reports selected: a tap can land while the canvas still
    /// holds the previous tool's touch.
    private func switchMode(_ name: String, in app: XCUIApplication) {
        let button = app.buttons[name].firstMatch
        let selected = NSPredicate(format: "isSelected == true")
        for _ in 0..<3 {
            button.tap()
            if XCTWaiter.wait(for: [XCTNSPredicateExpectation(predicate: selected, object: button)], timeout: 3) == .completed { return }
        }
        XCTFail("\(name) mode did not open")
    }

    /// Connector cutouts on Documents/Projects/Connector: RoundTrip with R1 renamed J1 and moved to
    /// (3, 30), 1.8 mm from the left edge (seeded by the run scripts). Its suggestion adds a wall
    /// cutout feature, undo and redo take it out and back, and it survives save and relaunch.
    func testConnectorCutout() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        var app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Connector", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
        waitForState(probe) { $0["ready"] as? Bool == true }
        app.buttons["Show inspector"].tap()
        let toggle = app.switches["cutout-J1"]
        guard toggle.waitForExistence(timeout: 10) else { throw XCTSkip("The Connector project is not seeded") }
        let before = number(try read(probe), "trayTriangles")
        reveal(toggle, in: app, upward: true)
        toggle.tap()
        let feature = app.buttons["feature-Cutout1"]
        XCTAssertTrue(feature.waitForExistence(timeout: 20), "the cutout joins the timeline")
        waitForState(probe) { number($0, "trayTriangles") != before && $0["canUndo"] as? Bool == true }
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = "connector-cutout"
        shot.lifetime = .keepAlways
        add(shot)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("connector-cutout.png"))
        }
        app.buttons["Undo"].tap()
        XCTAssertTrue(feature.waitForNonExistence(timeout: 20), "undo removes the cutout")
        app.buttons["Redo"].tap()
        XCTAssertTrue(feature.waitForExistence(timeout: 20), "redo restores it")
        reveal(app.buttons["saveEnclosure"], in: app, upward: true)
        app.buttons["saveEnclosure"].tap()
        waitForState(probe) { $0["dirty"] as? Bool == false }

        app.terminate()
        app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Connector", "CORVCAD_MODE": "enclosure"]
        app.launch()
        XCTAssertTrue(app.descendants(matching: .any)["mechanicalProbe"].waitForExistence(timeout: 45))
        app.buttons["Show inspector"].tap()
        XCTAssertTrue(app.buttons["feature-Cutout1"].waitForExistence(timeout: 20), "the saved cutout reopens")
        XCTAssertEqual(app.switches["cutout-J1"].value as? String, "1")
    }

    /// Generated parts (gate 3 of docs/3D_MODELS.md) on Documents/Projects/Parts: RoundTrip with
    /// R1, R2 and R3 given KiCad's R_0603, SOIC-8 and PinHeader_1x04 models (seeded by the run
    /// scripts). They generate in the background and replace their placeholders, in both
    /// orientations; the Parts pill hides them.
    func testBoardParts() throws {
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": "Parts", "CORVCAD_MODE": "enclosure"]
            app.launch()
            let probe = app.descendants(matching: .any)["mechanicalProbe"]
            guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
            waitForState(probe) { $0["ready"] as? Bool == true }
            let status = app.descendants(matching: .any)["partsStatus"]
            XCTAssertTrue(status.waitForExistence(timeout: 20))
            let generated = NSPredicate(format: "label CONTAINS '3 generated' AND NOT (label CONTAINS 'placeholder')")
            expectation(for: generated, evaluatedWith: status)
            waitForExpectations(timeout: 60)
            let name = "board-parts-\(orientation.isPortrait ? "portrait" : "landscape")"
            let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
            shot.name = name
            shot.lifetime = .keepAlways
            add(shot)
            if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
                try XCUIScreen.main.screenshot().pngRepresentation
                    .write(to: URL(fileURLWithPath: directory).appendingPathComponent("\(name).png"))
            }
            if orientation.isPortrait {
                // The cache menu: clearing it regenerates the board's parts.
                status.tap()
                let clear = app.buttons["clearGeneratedParts"]
                XCTAssertTrue(clear.waitForExistence(timeout: 5))
                XCTAssertFalse(app.staticTexts["Cache Zero KB"].exists, "the generated parts are cached")
                clear.tap()
                expectation(for: generated, evaluatedWith: status)
                waitForExpectations(timeout: 60)
            }
            app.buttons["Parts"].tap()
            XCTAssertTrue(status.waitForNonExistence(timeout: 5), "the Parts pill hides the parts")
            if orientation.isPortrait {
                // Component clearance: the 8.54 mm pin header doesn't fit under the 2 mm top
                // clearance; the fix raises it in the FreeCAD document to 9.1 mm (reach + 0.5).
                app.buttons["Show inspector"].tap()
                let fix = app.buttons["fix-board_clearance"]
                XCTAssertTrue(fix.waitForExistence(timeout: 10))
                reveal(fix, in: app, upward: true)
                fix.tap()
                waitForState(probe) { abs(number($0, "board_clearance") - 9.1) < 0.01 }
                XCTAssertTrue(fix.waitForNonExistence(timeout: 10), "the part fits after the fix")
                let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
                shot.name = "component-clearance"
                shot.lifetime = .keepAlways
                add(shot)
                if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
                    try XCUIScreen.main.screenshot().pngRepresentation
                        .write(to: URL(fileURLWithPath: directory).appendingPathComponent("component-clearance.png"))
                }
            }
            app.terminate()
        }
    }

    /// Lid vents on the Connector project: a slot and its FreeCAD linear pattern join the
    /// timeline, the stepper adds a slot, removal and its undo work.
    func testVents() throws {
        XCUIDevice.shared.orientation = .portrait
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Connector", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
        waitForState(probe) { $0["ready"] as? Bool == true }
        app.buttons["Show inspector"].tap()
        let addVents = app.buttons["addLidVents"]
        guard addVents.waitForExistence(timeout: 10) else { throw XCTSkip("The Connector project is not seeded") }
        let before = number(try read(probe), "lidTriangles")
        reveal(addVents, in: app, upward: true)
        addVents.tap()
        XCTAssertTrue(app.buttons["feature-Vent1Pattern"].waitForExistence(timeout: 20), "slot and pattern join the timeline")
        waitForState(probe) { number($0, "lidTriangles") != before }
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = "lid-vents"
        shot.lifetime = .keepAlways
        add(shot)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("lid-vents.png"))
        }
        let slots = number(try read(probe), "lidTriangles")
        let increment = app.steppers["ventCount-1"].buttons.element(boundBy: 1)
        reveal(increment, in: app, upward: true)
        increment.tap()
        waitForState(probe) { number($0, "lidTriangles") != slots }
        let remove = app.buttons["removeVents-1"]
        reveal(remove, in: app, upward: true)
        remove.tap()
        XCTAssertTrue(app.buttons["feature-Vent1"].waitForNonExistence(timeout: 20), "removed")
        app.buttons["Undo"].tap()
        XCTAssertTrue(app.buttons["feature-Vent1"].waitForExistence(timeout: 20), "undo restores the row")
    }

    /// Lid screws on the Connector project: M3 adds the corner posts and holes and raises the
    /// board margin in the same step; inserts enlarge the posts; undo takes both back.
    func testLidScrews() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Connector", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
        waitForState(probe) { $0["ready"] as? Bool == true }
        app.buttons["Show inspector"].tap()
        let picker = app.segmentedControls["lidScrews"]
        guard picker.waitForExistence(timeout: 10) else { throw XCTSkip("The Connector project is not seeded") }
        let margin = number(try read(probe), "margin")
        reveal(picker, in: app, upward: true)
        picker.buttons["M3"].tap()
        XCTAssertTrue(app.buttons["feature-ScrewPosts"].waitForExistence(timeout: 20), "posts join the timeline")
        XCTAssertTrue(app.buttons["feature-LidScrewHoles"].exists, "lid holes join the timeline")
        waitForState(probe) { number($0, "margin") >= 6 }
        let inserts = app.switches["screwInserts"]
        reveal(inserts, in: app, upward: true)
        inserts.tap()
        waitForState(probe) { number($0, "margin") >= 7.5 }
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = "lid-screws"
        shot.lifetime = .keepAlways
        add(shot)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("lid-screws.png"))
        }
        app.buttons["Undo"].tap()
        app.buttons["Undo"].tap()
        XCTAssertTrue(app.buttons["feature-ScrewPosts"].waitForNonExistence(timeout: 20), "undo removes the screws")
        waitForState(probe) { number($0, "margin") == margin }
    }

    /// Sketch mode on the Connector project: a dragged rectangle and a tapped triangle on the lid
    /// cut through it as one timeline feature; editing the sketch turns the cut into an
    /// extrusion, and undo brings the cut back.
    func testSketch() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Connector", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
        waitForState(probe) { $0["ready"] as? Bool == true }
        app.buttons["Show inspector"].tap()
        let newSketch = app.buttons["newSketch"]
        guard newSketch.waitForExistence(timeout: 10) else { throw XCTSkip("The Connector project is not seeded") }
        let before = number(try read(probe), "lidTriangles")
        reveal(newSketch, in: app, upward: true)
        newSketch.tap()
        let canvas = app.descendants(matching: .any)["sketchCanvas"]
        XCTAssertTrue(canvas.waitForExistence(timeout: 10))
        func point(_ x: Double, _ y: Double) -> XCUICoordinate { canvas.coordinate(withNormalizedOffset: CGVector(dx: x, dy: y)) }
        point(0.35, 0.4).press(forDuration: 0.1, thenDragTo: point(0.5, 0.6))
        app.buttons["Lines"].tap()
        for (x, y) in [(0.65, 0.3), (0.8, 0.3), (0.8, 0.5), (0.65, 0.3)] { point(x, y).tap() }
        XCTAssertEqual(canvas.value as? String, "2 shapes")
        app.buttons["Select"].tap()
        point(0.725, 0.3).tap()  // the triangle's first edge: selects it and shows its bend handle
        point(0.725, 0.3).press(forDuration: 0.1, thenDragTo: point(0.725, 0.24))
        XCTAssertTrue(app.descendants(matching: .any)["arcHeight"].firstMatch.waitForExistence(timeout: 5))
        XCTAssertTrue(app.staticTexts.matching(NSPredicate(format: "label CONTAINS[c] %@", "1 arc")).firstMatch.exists,
                      "the edge became an arc")
        let shot = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
        shot.name = "sketch-editor"
        shot.lifetime = .keepAlways
        add(shot)
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("sketch-editor.png"))
        }
        app.buttons["sketchApply"].tap()
        let cut = app.buttons["feature-UserSketch1Cut"]
        XCTAssertTrue(cut.waitForExistence(timeout: 20), "the cut joins the timeline")
        waitForState(probe) { number($0, "lidTriangles") != before }
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("sketch-cut.png"))
        }
        let edit = app.buttons["editSketch-1"]
        reveal(edit, in: app, upward: true)
        edit.tap()
        app.segmentedControls["sketchOperation"].buttons["Extrude"].tap()
        app.buttons["sketchApply"].tap()
        XCTAssertTrue(app.buttons["feature-UserSketch1Extrude"].waitForExistence(timeout: 20), "the cut became an extrusion")
        app.buttons["Undo"].tap()
        XCTAssertTrue(cut.waitForExistence(timeout: 20), "undo brings the cut back")
    }

    /// A mechanical-only project (Documents/Projects/Box: a 60 x 40 x 20 mm space, seeded by the
    /// run scripts): only Enclosure and Fabricate, a timeline without standoffs or connector
    /// cutouts, vents work, and Fabricate offers STEP without the board's outputs.
    func testMechanicalOnlyProject() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Box", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let probe = app.descendants(matching: .any)["mechanicalProbe"]
        guard probe.waitForExistence(timeout: 45) else { throw XCTSkip("Native FreeCAD edition is not enabled") }
        waitForState(probe) { $0["ready"] as? Bool == true }
        XCTAssertFalse(app.buttons["PCB"].exists, "no board")
        XCTAssertFalse(app.buttons["Schematic"].exists, "no schematic")
        app.buttons["Show inspector"].tap()
        XCTAssertTrue(app.buttons["feature-OuterPad"].waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["feature-BossPad"].exists, "no standoffs without mounting holes")
        XCTAssertFalse(app.staticTexts.matching(NSPredicate(format: "label CONTAINS %@", "connector footprint")).firstMatch.exists)
        let addVents = app.buttons["addLidVents"]
        reveal(addVents, in: app, upward: true)
        addVents.tap()
        XCTAssertTrue(app.buttons["feature-Vent1Pattern"].waitForExistence(timeout: 20), "vents work on a box")
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("mechanical-only.png"))
        }
        switchMode("Fabricate", in: app)
        XCTAssertTrue(app.buttons["export-step"].waitForExistence(timeout: 10), "exact STEP is offered")
        XCTAssertFalse(app.buttons["export-fab"].exists, "no board outputs")
        if let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"] {
            try XCUIScreen.main.screenshot().pngRepresentation
                .write(to: URL(fileURLWithPath: directory).appendingPathComponent("mechanical-only-fabricate.png"))
        }
    }

    private func read(_ probe: XCUIElement) throws -> [String: Any] {
        try XCTUnwrap(JSONSerialization.jsonObject(with: Data(probe.label.utf8)) as? [String: Any])
    }

    private func waitForState(_ probe: XCUIElement, timeout: TimeInterval = 20,
                              _ condition: @escaping ([String: Any]) -> Bool) {
        let predicate = NSPredicate { _, _ in
            guard let value = try? self.read(probe) else { return false }
            return condition(value)
        }
        let expectation = XCTNSPredicateExpectation(predicate: predicate, object: probe)
        XCTAssertEqual(XCTWaiter.wait(for: [expectation], timeout: timeout), .completed, probe.label)
    }

    private func reveal(_ element: XCUIElement, in app: XCUIApplication, upward: Bool) {
        for _ in 0..<6 {
            if element.isHittable { return }
            if upward { app.scrollViews.firstMatch.swipeUp() } else { app.scrollViews.firstMatch.swipeDown() }
        }
        XCTAssertTrue(element.isHittable)
    }
}

private func number(_ state: [String: Any], _ key: String) -> Double {
    (state[key] as? NSNumber)?.doubleValue ?? -1
}

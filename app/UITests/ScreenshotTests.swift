import XCTest

/// Captures every screen in portrait and landscape for visual review. The Demo project
/// must already exist in the app's Documents/Projects (the debug launch hook opens it).
/// Pass TEST_RUNNER_SNAPSHOT_DIR=/path to xcodebuild to write PNGs there.
final class ScreenshotTests: XCTestCase {
    private let screens: [(String, [String: String])] = [
        ("home", [:]),
        ("schematic", ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "schematic"]),
        ("sheet", ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "schematic", "CORVCAD_SHEET": "power-instance"]),
        ("pcb", ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "pcb"]),
        ("enclosure", ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "enclosure"]),
    ]

    func testCaptureScreens() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let device = ProcessInfo.processInfo.environment["SNAPSHOT_TAG"] ?? "ipad"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            for (name, environment) in screens {
                let app = XCUIApplication()
                app.launchEnvironment = environment
                app.launch()
                sleep(3)
                let capture = { (name: String) throws in
                    let shot = XCUIScreen.main.screenshot()
                    let file = "\(device)-\(orientation == .portrait ? "portrait" : "landscape")-\(name).png"
                    if let directory {
                        try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
                    }
                    let attachment = XCTAttachment(screenshot: shot)
                    attachment.name = file
                    attachment.lifetime = .keepAlways
                    self.add(attachment)
                }
                try capture(name)
                if name == "home" {
                    app.buttons["licences"].tap()
                    sleep(2)
                    try capture("licences")
                }
                app.terminate()
            }
        }
    }

    /// The PCB workspace with KiCad DRC findings in the inspector and on the board, from the
    /// debug fixture (CORVCAD_DRC_FIXTURE). Needs the project SNAPSHOT_PROJECT (default RoundTrip,
    /// engine/tests/fixtures/roundtrip) in Documents/Projects; CI copies it into the simulator.
    func testCaptureBoardCheck() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let project = ProcessInfo.processInfo.environment["SNAPSHOT_PROJECT"] ?? "RoundTrip"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "pcb", "CORVCAD_DRC_FIXTURE": "1"]
            app.launch()
            app.buttons["Show inspector"].tap()
            XCTAssertTrue(app.buttons["runDRC"].waitForExistence(timeout: 5), "DRC section shown")
            XCTAssertTrue(app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error'")).firstMatch.exists)
            if project == "ecc83-pp" {
                XCTAssertTrue(app.buttons["fillZones"].exists, "refill offered on a board with a zone and no teardrops")
            }
            sleep(1)
            let shot = XCUIScreen.main.screenshot()
            let file = "drc-\(project)-\(orientation == .portrait ? "portrait" : "landscape").png"
            if let directory {
                try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
            }
            let attachment = XCTAttachment(screenshot: shot)
            attachment.name = file
            attachment.lifetime = .keepAlways
            add(attachment)
            app.terminate()
        }
    }

    /// The Draw tool on SNAPSHOT_PROJECT's board (default ecc83-pp) in both orientations: a zone
    /// drawn corner by corner, then closed, then a silkscreen text.
    func testCaptureDrawing() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let project = ProcessInfo.processInfo.environment["SNAPSHOT_PROJECT"] ?? "ecc83-pp"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "pcb"]
            app.launch()
            sleep(2)
            let side = orientation == .portrait ? "portrait" : "landscape"
            let capture = { (name: String) throws in
                let shot = XCUIScreen.main.screenshot()
                let file = "draw-\(name)-\(side).png"
                if let directory {
                    try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
                }
                let attachment = XCTAttachment(screenshot: shot)
                attachment.name = file
                attachment.lifetime = .keepAlways
                self.add(attachment)
            }
            // The project opens editable a moment after launch; the Draw chip shows the tool took.
            let draw = app.buttons["Draw"].firstMatch
            XCTAssertTrue(draw.waitForExistence(timeout: 5))
            draw.tap()
            if !app.segmentedControls["drawKind"].waitForExistence(timeout: 3) { draw.tap() }
            XCTAssertTrue(app.segmentedControls["drawKind"].waitForExistence(timeout: 3), "Draw tool selected")
            let window = app.windows.firstMatch
            for (x, y) in [(0.42, 0.42), (0.58, 0.42), (0.58, 0.56)] {
                window.coordinate(withNormalizedOffset: CGVector(dx: x, dy: y)).tap()
                usleep(400_000)
            }
            try capture("zone-open")
            XCTAssertTrue(app.buttons["drawClose"].waitForExistence(timeout: 3), "Close offered after three corners")
            app.buttons["drawClose"].tap()
            sleep(2)
            XCTAssertFalse(app.buttons["drawClose"].exists, "the zone was committed")
            app.segmentedControls["drawKind"].buttons["Text"].tap()
            window.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.62)).tap()
            sleep(2)
            try capture("text")
            app.terminate()
        }
    }

    /// The project library browser and the symbol editor on SNAPSHOT_PROJECT (default ecc83-pp,
    /// whose own ecc83-pp.kicad_sym is an editable project library), in both orientations: a new
    /// symbol is named, gets a pin and a rectangle on the canvas, is saved and listed, and the
    /// workspace's undo takes it out of the library again.
    func testCaptureLibraries() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let project = ProcessInfo.processInfo.environment["SNAPSHOT_PROJECT"] ?? "ecc83-pp"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "schematic"]
            app.launch()
            let side = orientation == .portrait ? "portrait" : "landscape"
            let capture = { (name: String) throws in
                let shot = XCUIScreen.main.screenshot()
                let file = "libraries-\(name)-\(side).png"
                if let directory {
                    try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
                }
                let attachment = XCTAttachment(screenshot: shot)
                attachment.name = file
                attachment.lifetime = .keepAlways
                self.add(attachment)
            }
            app.buttons["Show inspector"].tap()
            XCTAssertTrue(app.buttons["projectLibraries"].waitForExistence(timeout: 5), "Libraries section shown")
            app.buttons["projectLibraries"].tap()
            let add = app.buttons["New symbol in \(project)"]
            XCTAssertTrue(add.waitForExistence(timeout: 5), "the project's own symbol library is editable")
            sleep(1)
            try capture("browser")
            add.tap()
            let symbol = "CorvTest\(side.capitalized)"
            let name = app.textFields["field-name"]
            XCTAssertTrue(name.waitForExistence(timeout: 5), "editor opened on the template")
            name.tap()
            name.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 12) + symbol + "\n")
            let canvas = app.otherElements["itemCanvas"]
            app.buttons["Pin"].firstMatch.tap()
            canvas.coordinate(withNormalizedOffset: CGVector(dx: 0.2, dy: 0.25)).tap()
            app.buttons["Rect"].firstMatch.tap()
            canvas.coordinate(withNormalizedOffset: CGVector(dx: 0.6, dy: 0.6))
                .press(forDuration: 0.1, thenDragTo: canvas.coordinate(withNormalizedOffset: CGVector(dx: 0.8, dy: 0.8)))
            sleep(1)
            try capture("symbol")
            app.buttons["saveItem"].tap()
            XCTAssertTrue(app.staticTexts[symbol].waitForExistence(timeout: 5), "saved symbol listed")
            app.buttons["Close"].firstMatch.tap()
            XCTAssertTrue(app.buttons["Undo"].waitForExistence(timeout: 5))
            app.buttons["Undo"].tap()
            app.buttons["projectLibraries"].tap()
            XCTAssertTrue(app.buttons["New symbol in \(project)"].waitForExistence(timeout: 5))
            XCTAssertFalse(app.staticTexts[symbol].exists, "undo took the symbol out of the library")
            // The footprint editor on a new footprint library's template.
            let library = "CorvFP\(side.capitalized)"
            app.buttons["newFootprintLibrary"].tap()
            let field = app.alerts.textFields.firstMatch
            XCTAssertTrue(field.waitForExistence(timeout: 5))
            field.typeText(library)
            app.alerts.buttons["Create"].tap()
            let newFootprint = app.buttons["New footprint in \(library)"]
            XCTAssertTrue(newFootprint.waitForExistence(timeout: 5), "new footprint library listed")
            newFootprint.tap()
            XCTAssertTrue(app.buttons["saveItem"].waitForExistence(timeout: 5))
            app.buttons["Pad"].firstMatch.tap()
            app.otherElements["itemCanvas"].coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)).tap()
            sleep(1)
            try capture("footprint")
            app.terminate()
        }
    }

    /// The PCB calculator's tabs from the home screen, and the Gerber viewer on GERBER_FOLDER (a
    /// folder under Documents, default Gerbers/RoundTrip), in both orientations.
    func testCaptureTools() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let folder = ProcessInfo.processInfo.environment["GERBER_FOLDER"] ?? "Gerbers/RoundTrip"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let side = orientation == .portrait ? "portrait" : "landscape"
            let capture = { (name: String) throws in
                let shot = XCUIScreen.main.screenshot()
                let file = "tools-\(name)-\(side).png"
                if let directory {
                    try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
                }
                let attachment = XCTAttachment(screenshot: shot)
                attachment.name = file
                attachment.lifetime = .keepAlways
                self.add(attachment)
            }
            var app = XCUIApplication()
            app.launch()
            XCTAssertTrue(app.buttons["openCalculator"].waitForExistence(timeout: 5))
            sleep(1)
            try capture("home")
            app.buttons["openCalculator"].tap()
            for tab in ["Track width", "Via", "Clearance", "Regulator", "Resistor"] {
                app.segmentedControls["calculatorTab"].buttons[tab].tap()
                sleep(1)
                try capture("calculator-\(tab.lowercased().replacingOccurrences(of: " ", with: "-"))")
            }
            XCTAssertTrue(app.staticTexts["yellow violet red gold"].exists, "4.7 kΩ E24 colour bands")
            app.terminate()

            app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_GERBERS": folder]
            app.launch()
            XCTAssertTrue(app.otherElements["gerberCanvas"].waitForExistence(timeout: 10), "viewer opened")
            sleep(2)
            try capture("gerbers")
            let firstLayer = app.buttons.matching(NSPredicate(format: "label ENDSWITH ', shown'")).firstMatch
            XCTAssertTrue(firstLayer.exists, "layers listed")
            firstLayer.tap()
            XCTAssertTrue(app.buttons.matching(NSPredicate(format: "label ENDSWITH ', hidden'")).firstMatch.exists, "a layer hides")
            app.terminate()
        }
    }

    /// The schematic of SNAPSHOT_PROJECT (default ecc83-pp, KiCad's demo with library symbols) in
    /// both orientations, for checking how library symbols, their fields and pin texts draw.
    func testCaptureSchematic() throws {
        let directory = ProcessInfo.processInfo.environment["SNAPSHOT_DIR"]
        let project = ProcessInfo.processInfo.environment["SNAPSHOT_PROJECT"] ?? "ecc83-pp"
        for orientation in [UIDeviceOrientation.portrait, .landscapeLeft] {
            XCUIDevice.shared.orientation = orientation
            let app = XCUIApplication()
            app.launchEnvironment = ["CORVCAD_OPEN": project, "CORVCAD_MODE": "schematic"]
            app.launch()
            sleep(2)
            let shot = XCUIScreen.main.screenshot()
            let file = "schematic-\(project)-\(orientation == .portrait ? "portrait" : "landscape").png"
            if let directory {
                try shot.pngRepresentation.write(to: URL(fileURLWithPath: directory).appendingPathComponent(file))
            }
            let attachment = XCTAttachment(screenshot: shot)
            attachment.name = file
            attachment.lifetime = .keepAlways
            add(attachment)
            app.terminate()
        }
    }
}

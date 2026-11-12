import XCTest

/// A paced, scripted walk through the schematic and PCB workflows in landscape, for the demo
/// video (.github/workflows/demo-video.yml records the simulator while it runs). Needs the
/// NATIVE_KICAD edition and the ecc83-pp and ecc83-unrouted projects (see NativeKiCadTests).
/// Lenient: a step that cannot be done is skipped, so the recording continues.
final class DemoVideoTests: XCTestCase {
    func testDemoWalkthrough() throws {
        XCUIDevice.shared.orientation = .landscapeLeft
        let app = XCUIApplication()
        app.launch()
        pause(3)

        // Schematic: KiCad's ecc83 demo, drawn from its own library symbols.
        open(app, project: "ecc83-pp")
        tapIfExists(app.buttons["Schematic"].firstMatch)
        pause(3)
        app.windows.firstMatch.pinch(withScale: 1.6, velocity: 0.8)
        pause(2)
        app.windows.firstMatch.pinch(withScale: 0.65, velocity: -0.8)
        pause(2)

        // Place an official KiCad part and pick its footprint.
        app.buttons["Part"].firstMatch.tap()
        if app.buttons["choosePart"].waitForExistence(timeout: 3) {
            app.buttons["choosePart"].tap()
            let search = app.textFields["partSearch"]
            if search.waitForExistence(timeout: 5) {
                search.tap()
                slowType(search, "LM358")
                pause(1.5)
                let part = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'LM358'")).firstMatch
                if part.waitForExistence(timeout: 5) { part.tap() }
            }
            pause(1)
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.78, dy: 0.72)).tap()
            let footprints = app.textFields["footprintSearch"]
            if footprints.waitForExistence(timeout: 10) {
                pause(2)
                footprints.tap()
                slowType(footprints, "SOIC-8_3.9")
                pause(1.5)
                let soic = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'SOIC-8_3.9x4.9mm_P1.27mm'")).firstMatch
                if soic.waitForExistence(timeout: 5) { soic.tap() }
            }
        }
        pause(3)
        tapIfExists(app.buttons["Select"].firstMatch)
        pause(1)

        // PCB: the same board without tracks, routed with KiCad's push-and-shove router.
        tapIfExists(app.buttons["Projects"].firstMatch)
        pause(2)
        open(app, project: "ecc83-unrouted")
        tapIfExists(app.buttons["PCB"].firstMatch)
        pause(3)
        let probe = app.descendants(matching: .any)["ratsnestProbe"]
        app.buttons["Track"].firstMatch.tap()
        pause(2)
        if probe.waitForExistence(timeout: 10) {
            for _ in 0..<3 {
                let edge = numbers(probe.label)
                guard edge.count >= 7, edge[6] > 0 else { break }
                tap(app, edge[0], edge[1])
                pause(1.2)
                tap(app, edge[2], edge[3])
                pause(2.5)
            }
            // One connection on B.Cu through a via half way.
            let edge = numbers(probe.label)
            if edge.count >= 7, edge[6] > 0 {
                tapIfExists(app.buttons["B.Cu"].firstMatch)
                pause(1)
                tap(app, edge[0], edge[1])
                pause(1)
                tapIfExists(app.buttons["routeVia"].firstMatch)
                pause(1)
                tap(app, (edge[0] + edge[2]) / 2, (edge[1] + edge[3]) / 2)
                pause(1.5)
                tap(app, edge[2], edge[3])
                pause(2.5)
            }
        }
        tapIfExists(app.buttons["Select"].firstMatch)
        pause(1)

        // KiCad's DRC from the inspector.
        tapIfExists(app.buttons["Show inspector"].firstMatch)
        pause(1.5)
        if app.buttons["runDRC"].waitForExistence(timeout: 5) {
            app.buttons["runDRC"].tap()
            let finding = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Error' OR label BEGINSWITH 'Warning'"))
            if finding.firstMatch.waitForExistence(timeout: 60) {
                pause(2)
                finding.firstMatch.tap()
            }
        }
        pause(4)

        // The enclosure around the board.
        tapIfExists(app.buttons["Enclosure"].firstMatch)
        pause(5)
    }

    private func open(_ app: XCUIApplication, project: String) {
        let card = app.buttons.matching(NSPredicate(format: "label CONTAINS %@", project)).firstMatch
        if card.waitForExistence(timeout: 10) { card.tap() }
        pause(2)
    }

    private func tapIfExists(_ element: XCUIElement) {
        if element.waitForExistence(timeout: 3) { element.tap() }
    }

    /// Types one character at a time, so the search reads on video.
    private func slowType(_ field: XCUIElement, _ text: String) {
        for character in text {
            field.typeText(String(character))
            Thread.sleep(forTimeInterval: 0.18)
        }
    }

    private func pause(_ seconds: Double) {
        Thread.sleep(forTimeInterval: seconds)
    }

    /// ratsnestProbe's label: x0 y0 x1 y1 tracks vias unrouted (window points).
    private func numbers(_ label: String) -> [Double] {
        label.split(separator: " ").compactMap { Double($0) }
    }

    private func tap(_ app: XCUIApplication, _ x: Double, _ y: Double) {
        app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: x, dy: y)).tap()
    }
}

import XCTest

/// New projects choose their parts: an electronics-only project has no Enclosure tab until
/// Fabricate adds one, and the choice survives a relaunch. Pass SNAPSHOT_DIR to write PNGs.
final class ProjectPartsTests: XCTestCase {
    func testElectronicsOnlyProjectGainsAnEnclosure() throws {
        let name = "Parts\(Int(Date().timeIntervalSince1970) % 1_000_000)"
        var app = XCUIApplication()
        for orientation in [UIDeviceOrientation.landscapeLeft, .portrait] {
            XCUIDevice.shared.orientation = orientation
            app.launch()
            app.buttons["New project"].firstMatch.tap()
            XCTAssertTrue(app.buttons["engine-Enclosure"].waitForExistence(timeout: 10))
            try capture("new-project-\(orientation == .portrait ? "portrait" : "landscape")")
            if orientation == .landscapeLeft { app.buttons["Cancel"].firstMatch.tap(); app.terminate() }
        }

        let enclosureCard = app.buttons["engine-Enclosure"]
        XCTAssertEqual(enclosureCard.value as? String, "On", "new projects have an enclosure by default")
        enclosureCard.tap()
        XCTAssertEqual(enclosureCard.value as? String, "Off")
        app.buttons["engine-Electronics"].tap()
        XCTAssertEqual(app.buttons["engine-Electronics"].value as? String, "Always on", "electronics stays on")
        let field = app.textFields.firstMatch
        field.tap()
        field.typeText(name)
        app.buttons["Create project"].tap()

        XCTAssertTrue(app.buttons["Fabricate"].firstMatch.waitForExistence(timeout: 30))
        XCTAssertFalse(app.buttons["Enclosure"].exists, "no Enclosure tab without an enclosure")
        app.buttons["Fabricate"].firstMatch.tap()
        let add = app.buttons["addEnclosure"]
        XCTAssertTrue(add.waitForExistence(timeout: 10))
        XCTAssertFalse(app.buttons["export-traySTL"].exists, "no enclosure outputs")
        try capture("fabricate-without-enclosure")
        add.tap()
        let enclosure = app.buttons["Enclosure"].firstMatch
        XCTAssertTrue(enclosure.waitForExistence(timeout: 30))
        XCTAssertTrue(enclosure.isSelected, "adding the enclosure opens it")

        app.terminate()
        app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": name, "CORVCAD_MODE": "pcb"]
        app.launch()
        XCTAssertTrue(app.buttons["Enclosure"].firstMatch.waitForExistence(timeout: 30), "the added enclosure persists")
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

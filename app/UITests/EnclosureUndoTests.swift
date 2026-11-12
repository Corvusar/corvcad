import XCTest

/// An enclosure slider change is one undo step: undo restores the value, redo reapplies it.
/// Uses the Demo project and leaves its parameters as they were.
final class EnclosureUndoTests: XCTestCase {
    func testSliderUndoRedo() throws {
        let app = XCUIApplication()
        app.launchEnvironment = ["CORVCAD_OPEN": "Demo", "CORVCAD_MODE": "enclosure"]
        app.launch()
        let show = app.buttons["Show inspector"]
        if show.waitForExistence(timeout: 5) { show.tap() }
        let slider = app.sliders.firstMatch
        // On a cold simulator the first tap can land before the workspace takes touches.
        if !slider.waitForExistence(timeout: 5), show.exists { show.tap() }
        XCTAssertTrue(slider.waitForExistence(timeout: 10))
        let before = slider.value as? String
        slider.adjust(toNormalizedSliderPosition: before == "90%" ? 0.2 : 0.9)
        sleep(2)
        let after = slider.value as? String
        XCTAssertNotEqual(before, after)

        app.buttons["Undo"].firstMatch.tap()
        sleep(2)
        XCTAssertEqual(slider.value as? String, before)
        app.buttons["Redo"].firstMatch.tap()
        sleep(2)
        XCTAssertEqual(slider.value as? String, after)
        app.buttons["Undo"].firstMatch.tap()
    }
}

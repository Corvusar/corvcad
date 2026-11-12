import SwiftUI

@main
struct CorvCADApp: App {
    init() {
        // NATIVE_KICAD edition: KiCad's official symbol and footprint libraries are bundled in
        // KiCadSymbols and KiCadFootprints. The global library tables refer to them through
        // KiCad's own variables.
        for (folder, variable) in [("KiCadSymbols", "KICAD10_SYMBOL_DIR"), ("KiCadFootprints", "KICAD10_FOOTPRINT_DIR")] {
            if let url = Bundle.main.url(forResource: folder, withExtension: nil) {
                setenv(variable, url.path, 1)
            }
        }
        #if DEBUG
        if ProcessInfo.processInfo.environment["CORVCAD_NO_KEYBOARD"] != nil { KeyboardSuppression.install() }
        #endif
    }

    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

#if DEBUG
/// Recording aid for the showcase takes (launch environment `CORVCAD_NO_KEYBOARD`): text fields and
/// text views take an empty input view and an empty shortcuts bar, so typing arrives as key events
/// (XCUITest's `typeText`, a hardware keyboard) and no on-screen keyboard covers the app.
private enum KeyboardSuppression {
    private static let empty = UIView(frame: .zero)

    static func install() {
        for cls in [UITextField.self, UITextView.self] as [AnyClass] {
            if let method = class_getInstanceMethod(cls, #selector(getter: UIResponder.inputView)) {
                let block: @convention(block) (AnyObject) -> UIView? = { _ in empty }
                method_setImplementation(method, imp_implementationWithBlock(block))
            }
            let selector = #selector(getter: UIResponder.inputAssistantItem)
            if let method = class_getInstanceMethod(cls, selector) {
                typealias Getter = @convention(c) (AnyObject, Selector) -> UITextInputAssistantItem
                let original = unsafeBitCast(method_getImplementation(method), to: Getter.self)
                let block: @convention(block) (AnyObject) -> UITextInputAssistantItem = { object in
                    let item = original(object, selector)
                    item.leadingBarButtonGroups = []
                    item.trailingBarButtonGroups = []
                    return item
                }
                method_setImplementation(method, imp_implementationWithBlock(block))
            }
        }
    }
}
#endif

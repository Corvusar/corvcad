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
    }

    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

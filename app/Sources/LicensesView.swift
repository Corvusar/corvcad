import SwiftUI

/// Licence information. The sideloaded edition links KiCad 10.0.6 and bundles its retained
/// notices (`notices/`, from integrations/kicad_core/audit); their presence identifies it.
struct LicensesView: View {
    let onClose: () -> Void

    static let sourceURL = URL(string: "https://github.com/Corvusar/corvcad")!
    private static let notices: [URL] = (Bundle.main.url(forResource: "notices", withExtension: nil))
        .flatMap { try? FileManager.default.contentsOfDirectory(at: $0, includingPropertiesForKeys: nil) }?
        .sorted { $0.lastPathComponent < $1.lastPathComponent } ?? []
    static var nativeEdition: Bool { !notices.isEmpty }
    private static let freeCADNotices: [URL] = {
        guard let root = Bundle.main.resourceURL?.appendingPathComponent("FreeCADRuntime/notices"),
              let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: [.isRegularFileKey],
                                                         options: [.skipsHiddenFiles]) else { return [] }
        return files.compactMap { $0 as? URL }.filter {
            (try? $0.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true
        }.sorted { $0.path < $1.path }
    }()

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack {
                VStack(alignment: .leading, spacing: 6) {
                    MonoLabel("Licences", color: Corv.gold)
                    Text(MechanicalSession.enabled ? "FreeCAD edition" : Self.nativeEdition ? "KiCad core edition" : "CorvCAD")
                        .font(.corvDisplay(30)).foregroundStyle(Corv.text)
                }
                Spacer()
                Button("Done", action: onClose).buttonStyle(.corvGhost)
            }
            if MechanicalSession.enabled {
                Text("This development edition includes FreeCAD and its dependencies, and CorvCAD's port of KiCad's 3D model generator, which is GPL-3.0-or-later, so the app as a whole is distributed under the GNU General Public License, version 3 or later. CorvCAD's own code remains MIT; each bundled component retains its licence and notices below. Distribution requires the corresponding source and relinking materials described by those licences.")
                    .font(.callout).foregroundStyle(Corv.textDim).fixedSize(horizontal: false, vertical: true)
                Link(destination: Self.sourceURL) { Label("Source code", systemImage: "arrow.up.right.square") }
                    .foregroundStyle(Corv.gold)
            } else if Self.nativeEdition {
                Text("This edition links KiCad 10.0.6 and its dependencies, so the app as a whole is distributed under the GNU General Public License, version 3 or later. Each file keeps its own licence; CorvCAD's own code remains MIT. The complete corresponding source for each release, including KiCad, its patches, wxWidgets and the dependency sources, is published with that release.")
                    .font(.callout).foregroundStyle(Corv.textDim).fixedSize(horizontal: false, vertical: true)
                Link(destination: Self.sourceURL) { Label("Source code", systemImage: "arrow.up.right.square") }
                    .foregroundStyle(Corv.gold)
            } else {
                Text("CorvCAD is free software under the MIT licence. This edition contains no KiCad code.")
                    .font(.callout).foregroundStyle(Corv.textDim)
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 10) {
                    notice("CorvCAD (MIT)", Bundle.main.url(forResource: "LICENSE", withExtension: nil))
                    ForEach(Self.notices, id: \.self) { url in
                        // Audit file names end in a content hash: kicad-LICENSE-3972dc9744f6.txt
                        notice(url.deletingPathExtension().lastPathComponent
                            .replacingOccurrences(of: "-[0-9a-f]{12}$", with: "", options: .regularExpression), url)
                    }
                    if let generator = Bundle.main.url(forResource: "parts3d-notices/kicad-packages3D-generator-LICENSE",
                                                       withExtension: "txt") {
                        notice("kicad-packages3D-generator (GPL-3.0-or-later)", generator)
                    }
                    ForEach(Self.freeCADNotices, id: \.self) { url in
                        let prefix = "/FreeCADRuntime/notices/"
                        notice(url.path.components(separatedBy: prefix).last ?? url.lastPathComponent, url)
                    }
                }
            }
        }
        .padding(28)
        .background(VoidBackground())
        .presentationBackground(Corv.space)
    }

    private func notice(_ title: String, _ url: URL?) -> some View {
        DisclosureGroup {
            Text(url.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? "")
                .font(.system(.caption, design: .monospaced)).foregroundStyle(Corv.textDim)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            MonoLabel(title, color: Corv.text, small: true).lineLimit(1)
        }
        .tint(Corv.gold)
        .padding(12)
        .corvPanel(radius: 12)
    }
}

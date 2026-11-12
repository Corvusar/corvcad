import SwiftUI

/// The official KiCad symbols bundled with the NATIVE_KICAD edition, searched from the index
/// that fetch.py writes beside them (KiCadSymbols/index.json), so no library loads until a part
/// is placed. Nil in the compatible edition.
final class PartLibrary {
    struct Part: Identifiable, Hashable {
        let library: String
        let name: String
        let prefix: String
        let description: String
        let keywords: String
        let footprint: String
        let pins: Int
        /// KiCad's footprint filters (ki_fp_filters): wildcards on footprint names.
        let filters: [String]
        /// Lowercased name, description, keywords and library, matched by search.
        let haystack: String
        var id: String { "\(library):\(name)" }
    }

    let parts: [Part]

    /// Nil without the bundled index (compatible edition) or when the session cannot place
    /// library symbols.
    static func bundled() -> PartLibrary? {
        guard let url = Bundle.main.url(forResource: "index", withExtension: "json", subdirectory: "KiCadSymbols"),
              let data = try? Data(contentsOf: url),
              let rows = try? JSONSerialization.jsonObject(with: data) as? [[Any]] else { return nil }
        let parts = rows.compactMap { row -> Part? in
            guard row.count >= 7, let library = row[0] as? String, let name = row[1] as? String else { return nil }
            let description = row[3] as? String ?? "", keywords = row[4] as? String ?? ""
            return Part(library: library, name: name, prefix: row[2] as? String ?? "",
                        description: description, keywords: keywords,
                        footprint: row[5] as? String ?? "", pins: row[6] as? Int ?? 0,
                        filters: (row.count > 8 ? row[8] as? String ?? "" : "").split(separator: " ").map(String.init),
                        haystack: "\(name) \(description) \(keywords) \(library)".lowercased())
        }
        return parts.isEmpty ? nil : PartLibrary(parts: parts)
    }

    private init(parts: [Part]) { self.parts = parts }

    /// This index with the project's own symbols first (ProjectLibraries.parts).
    func adding(_ extra: [Part]) -> PartLibrary { extra.isEmpty ? self : PartLibrary(parts: extra + parts) }

    /// The part a placed symbol came from ("Library:Name").
    func part(_ id: String) -> Part? { parts.first { $0.id == id } }

    /// Parts matching every word of the query in their name, description, keywords or library;
    /// exact and prefix name matches first. ponytail: a linear scan of ~23,000 rows per query.
    func search(_ query: String, limit: Int = 120) -> [Part] {
        let words = query.lowercased().split(separator: " ").map(String.init)
        guard !words.isEmpty else { return [] }
        let first = words[0]
        let matches = parts.filter { part in words.allSatisfy { part.haystack.contains($0) } }
        func rank(_ part: Part) -> Int {
            let name = part.name.lowercased()
            return name == first ? 0 : name.hasPrefix(first) ? 1 : name.contains(first) ? 2 : 3
        }
        return Array(matches.sorted { (rank($0), $0.name.count, $0.id) < (rank($1), $1.name.count, $1.id) }.prefix(limit))
    }
}

/// The official KiCad footprints bundled with the NATIVE_KICAD edition, from the index fetch.py
/// writes beside them (KiCadFootprints/index.json). Nil in the compatible edition.
final class FootprintLibrary {
    struct Footprint: Identifiable, Hashable {
        let library: String
        let name: String
        let pads: Int
        let description: String
        var id: String { "\(library):\(name)" }
    }

    let footprints: [Footprint]

    static func bundled() -> FootprintLibrary? {
        guard let url = Bundle.main.url(forResource: "index", withExtension: "json", subdirectory: "KiCadFootprints"),
              let data = try? Data(contentsOf: url),
              let rows = try? JSONSerialization.jsonObject(with: data) as? [[Any]] else { return nil }
        let footprints = rows.compactMap { row -> Footprint? in
            guard row.count >= 4, let library = row[0] as? String, let name = row[1] as? String else { return nil }
            return Footprint(library: library, name: name, pads: row[2] as? Int ?? 0, description: row[3] as? String ?? "")
        }
        return footprints.isEmpty ? nil : FootprintLibrary(footprints: footprints)
    }

    private init(footprints: [Footprint]) { self.footprints = footprints }

    /// This index with the project's own footprints first.
    func adding(_ extra: [Footprint]) -> FootprintLibrary {
        extra.isEmpty ? self : FootprintLibrary(footprints: extra + footprints)
    }

    /// CvPcb's filtering: any of the symbol's footprint filters matches (wildcards, case
    /// insensitive; against Library:Name when the filter has a colon, else the name), and the
    /// distinct pad count equals the symbol's pin count. No filters: the pin count alone.
    func candidates(filters: [String], pins: Int) -> [Footprint] {
        let patterns = filters.map { ($0.contains(":"), NSPredicate(format: "SELF LIKE[c] %@", $0)) }
        return footprints.filter { footprint in
            (pins == 0 || footprint.pads == pins) &&
                (patterns.isEmpty || patterns.contains { $0.1.evaluate(with: $0.0 ? footprint.id : footprint.name) })
        }
    }
}

/// Chooses a footprint for a placed symbol: the footprints its KiCad filters and pin count
/// allow, or the whole library.
struct FootprintPicker: View {
    let library: FootprintLibrary
    /// "U1 · Amplifier_Operational:LM358".
    let title: String
    let filters: [String]
    let pins: Int
    let onPick: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var all = false
    @State private var matches: [FootprintLibrary.Footprint] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    MonoLabel("Footprint", color: Corv.gold)
                    Text(title).font(.corvReadout).foregroundStyle(Corv.text).lineLimit(1)
                }
                Spacer()
                Button("Close") { dismiss() }.buttonStyle(.corvGhost)
            }
            Toggle(isOn: $all) {
                MonoLabel(all ? "All \(library.footprints.count) footprints"
                              : "Matching the symbol's filters" + (pins > 0 ? " · \(pins) pads" : ""), small: true)
            }
            .tint(Corv.gold)
            TextField("Filter: SOIC, DIP, 0805…", text: $query)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
                .corvField()
                .accessibilityIdentifier("footprintSearch")
            let words = query.lowercased().split(separator: " ")
            let shown = matches.filter { footprint in
                words.allSatisfy { footprint.id.lowercased().contains($0) || footprint.description.lowercased().contains($0) }
            }
            if shown.isEmpty {
                Text(all ? "No footprint matches." : "No footprint matches the symbol's filters; turn on all footprints.")
                    .font(.caption).foregroundStyle(Corv.textFaint)
            }
            ScrollView {
                // ponytail: first 300 rows; narrow with the filter field.
                LazyVStack(alignment: .leading, spacing: 6) {
                    ForEach(shown.prefix(300)) { footprint in
                        Button {
                            onPick(footprint.id)
                            dismiss()
                        } label: {
                            FootprintRow(footprint: footprint)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .padding(20)
        .background(Corv.void)
        .task(id: all) {
            matches = all ? library.footprints : library.candidates(filters: filters, pins: pins)
        }
    }
}

private struct FootprintRow: View {
    let footprint: FootprintLibrary.Footprint

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                Text(footprint.name).font(.corvReadout).foregroundStyle(Corv.text).lineLimit(1)
                Spacer(minLength: 0)
                MonoLabel("\(footprint.pads) pads", color: Corv.cyan, small: true)
            }
            MonoLabel(footprint.library, color: Corv.textFaint, small: true)
            if !footprint.description.isEmpty {
                Text(footprint.description).font(.caption).foregroundStyle(Corv.textDim).lineLimit(2)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
    }
}

/// Search sheet for the schematic's Part tool.
struct PartPicker: View {
    let library: PartLibrary
    let onPick: (PartLibrary.Part) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                MonoLabel("Symbols · \(library.parts.count)", color: Corv.gold)
                Spacer()
                Button("Close") { dismiss() }.buttonStyle(.corvGhost)
            }
            TextField("Search: resistor, LED, NE555, Conn_01x04…", text: $query)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
                .corvField()
                .accessibilityIdentifier("partSearch")
            let results = library.search(query)
            if query.isEmpty {
                Text("Search names, descriptions and keywords of the project's libraries and KiCad's official symbol libraries.")
                    .font(.caption).foregroundStyle(Corv.textFaint)
            } else if results.isEmpty {
                Text("No symbol matches “\(query)”.").font(.caption).foregroundStyle(Corv.textFaint)
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 6) {
                    ForEach(results) { part in
                        Button {
                            onPick(part)
                            dismiss()
                        } label: {
                            PartRow(part: part)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .padding(20)
        .background(Corv.void)
    }
}

private struct PartRow: View {
    let part: PartLibrary.Part

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                Text(part.name).font(.corvReadout).foregroundStyle(Corv.text)
                MonoLabel(part.library, color: Corv.textFaint, small: true)
                Spacer(minLength: 0)
                MonoLabel("\(part.pins) pins", color: Corv.cyan, small: true)
            }
            if !part.description.isEmpty {
                Text(part.description).font(.caption).foregroundStyle(Corv.textDim).lineLimit(2)
            }
            if !part.footprint.isEmpty {
                MonoLabel(part.footprint, color: Corv.textFaint, small: true)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.03), in: .rect(cornerRadius: 10))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Corv.line))
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
    }
}

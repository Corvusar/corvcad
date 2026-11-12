// Mixed native C ABI integration smoke test. MIT. No Python/FreeCAD/KiCad C++ types.
// argv: kicad-first|freecad-first runtime source-project input-json fresh-output
// Static library initializers necessarily precede main; order refers to C ABI initialization.
#include "corvcad/c_api.h"
#include "corvcad/schematic_session_c_api.h"
#include "corvcad/board_native_c_api.h"
#include "corvcad/mechanical_session_c_api.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
// Exact current engine/src/c_api.cpp cc_default_settings() values; avoid linking the
// portable adapter/stub translation units into the mixed native-only probe.
constexpr cc_settings defaults{3, 2, 2, 2, 4, 6, 2, 0.25};
constexpr char native_backend[] = "kicad-10.0.6-native"; // current public backend contract
constexpr char label[] = "CORVCAD_MIXED_C_ABI";
std::array<char, 4096> error{};
struct Result {
    cc_sch_result value{};
    ~Result() { cc_sch_result_clear(&value); }
} result;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void mechanical(int okay, const char* operation) {
    require(okay == 1, std::string(operation) + ": " + error.data());
}
void schematic(int okay, const char* operation) {
    require(okay != 0, std::string(operation) + ": " + (result.value.message ? result.value.message : "no error detail"));
}
QByteArray read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot read " + path.string());
    const std::string data{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    return QByteArray(data.data(), static_cast<qsizetype>(data.size()));
}
void write(const fs::path& path, const QByteArray& data) {
    require(!fs::exists(path), "Refusing an existing output: " + path.string());
    std::ofstream stream(path, std::ios::binary);
    stream.write(data.constData(), data.size());
    require(bool(stream), "Cannot write " + path.string());
}
QJsonObject json(const QByteArray& text) {
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(text, &parse);
    require(parse.error == QJsonParseError::NoError && document.isObject(), "Expected actual native JSON object");
    return document.object();
}
QString hash(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
QJsonObject file_record(const fs::path& path) {
    const auto data = read(path);
    return {{"bytes", static_cast<qint64>(data.size())}, {"sha256", hash(data)}};
}
QByteArray owned(char* text, const char* operation) {
    require(text != nullptr, std::string(operation) + ": " + (result.value.message ? result.value.message : error.data()));
    QByteArray bytes(text);
    cc_free_text(text); // native KiCad exports a weak standalone allocator-compatible implementation
    return bytes;
}
QJsonObject snapshot(cc_sch_session* session) {
    auto state = json(owned(cc_sch_session_snapshot_json(session, &result.value), "schematic snapshot"));
    // Native serialization/undo may change traversal order without changing items.
    // Compare supported item content, as the accepted schematic ABI harness does.
    QJsonArray canonical;
    for (const auto& value : state["sheets"].toArray()) {
        auto sheet = value.toObject();
        for (const auto& key : sheet.keys()) {
            if (!sheet[key].isArray()) continue;
            std::vector<QJsonValue> items;
            for (const auto& item : sheet[key].toArray()) items.push_back(item);
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
                return QJsonDocument(a.toObject()).toJson(QJsonDocument::Compact)
                     < QJsonDocument(b.toObject()).toJson(QJsonDocument::Compact);
            });
            QJsonArray array;
            for (const auto& item : items) array.append(item);
            sheet[key] = array;
        }
        canonical.append(sheet);
    }
    state["sheets"] = canonical;
    return state;
}
int label_count(const QJsonObject& state) {
    int count = 0;
    for (const auto& sheet : state["sheets"].toArray())
        for (const auto& item : sheet.toObject()["labels"].toArray())
            if (item.toObject()["text"].toString() == label) ++count;
    return count;
}
void settings(cc_mechanical_session* session, double expected_wall) {
    cc_settings actual{};
    mechanical(cc_mechanical_get_settings(session, &actual, error.data(), error.size()), "mechanical settings");
    require(std::abs(actual.wall - expected_wall) < 1e-9 && actual.margin == defaults.margin
        && actual.floor == defaults.floor && actual.board_clearance == defaults.board_clearance
        && actual.standoff_height == defaults.standoff_height && actual.standoff_diameter == defaults.standoff_diameter
        && actual.lid_thickness == defaults.lid_thickness && actual.lid_gap == defaults.lid_gap,
        "Mechanical parameters changed across native engine calls");
}
QJsonObject previews(cc_mechanical_session* session) {
    QJsonObject output;
    for (int part : {0, 1}) {
        const auto count = cc_mechanical_triangles(session, part, nullptr, 0, error.data(), error.size());
        require(count > 0, std::string("No exact-solid preview: ") + error.data());
        std::vector<cc_triangle> triangles(count);
        require(cc_mechanical_triangles(session, part, triangles.data(), triangles.size(), error.data(), error.size()) == count,
                "Incomplete native preview read-back");
        for (const auto& t : triangles)
            for (float coordinate : {t.ax,t.ay,t.az,t.bx,t.by,t.bz,t.cx,t.cy,t.cz})
                require(std::isfinite(coordinate), "Nonfinite native preview coordinate");
        output[part == 0 ? "tray" : "lid"] = static_cast<qint64>(count);
    }
    return output;
}
void kicad_checks(cc_sch_session* session, const fs::path& board, const fs::path& output, const std::string& prefix) {
    auto netlist = owned(cc_sch_session_netlist(session, &result.value), "native netlist");
    require(netlist.contains("(export"), "Native netlist missing exporter result");
    write(output / (prefix + ".net"), netlist);
    auto erc = owned(cc_sch_session_erc_json(session, &result.value), "native ERC");
    require(json(erc).contains("sheets"), "Native ERC report lacks sheets");
    write(output / (prefix + "-erc.json"), erc);
    for (int refill : {0, 1}) {
        auto drc = owned(cc_board_native_drc(board.c_str(), refill, error.data(), error.size()), "native board DRC");
        const auto data = json(drc);
        require(data.contains("violations") && data.contains("unconnected_items"), "Native DRC report lacks violation lists");
        write(output / (prefix + (refill ? "-refill-drc.json" : "-drc.json")), drc);
    }
    write(output / (prefix + "-ratsnest.txt"), owned(cc_board_native_ratsnest(board.c_str(), error.data(), error.size()), "native ratsnest"));
}

QByteArray board_setup(const fs::path& board) {
    std::unique_ptr<char, decltype(&cc_free_text)> text(
        cc_board_native_board_setup(board.c_str(), error.data(), error.size()), &cc_free_text);
    require(bool(text), std::string("Read native Board Setup: ") + error.data());
    return QByteArray(text.get());
}
std::multiset<QString> pcb_uuids(const QByteArray& board) {
    const QRegularExpression expression(QString::fromLatin1(
        R"UUID(\(uuid\s+"?([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})"?\s*\))UUID"));
    require(expression.isValid(), "Invalid PCB UUID check expression");
    std::multiset<QString> identifiers;
    auto matches = expression.globalMatch(QString::fromUtf8(board));
    while (matches.hasNext()) identifiers.insert(matches.next().captured(1).toLower());
    return identifiers;
}
QJsonObject edit_board(cc_mechanical_session* mech, const fs::path& board, const fs::path& project_file,
                      const std::map<fs::path, QByteArray>& original, const fs::path& output) {
    const auto original_uuids = pcb_uuids(read(board));
    require(!original_uuids.empty(), "Original PCB has no UUID-bearing items to check");
    const auto before = board_setup(board);
    auto lines = before.split('\n');
    double old_clearance = 0, new_clearance = 0;
    int changed = 0;
    for (auto& line : lines) {
        if (!line.startsWith("rule\tmin_clearance\t")) continue;
        const auto fields = line.split('\t');
        bool okay = false;
        old_clearance = fields.size() == 3 ? fields[2].toDouble(&okay) : 0;
        require(okay && std::isfinite(old_clearance), "Native clearance rule is not a finite size");
        new_clearance = old_clearance < 999 ? old_clearance + 0.025 : old_clearance - 0.025;
        // Match the native Board Setup serializer's default six-digit precision.
        line = "rule\tmin_clearance\t" + QByteArray::number(new_clearance, 'g', 6);
        ++changed;
    }
    require(changed == 1 && new_clearance != old_clearance, "Expected one actual Board Setup clearance edit");
    QByteArray requested;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (index) requested += '\n';
        requested += lines[index];
    }
    char* project_text = nullptr;
    std::unique_ptr<char, decltype(&cc_free_text)> board_text(
        cc_board_native_apply_board_setup(board.c_str(), requested.constData(), &project_text,
                                         error.data(), error.size()), &cc_free_text);
    std::unique_ptr<char, decltype(&cc_free_text)> project_owner(project_text, &cc_free_text);
    require(bool(board_text) && bool(project_owner), std::string("Apply native Board Setup: ") + error.data());
    const QByteArray serialized_board(board_text.get()), serialized_project(project_owner.get());
    const auto serialized_uuids = pcb_uuids(serialized_board);
    // Retain actual native serializer output even if a preservation check fails.
    write(output / "board-native-candidate.kicad_pcb", serialized_board);
    write(output / "board-native-candidate.kicad_pro", serialized_project);
    QJsonArray original_ids, serialized_ids;
    for (const auto& id : original_uuids) original_ids.append(id);
    for (const auto& id : serialized_uuids) serialized_ids.append(id);
    write(output / "board-native-uuids.json", QJsonDocument(QJsonObject{
        {"original", original_ids}, {"serialized", serialized_ids}}).toJson());
    require(serialized_board.contains("(kicad_pcb"), "Board edit did not serialize a KiCad board");
    // KiCad adds missing mandatory footprint fields (Datasheet/Description).
    // Every original identity must survive, including duplicate multiplicity.
    require(std::includes(serialized_uuids.begin(), serialized_uuids.end(),
                          original_uuids.begin(), original_uuids.end()),
            "Native Board Setup serialization lost original PCB item UUIDs");
    json(serialized_project);
    // The native board is still edited here. Its C ABI deliberately reloads an
    // unsaved edit on the next board call, so interleave FreeCAD before persisting
    // both returned files into an independent complete project copy.
    mechanical(cc_mechanical_recompute(mech, error.data(), error.size()), "mechanical recompute during native PCB edit");
    previews(mech);
    const auto copy = output / "board-copy";
    require(!fs::exists(copy) && fs::create_directory(copy), "Board edit copy must be new");
    auto board_file = project_file;
    board_file.replace_extension(".kicad_pcb");
    require(original.count(board_file) && original.count(project_file), "Original project pair is incomplete");
    for (const auto& [relative, bytes] : original) {
        const auto destination = copy / relative;
        fs::create_directories(destination.parent_path());
        write(destination, relative == board_file ? serialized_board : relative == project_file ? serialized_project : bytes);
    }
    const auto reopened = board_setup(copy / board_file);
    require(reopened == requested, "Board Setup edit did not survive saving/reopening both native project files");
    require(pcb_uuids(read(copy / board_file)) == serialized_uuids, "Reopened PCB changed its saved item UUIDs");
    mechanical(cc_mechanical_recompute(mech, error.data(), error.size()), "mechanical recompute after PCB reopen");
    previews(mech);
    require(board_setup(copy / board_file) == reopened, "Mechanical recompute changed the reopened PCB edit");
    require(pcb_uuids(read(copy / board_file)) == serialized_uuids, "Interleaved recompute changed saved PCB item UUIDs");
    require(board_setup(board) == before, "Original Board Setup changed after edited-copy checks");
    write(output / "board-setup-before.txt", before);
    write(output / "board-setup-after.txt", reopened);
    return {{"rule", "min_clearance"}, {"before_mm", old_clearance}, {"after_mm", new_clearance},
            {"preserved_uuid_count", static_cast<qint64>(original_uuids.size())},
            {"serialized_uuid_count", static_cast<qint64>(serialized_uuids.size())},
            {"copy", "board-copy"}, {"board_file", QString::fromStdString(board_file.generic_string())},
            {"project_file", QString::fromStdString(project_file.generic_string())},
            {"board", file_record(copy / board_file)}, {"project", file_record(copy / project_file)},
            {"setup_before_sha256", hash(before)}, {"setup_after_sha256", hash(reopened)}};
}

int run(int argc, char** argv) {
    require(argc == 6, "Usage: mixed-probe kicad-first|freecad-first RUNTIME SOURCE_PROJECT INPUT_JSON FRESH_OUTPUT");
    const std::string order = argv[1];
    require(order == "kicad-first" || order == "freecad-first", "Unknown C ABI initialization order");
    const fs::path runtime = fs::canonical(argv[2]), source = fs::canonical(argv[3]), output = fs::absolute(argv[5]);
    const auto output_relative = fs::weakly_canonical(output).lexically_relative(source);
    require(!output_relative.empty() && *output_relative.begin() == "..", "Probe output must be outside the source fixture");
    require(!fs::exists(output) && fs::create_directory(output), "Probe output must be new");
    std::map<fs::path, QByteArray> original;
    fs::path project_file;
    for (const auto& entry : fs::recursive_directory_iterator(source)) {
        require(!entry.is_symlink(), "Fixture project must contain no symlinks");
        if (entry.is_regular_file()) {
            const auto relative = entry.path().lexically_relative(source);
            original[relative] = read(entry.path());
            if (entry.path().extension() == ".kicad_pro") {
                require(project_file.empty(), "Expected exactly one project fixture");
                project_file = relative;
            }
        }
    }
    require(!project_file.empty(), "Project fixture needs a .kicad_pro");
    const fs::path project = output / "project";
    fs::copy(source, project, fs::copy_options::recursive);
    fs::path board = project / project_file;
    board.replace_extension(".kicad_pcb");
    require(fs::is_regular_file(board), "Project fixture needs a board");
    auto input = json(read(argv[4]));
    auto provenance = input["source"].toObject();
    require(provenance["name"].toString().toStdString() == board.filename().string(), "Accepted input belongs to another board name");
    provenance["sha256"] = hash(read(board)); // exact fixture bytes, including checkout line endings
    input["source"] = provenance;
    const QByteArray input_text = QJsonDocument(input).toJson(QJsonDocument::Compact);
    write(output / "input.json", input_text);
    const auto document = project / "enclosure.FCStd", user = output / "freecad-user";
    using Sch = std::unique_ptr<cc_sch_session, decltype(&cc_sch_session_free)>;
    using Mech = std::unique_ptr<cc_mechanical_session, decltype(&cc_mechanical_close)>;
    Sch sch(nullptr, &cc_sch_session_free);
    Mech mech(nullptr, &cc_mechanical_close);
    QJsonObject before, initial_previews;
    unsigned capabilities = 0;
    auto start_kicad = [&] {
        sch.reset(cc_sch_session_open((project / project_file).c_str(), &result.value));
        schematic(bool(sch), "initialize native schematic session");
        require(cc_sch_session_backend(sch.get()) && std::strcmp(cc_sch_session_backend(sch.get()), native_backend) == 0,
                "Native KiCad schematic backend not linked");
        require(cc_board_native_backend() && std::strcmp(cc_board_native_backend(), native_backend) == 0,
                "Native KiCad board backend not linked");
        capabilities = cc_sch_session_capabilities(sch.get());
        constexpr unsigned required = CC_SCH_NATIVE_KICAD | CC_SCH_NETLIST | CC_SCH_ERC | CC_SCH_TRANSACTIONS;
        require((capabilities & required) == required, "Native schematic capabilities missing");
        kicad_checks(sch.get(), board, output, "initial");
        before = snapshot(sch.get());
        require(label_count(before) == 0, "Fixture already contains the probe label");
    };
    auto start_freecad = [&] {
        require(cc_mechanical_available() == 1, "Native FreeCAD adapter not linked");
        mechanical(cc_mechanical_initialize(runtime.c_str(), user.c_str(), error.data(), error.size()), "initialize native FreeCAD");
        mech.reset(cc_mechanical_open(document.c_str(), input_text.constData(), defaults, error.data(), error.size()));
        require(bool(mech), std::string("open native mechanical session: ") + error.data());
        settings(mech.get(), defaults.wall);
        initial_previews = previews(mech.get());
    };
    // No QCoreApplication is constructed by this probe before either explicit C ABI path.
    if (order == "kicad-first") { start_kicad(); start_freecad(); }
    else { start_freecad(); start_kicad(); }
    require(snapshot(sch.get()) == before, "Schematic state changed while initializing the other engine");
    settings(mech.get(), defaults.wall);
    require(previews(mech.get()) == initial_previews, "Preview changed while initializing the other engine");
    write(output / "before.json", QJsonDocument(before).toJson());
    const auto sheets = before["sheets"].toArray();
    require(!sheets.isEmpty(), "Expected a root schematic sheet");
    const auto sheet_path = sheets.first().toObject()["path"].toString().toUtf8();
    std::unique_ptr<cc_sch_transaction, decltype(&cc_sch_transaction_free)> transaction(
        cc_sch_transaction_begin(sch.get(), &result.value), &cc_sch_transaction_free);
    schematic(bool(transaction), "begin schematic edit");
    schematic(cc_sch_transaction_add_label(transaction.get(), sheet_path.constData(), label, 25.4, 25.4, &result.value), "add integration label");
    // Keep the KiCad transaction live while another engine recomputes its exact document.
    mechanical(cc_mechanical_set_parameter(mech.get(), "wall", defaults.wall + 0.5, error.data(), error.size()), "interleaved mechanical edit");
    previews(mech.get());
    schematic(cc_sch_transaction_commit(transaction.get(), &result.value), "commit schematic label");
    transaction.reset();
    const auto edited = snapshot(sch.get());
    require(label_count(edited) == 1, "Schematic edit disappeared after mechanical recompute");
    schematic(cc_sch_session_undo(sch.get(), &result.value), "schematic undo");
    mechanical(cc_mechanical_undo(mech.get(), error.data(), error.size()), "mechanical undo");
    require(snapshot(sch.get()) == before, "Schematic undo did not restore its state");
    settings(mech.get(), defaults.wall);
    schematic(cc_sch_session_redo(sch.get(), &result.value), "schematic redo");
    mechanical(cc_mechanical_redo(mech.get(), error.data(), error.size()), "mechanical redo");
    require(snapshot(sch.get()) == edited, "Schematic redo changed across engine calls");
    settings(mech.get(), defaults.wall + 0.5);
    require(!cc_mechanical_set_parameter(mech.get(), "wall", -1, error.data(), error.size()) && error[0], "Invalid wall accepted");
    settings(mech.get(), defaults.wall + 0.5);
    const auto board_edit = edit_board(mech.get(), board, project_file, original, output);
    require(snapshot(sch.get()) == edited, "PCB edit/copy checks changed the live schematic");
    settings(mech.get(), defaults.wall + 0.5);
    kicad_checks(sch.get(), board, output, "interleaved");
    mechanical(cc_mechanical_recompute(mech.get(), error.data(), error.size()), "mechanical recompute after KiCad checks");
    const auto counts = previews(mech.get());
    mechanical(cc_mechanical_save(mech.get(), error.data(), error.size()), "save mechanical document");
    const auto saved_bytes = read(document);
    const auto copy = output / "schematic-copy";
    schematic(cc_sch_session_save_copy(sch.get(), copy.c_str(), &result.value), "save edited schematic copy");
    Sch copy_session(cc_sch_session_open((copy / project_file).c_str(), &result.value), &cc_sch_session_free);
    schematic(bool(copy_session), "reopen serialized schematic copy");
    const auto reopened = snapshot(copy_session.get());
    require(label_count(reopened) == 1, "Edited label did not survive native serialization");
    const auto saved_sheets = reopened["sheets"].toArray();
    require(saved_sheets.size() == sheets.size(), "Saved schematic lost sheets");
    for (qsizetype index = 0; index < saved_sheets.size(); ++index)
        for (const char* key : {"uuid","symbols","labels","wires","junctions","noConnects","buses"})
            require(saved_sheets[index].toObject()[key] == edited["sheets"].toArray()[index].toObject()[key],
                    std::string("Saved schematic changed supported content: ") + key);
    write(output / "saved-copy.json", QJsonDocument(reopened).toJson());
    require(snapshot(sch.get()) == edited, "Opening saved copy altered the original live schematic");
    mech.reset();
    mech.reset(cc_mechanical_open(document.c_str(), input_text.constData(), defaults, error.data(), error.size()));
    require(bool(mech), std::string("reopen mechanical document: ") + error.data());
    settings(mech.get(), defaults.wall + 0.5);
    require(read(document) == saved_bytes, "Mechanical reopen changed FCStd bytes");
    QJsonObject exports{{"enclosure.FCStd", file_record(document)}};
    for (int part : {0, 1}) {
        const std::string name = part ? "lid" : "tray";
        const auto step = output / (name + ".step"), stl = output / (name + ".stl");
        mechanical(cc_mechanical_export_step(mech.get(), part, step.c_str(), error.data(), error.size()), "exact STEP export");
        mechanical(cc_mechanical_export_stl(mech.get(), part, stl.c_str(), error.data(), error.size()), "native STL export");
        require(fs::file_size(step) && fs::file_size(stl), "Missing actual mechanical export");
        exports[QString::fromStdString(name + ".step")] = file_record(step);
        exports[QString::fromStdString(name + ".stl")] = file_record(stl);
    }
    settings(mech.get(), defaults.wall + 0.5);
    require(snapshot(sch.get()) == edited, "Mechanical exports changed the live schematic");
    kicad_checks(sch.get(), board, output, "after-copy-and-exports");
    settings(mech.get(), defaults.wall + 0.5);
    copy_session.reset();
    sch.reset();
    mech.reset();
    QJsonObject inputs;
    for (const auto& [relative, bytes] : original) {
        require(read(source / relative) == bytes && read(project / relative) == bytes, "Original KiCad input bytes were modified");
        inputs[QString::fromStdString(relative.generic_string())] = file_record(source / relative);
    }
    QJsonObject report{{"schema",1}, {"passed",true}, {"order",QString::fromStdString(order)},
        {"scope","Combined native C ABI smoke test; no app or physical-device acceptance"},
        {"schematic_backend",native_backend}, {"board_backend",native_backend}, {"capabilities",static_cast<int>(capabilities)},
        {"source_inputs",inputs}, {"input_json_sha256",hash(input_text)}, {"triangle_counts",counts}, {"exports",exports},
        {"board_edit",board_edit},
        {"operations",QJsonObject{{"both_native_engines",true},{"interleaved_transactions",true},{"undo_redo",true},
            {"rejected_mechanical_edit",true},{"netlist_erc_drc_refill",true},{"schematic_save_copy_reopen",true},
            {"pcb_board_setup_edit_reopen",true},
            {"mechanical_save_reopen",true},{"source_bytes_preserved",true},{"step_stl",true}}}};
    write(output / "mixed-report.json", QJsonDocument(report).toJson());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& exception) { std::cerr << exception.what() << '\n'; return 1; }
}

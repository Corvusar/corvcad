// In-process native C ABI regression executable. MIT.
// argv: read-only runtime root, fresh writable user root, accepted F1 fixture.
#include <Python.h>
#include <QByteArray>
#include <QCryptographicHash>
#include "corvcad/mechanical_session_c_api.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
std::array<char, 2048> error{};
constexpr cc_settings defaults{3.0, 2.0, 2.0, 2.0, 4.0, 6.0, 2.0, 0.25};

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void success(int result, const char* operation)
{
    if (!result) throw std::runtime_error(std::string(operation) + ": " + error.data());
    require(error[0] == '\0', "Successful API call retained a stale error");
}

void rejected(int result, const char* operation)
{
    if (result || error[0] == '\0') throw std::runtime_error(std::string("Expected rejected API call: ") + operation);
}

std::string utf8(const fs::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + utf8(path));
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) throw std::runtime_error("Cannot write " + utf8(path));
}

std::string quote(const std::string& text)
{
    std::ostringstream result;
    result << '"';
    for (unsigned char character : text) {
        if (character == '"' || character == '\\') result << '\\' << character;
        else if (character < 0x20) result << "\\u00" << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(character);
        else result << character;
    }
    return result.str() + '"';
}

std::string sha256(const fs::path& path)
{
    const std::string data = read(path);
    const QByteArray digest = QCryptographicHash::hash(QByteArray(data.data(), static_cast<qsizetype>(data.size())), QCryptographicHash::Sha256).toHex();
    return {digest.constData(), static_cast<size_t>(digest.size())};
}

cc_settings settings(cc_mechanical_session* session)
{
    cc_settings result{};
    success(cc_mechanical_get_settings(session, &result, error.data(), error.size()), "Read settings");
    return result;
}

void same_settings(cc_settings actual, cc_settings expected)
{
    const std::array<double, 8> a{actual.margin, actual.wall, actual.floor, actual.board_clearance, actual.standoff_height,
                                 actual.standoff_diameter, actual.lid_thickness, actual.lid_gap};
    const std::array<double, 8> b{expected.margin, expected.wall, expected.floor, expected.board_clearance, expected.standoff_height,
                                 expected.standoff_diameter, expected.lid_thickness, expected.lid_gap};
    for (size_t index = 0; index < a.size(); ++index)
        require(std::isfinite(a[index]) && std::abs(a[index] - b[index]) < 1e-9, "Unexpected enclosure setting");
}

struct Mesh {
    size_t count = 0;
    long double signed_volume = 0;
};

Mesh preview(cc_mechanical_session* session, int part, bool buffers)
{
    const size_t count = cc_mechanical_triangles(session, part, nullptr, 0, error.data(), error.size());
    require(count > 2 && error[0] == '\0', "Preview count query failed");
    if (buffers) {
        std::array<cc_triangle, 2> untouched{};
        std::memset(untouched.data(), 0xa5, sizeof(untouched));
        const auto original = untouched;
        require(cc_mechanical_triangles(session, part, untouched.data(), 1, error.data(), error.size()) == count,
                "Short preview buffer did not return its required size");
        require(error[0] == '\0' && std::memcmp(original.data(), untouched.data(), sizeof(untouched)) == 0,
                "Short preview buffer was modified");
        require(cc_mechanical_triangles(session, part, untouched.data(), 0, error.data(), error.size()) == count,
                "Zero capacity preview did not return its required size");
        require(std::memcmp(original.data(), untouched.data(), sizeof(untouched)) == 0, "Zero capacity preview buffer was modified");
        rejected(static_cast<int>(cc_mechanical_triangles(session, part, nullptr, 1, error.data(), error.size())), "null mesh storage");
        rejected(static_cast<int>(cc_mechanical_triangles(session, 2, untouched.data(), 2, error.data(), error.size())), "invalid mechanical part");
        require(std::memcmp(original.data(), untouched.data(), sizeof(untouched)) == 0, "Invalid preview request modified its buffer");
    }
    std::vector<cc_triangle> triangles(count);
    require(cc_mechanical_triangles(session, part, triangles.data(), triangles.size(), error.data(), error.size()) == count,
            "Full preview copy failed");
    require(error[0] == '\0', "Full preview retained an error");
    Mesh result{count, 0};
    for (const auto& triangle : triangles) {
        for (float value : {triangle.ax, triangle.ay, triangle.az, triangle.bx, triangle.by, triangle.bz,
                           triangle.cx, triangle.cy, triangle.cz}) require(std::isfinite(value), "Nonfinite C ABI preview coordinate");
        const long double ax = triangle.ax, ay = triangle.ay, az = triangle.az;
        const long double bx = triangle.bx, by = triangle.by, bz = triangle.bz;
        const long double cx = triangle.cx, cy = triangle.cy, cz = triangle.cz;
        result.signed_volume += (ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) + az * (bx * cy - by * cx)) / 6;
    }
    require(std::isfinite(static_cast<double>(result.signed_volume)) && result.signed_volume > 0,
            "Preview must enclose a positive oriented volume");
    return result;
}

class PyObjectOwner {
public:
    explicit PyObjectOwner(PyObject* object) : object_(object) { if (!object) throw std::runtime_error("Cannot inspect native runtime metadata"); }
    ~PyObjectOwner() { Py_DECREF(object_); }
    PyObject* get() const { return object_; }
    PyObjectOwner(const PyObjectOwner&) = delete;
    PyObjectOwner& operator=(const PyObjectOwner&) = delete;
private:
    PyObject* object_;
};

std::string metadata(const fs::path& runtime)
{
    const PyGILState_STATE gil = PyGILState_Ensure();
    try {
        std::string result;
        {
            PyObjectOwner dictionary(PyDict_New());
            PyObjectOwner sys(PyImport_ImportModule("sys"));
            PyObjectOwner platform(PyObject_GetAttrString(sys.get(), "platform"));
            PyObjectOwner freecad(PyImport_ImportModule("FreeCAD"));
            PyObjectOwner version(PyObject_CallMethod(freecad.get(), "Version", nullptr));
            PyObjectOwner home(PyObject_CallMethod(freecad.get(), "getHomePath", nullptr));
            PyObjectOwner resources(PyObject_CallMethod(freecad.get(), "getResourceDir", nullptr));
            PyObjectOwner library(PyObject_CallMethod(freecad.get(), "getLibraryDir", nullptr));
            const char* home_text = PyUnicode_AsUTF8(home.get());
            const char* resources_text = PyUnicode_AsUTF8(resources.get());
            const char* library_text = PyUnicode_AsUTF8(library.get());
            require(home_text && resources_text && library_text, "Cannot inspect FreeCAD directory roots");
            require(fs::canonical(fs::u8path(home_text)) == fs::canonical(runtime), "FreeCAD home follows the probe executable instead of the bundled runtime");
            require(fs::canonical(fs::u8path(resources_text)) == fs::canonical(runtime / "data"), "FreeCAD resource root differs from the bundle");
            require(fs::canonical(fs::u8path(library_text)) == fs::canonical(runtime / "lib"), "FreeCAD library root differs from the bundle");
            PyObjectOwner partdesign(PyImport_ImportModule("PartDesign"));
            PyObjectOwner initializer(PyObject_GetAttrString(partdesign.get(), "__file__"));
            const char* initializer_text = PyUnicode_AsUTF8(initializer.get());
            require(initializer_text && fs::canonical(fs::u8path(initializer_text)) == fs::canonical(runtime / "Mod/PartDesign/__init__.py"),
                    "PartDesign helper was not loaded from the bundled runtime");
            PyObjectOwner origins(PyDict_New());
            require(PyDict_SetItemString(dictionary.get(), "platform", platform.get()) == 0, "Metadata platform failed");
            require(PyDict_SetItemString(dictionary.get(), "freecad_version", version.get()) == 0, "Metadata version failed");
            require(PyDict_SetItemString(dictionary.get(), "home_path", home.get()) == 0, "Metadata home failed");
            require(PyDict_SetItemString(dictionary.get(), "resource_dir", resources.get()) == 0, "Metadata resources failed");
            require(PyDict_SetItemString(dictionary.get(), "library_dir", library.get()) == 0, "Metadata library failed");
            require(PyDict_SetItemString(dictionary.get(), "partdesign_helper", initializer.get()) == 0, "Metadata PartDesign failed");
            for (const char* name : {"FreeCAD", "__FreeCADBase__", "Materials", "Part", "Mesh", "Sketcher", "_PartDesign", "Spreadsheet", "Import"}) {
                PyObjectOwner module(PyImport_ImportModule(name));
                PyObjectOwner spec(PyObject_GetAttrString(module.get(), "__spec__"));
                PyObjectOwner origin(PyObject_GetAttrString(spec.get(), "origin"));
                require(PyDict_SetItemString(origins.get(), name, origin.get()) == 0, "Metadata origins failed");
            }
            require(PyDict_SetItemString(dictionary.get(), "builtin_origins", origins.get()) == 0, "Metadata builtins failed");
            PyObjectOwner json(PyImport_ImportModule("json"));
            PyObjectOwner encoded(PyObject_CallMethod(json.get(), "dumps", "O", dictionary.get()));
            const char* text = PyUnicode_AsUTF8(encoded.get());
            require(text, "Cannot serialize runtime metadata");
            result = text;
        }
        PyGILState_Release(gil);
        return result;
    }
    catch (...) { PyErr_Clear(); PyGILState_Release(gil); throw; }
}

int probe(const fs::path& runtime, const fs::path& user, const fs::path& fixture)
{
    require(!fs::exists(user / "f4-session-report.json"), "Refusing to reuse a native session report");
    const auto runtime_text = utf8(runtime), user_text = utf8(user);
    require(cc_mechanical_available() == 1, "Native mechanical edition is not linked");
    cc_settings untouched{};
    rejected(cc_mechanical_get_settings(nullptr, &untouched, error.data(), error.size()), "uninitialized runtime");
    rejected(cc_mechanical_initialize(nullptr, user_text.c_str(), error.data(), error.size()), "null runtime root");
    rejected(cc_mechanical_initialize(runtime_text.c_str(), nullptr, error.data(), error.size()), "null user root");
    success(cc_mechanical_initialize(runtime_text.c_str(), user_text.c_str(), error.data(), error.size()), "Initialize native runtime");
    success(cc_mechanical_initialize(runtime_text.c_str(), user_text.c_str(), error.data(), error.size()), "Repeat native initialization");
    const auto other_user = utf8(user / "other-root");
    rejected(cc_mechanical_initialize(runtime_text.c_str(), other_user.c_str(), error.data(), error.size()), "different runtime roots");
    const auto document = utf8(user / "enclosure.FCStd");
    const std::string input = read(fixture);
    require(cc_mechanical_open(nullptr, input.c_str(), defaults, error.data(), error.size()) == nullptr && error[0], "Null document path accepted");
    require(cc_mechanical_open(document.c_str(), nullptr, defaults, error.data(), error.size()) == nullptr && error[0], "Null board input accepted");
    require(cc_mechanical_open(document.c_str(), "{invalid JSON}", defaults, error.data(), error.size()) == nullptr && error[0], "Malformed board input accepted");
    auto invalid = defaults;
    invalid.wall = std::numeric_limits<double>::quiet_NaN();
    require(cc_mechanical_open(document.c_str(), input.c_str(), invalid, error.data(), error.size()) == nullptr && error[0], "Nonfinite settings accepted");
    cc_mechanical_session* session = cc_mechanical_open(document.c_str(), input.c_str(), defaults, error.data(), error.size());
    require(session && !error[0], "Cannot create native mechanical session");
    same_settings(settings(session), defaults);
    require(!fs::exists(fs::u8path(document)), "Opening an unsaved model wrote a document");
    require(!cc_mechanical_can_undo(session) && !cc_mechanical_can_redo(session), "New model has unexpected history");
    rejected(cc_mechanical_get_settings(session, nullptr, error.data(), error.size()), "null settings output");
    auto* foreign = reinterpret_cast<cc_mechanical_session*>(std::uintptr_t{1});
    rejected(cc_mechanical_recompute(foreign, error.data(), error.size()), "foreign session handle");
    require(!cc_mechanical_can_undo(foreign) && !cc_mechanical_can_redo(nullptr), "Invalid history handle accepted");
    rejected(cc_mechanical_set_parameter(session, "unknown", 1, error.data(), error.size()), "unknown parameter");
    rejected(cc_mechanical_set_parameter(session, "wall", std::numeric_limits<double>::infinity(), error.data(), error.size()), "nonfinite parameter");
    rejected(cc_mechanical_set_parameter(session, "wall", -1, error.data(), error.size()), "invalid parameter rollback");
    same_settings(settings(session), defaults);
    require(!cc_mechanical_can_undo(session), "Rejected change polluted native undo history");
    char tiny[1] = {'x'};
    require(!cc_mechanical_set_parameter(session, "unknown", 1, tiny, sizeof(tiny)) && tiny[0] == '\0', "Tiny error buffer was not terminated");
    require(!cc_mechanical_recompute(session, nullptr, 1), "Null error storage with nonzero capacity accepted");
    success(cc_mechanical_recompute(session, error.data(), error.size()), "Recompute native document");
    const Mesh baseline = preview(session, 0, true);
    cc_settings edited = defaults;
    edited.wall += 0.5;
    success(cc_mechanical_set_parameter(session, "wall", edited.wall, error.data(), error.size()), "Edit native parameter");
    same_settings(settings(session), edited);
    require(cc_mechanical_can_undo(session) && !cc_mechanical_can_redo(session), "Native undo step was not created");
    const Mesh changed = preview(session, 0, false);
    require(changed.signed_volume > baseline.signed_volume, "Parameter change did not recompute exact preview geometry");
    success(cc_mechanical_undo(session, error.data(), error.size()), "Undo parameter change");
    same_settings(settings(session), defaults);
    require(cc_mechanical_can_redo(session), "Native redo step missing");
    success(cc_mechanical_redo(session, error.data(), error.size()), "Redo parameter change");
    same_settings(settings(session), edited);
    char* features = cc_mechanical_call_json(session, "features", nullptr, error.data(), error.size());
    require(features && std::string(features).find("\"name\": \"OuterPad\"") != std::string::npos,
            "Feature list misses the tray's outer pad");
    std::free(features);
    require(!cc_mechanical_call_json(session, "save", nullptr, error.data(), error.size()) && error[0] != '\0',
            "A method outside the JSON whitelist ran");
    success(cc_mechanical_save(session, error.data(), error.size()), "Save exact native document");
    const std::string persisted = read(fs::u8path(document));
    require(!persisted.empty(), "Native save is empty");
    write(fs::u8path(document), "external document modification");
    rejected(cc_mechanical_save(session, error.data(), error.size()), "save external modification guard");
    require(read(fs::u8path(document)) == "external document modification", "Native save overwrote external data");
    write(fs::u8path(document), persisted); // Restore only this test-owned fixture.
    success(cc_mechanical_save(session, error.data(), error.size()), "Save after exact document restoration");
    cc_mechanical_close(session);
    rejected(cc_mechanical_get_settings(session, &untouched, error.data(), error.size()), "closed session");
    cc_mechanical_close(session);
    cc_mechanical_close(nullptr);
    cc_settings ignored = defaults;
    ignored.wall += 2;
    auto* reopened = cc_mechanical_open(document.c_str(), input.c_str(), ignored, error.data(), error.size());
    require(reopened && reopened != session && !error[0], "Cannot reopen persisted native document");
    same_settings(settings(reopened), edited);
    rejected(cc_mechanical_get_settings(session, &untouched, error.data(), error.size()), "stale handle after new session");
    require(cc_mechanical_open(document.c_str(), input.c_str(), defaults, error.data(), error.size()) == nullptr && error[0], "Duplicate document ownership accepted");
    const auto second_path = utf8(user / "secondary.FCStd");
    auto* second = cc_mechanical_open(second_path.c_str(), input.c_str(), defaults, error.data(), error.size());
    require(second && !error[0], "Cannot open a second independent native session");
    success(cc_mechanical_set_parameter(second, "floor", defaults.floor + 0.5, error.data(), error.size()), "Edit second document");
    same_settings(settings(reopened), edited);
    cc_mechanical_close(second);
    const auto unknown_path = utf8(user / "unknown.step");
    write(fs::u8path(unknown_path), "unknown user export");
    rejected(cc_mechanical_export_step(reopened, 0, unknown_path.c_str(), error.data(), error.size()), "unknown export overwrite guard");
    require(read(fs::u8path(unknown_path)) == "unknown user export", "Unknown export was overwritten");
    const auto owned_path = utf8(user / "owned.step");
    success(cc_mechanical_export_step(reopened, 0, owned_path.c_str(), error.data(), error.size()), "Create owned STEP export");
    write(fs::u8path(owned_path), "externally changed export");
    rejected(cc_mechanical_export_step(reopened, 0, owned_path.c_str(), error.data(), error.size()), "external export modification guard");
    require(read(fs::u8path(owned_path)) == "externally changed export", "External export modification was overwritten");
    rejected(cc_mechanical_export_step(reopened, -1, owned_path.c_str(), error.data(), error.size()), "invalid export part");
    rejected(cc_mechanical_export_stl(reopened, 0, nullptr, error.data(), error.size()), "null export path");
    const Mesh tray = preview(reopened, 0, false), lid = preview(reopened, 1, true);
    for (int part : {0, 1}) {
        const std::string stem = part == 0 ? "tray" : "lid";
        const auto step = utf8(user / (stem + ".step")), stl = utf8(user / (stem + ".stl"));
        success(cc_mechanical_export_step(reopened, part, step.c_str(), error.data(), error.size()), "Export exact native STEP part");
        success(cc_mechanical_export_stl(reopened, part, stl.c_str(), error.data(), error.size()), "Export native STL part");
    }
    cc_mechanical_close(reopened);
    std::ostringstream report;
    report << std::setprecision(17) << "{\n  \"schema\":1,\"passed\":true,\"native_runtime\":" << metadata(runtime)
           << ",\n  \"operations\":{\"initialize\":true,\"create\":true,\"invalid_change_rollback\":true,"
              "\"recompute\":true,\"undo\":true,\"redo\":true,\"save\":true,\"reopen\":true,"
              "\"multiple_sessions\":true,\"duplicate_path_guard\":true,\"stale_handle_guard\":true,"
              "\"triangle_short_buffer\":true,\"null_and_nonfinite_guards\":true,\"save_external_change_guard\":true,"
              "\"export_unknown_overwrite_guard\":true,\"export_external_change_guard\":true,\"step\":true,\"stl\":true},"
           << "\n  \"triangle_counts\":{\"tray\":" << tray.count << ",\"lid\":" << lid.count << "},"
           << "\n  \"preview_signed_volume_mm3\":{\"tray\":" << static_cast<double>(tray.signed_volume)
           << ",\"lid\":" << static_cast<double>(lid.signed_volume) << "},\n  \"files\":{";
    bool separator = false;
    for (const char* name : {"enclosure.FCStd", "tray.step", "lid.step", "tray.stl", "lid.stl"}) {
        const auto path = user / name;
        require(fs::is_regular_file(path) && fs::file_size(path) > 0, "Missing native document/export");
        if (separator) report << ',';
        separator = true;
        report << quote(name) << ":{\"bytes\":" << fs::file_size(path) << ",\"sha256\":" << quote(sha256(path)) << '}';
    }
    report << "}\n}\n";
    write(user / "f4-session-report.json", report.str());
    std::cout << "Native mechanical C ABI regression passed: " << utf8(user) << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 4) throw std::runtime_error("Usage: mechanical-session-test RUNTIME_ROOT USER_ROOT FIXTURE_JSON");
        return probe(fs::absolute(fs::u8path(argv[1])), fs::absolute(fs::u8path(argv[2])), fs::absolute(fs::u8path(argv[3])));
    }
    catch (const std::exception& exception) { std::cerr << "Native C ABI regression failed: " << exception.what() << '\n'; }
    catch (...) { std::cerr << "Native C ABI regression failed with an unknown exception\n"; }
    return 1;
}

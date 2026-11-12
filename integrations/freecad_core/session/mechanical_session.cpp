// CorvCAD native mechanical session boundary. MIT.
// FreeCAD/Python/Qt remain private, separately licensed implementation inputs.
#include <Python.h>
#include <QByteArray>
#include <QCoreApplication>
#include <QtGlobal>
#include <FCConfig.h>
#include <App/Application.h>
#include <App/ApplicationDirectories.h>
#include <Base/Exception.h>
#include <Base/Factory.h>
#include <Base/Interpreter.h>
#include "corvcad/mechanical_session_c_api.h"

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

extern bool corvcad_register_freecad_modules();

struct cc_mechanical_session { PyObject* object = nullptr; };

namespace {
namespace fs = std::filesystem;
constexpr std::array<const char*, 8> parameter_names = {
    "margin", "wall", "floor", "board_clearance", "standoff_height",
    "standoff_diameter", "lid_thickness", "lid_gap"};
constexpr const char* freecad_pin = "4fd3bf320d9566a27e60069fc8387448aaa3a094";

class Object {
public:
    explicit Object(PyObject* value = nullptr) : value_(value) {}
    ~Object() { Py_XDECREF(value_); }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&& other) noexcept : value_(other.release()) {}
    PyObject* get() const { return value_; }
    PyObject* release() { auto* value = value_; value_ = nullptr; return value; }
private:
    PyObject* value_;
};

class GIL {
public:
    GIL() : state_(PyGILState_Ensure()) {}
    ~GIL() { PyGILState_Release(state_); }
    GIL(const GIL&) = delete;
    GIL& operator=(const GIL&) = delete;
private:
    PyGILState_STATE state_;
};

enum class Phase { uninitialized, initializing, ready, failed };
struct Runtime {
    std::mutex mutex;
    Phase phase = Phase::uninitialized;
    fs::path root, user;
    PyObject* session_class = nullptr;
    // App::Application retains argc/argv, and QCoreApplication can modify its
    // arguments. Both sets of backing storage therefore last for the process.
    std::vector<std::string> arguments;
    std::vector<char*> argv;
    int qt_argc = 1;
    std::array<char*, 2> qt_argv{};
    QCoreApplication* qt = nullptr;
    // Closed handles remain as tiny tombstones. Their addresses cannot be
    // reused for a different document; stale/foreign handles are never read.
    std::list<cc_mechanical_session> handles;
};

Runtime& runtime()
{
    // Do not let process-exit C++ destruction finalize Python, Qt or FreeCAD.
    static Runtime* const instance = new Runtime;
    return *instance;
}

void error_text(char* output, size_t size, const char* message) noexcept
{
    if (!output || !size) return;
    const size_t length = std::strlen(message);
    size_t count = std::min(size - 1, length);
    // Preserve a complete UTF-8 prefix when a native error buffer is short.
    if (count < length) {
        while (count && (static_cast<unsigned char>(message[count]) & 0xc0) == 0x80) --count;
    }
    std::memcpy(output, message, count);
    output[count] = '\0';
}

template<class Result, class Operation>
Result boundary(char* error, size_t size, Result failure, Operation operation)
{
    try {
        if (!error && size) throw std::invalid_argument("Null error buffer with nonzero size");
        error_text(error, size, "");
        Runtime& state = runtime();
        std::lock_guard<std::mutex> lock(state.mutex);
        return operation(state);
    }
    catch (const Base::Exception& exception) { error_text(error, size, exception.what()); }
    catch (const std::exception& exception) { error_text(error, size, exception.what()); }
    catch (...) { error_text(error, size, "Unknown native mechanical session failure"); }
    return failure;
}

[[noreturn]] void python_error(const char* operation)
{
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    Object owned_type(type), owned_value(value), owned_traceback(traceback);
    Object description(value ? PyObject_Str(value) : nullptr);
    const char* text = description.get() ? PyUnicode_AsUTF8(description.get()) : nullptr;
    std::string detail = text ? text : "Python operation failed";
    PyErr_Clear();
    throw std::runtime_error(std::string(operation) + ": " + detail);
}

PyObject* checked(PyObject* value, const char* operation)
{
    if (!value) python_error(operation);
    return value;
}

Object no_args(PyObject* object, const char* method)
{
    Object callable(checked(PyObject_GetAttrString(object, method), method));
    return Object(checked(PyObject_CallNoArgs(callable.get()), method));
}

void require_none(PyObject* result)
{
    if (result != Py_None) throw std::runtime_error("Unexpected mechanical backend result");
}

std::string unicode(PyObject* value)
{
    if (!PyUnicode_Check(value)) throw std::runtime_error("Expected UTF-8 backend text");
    const char* text = PyUnicode_AsUTF8(value);
    if (!text) python_error("Read backend text");
    return text;
}

double number(PyObject* value)
{
    if (!value || PyBool_Check(value) || !(PyFloat_Check(value) || PyLong_Check(value)))
        throw std::runtime_error("Expected a numeric mechanical backend value");
    const double result = PyFloat_AsDouble(value);
    if (PyErr_Occurred()) python_error("Read backend number");
    if (!std::isfinite(result)) throw std::runtime_error("Nonfinite mechanical backend value");
    return result;
}

void ready(const Runtime& state)
{
    if (state.phase != Phase::ready) throw std::runtime_error("Mechanical runtime is not initialized");
}

cc_mechanical_session& session(Runtime& state, cc_mechanical_session* handle)
{
    ready(state);
    if (handle) {
        for (auto& candidate : state.handles) {
            if (&candidate == handle && candidate.object) return candidate;
        }
    }
    throw std::invalid_argument("Null, closed or foreign mechanical session");
}

const char* text_argument(const char* value, const char* name)
{
    if (!value || !*value) throw std::invalid_argument(std::string("Missing ") + name);
    return value;
}

void part_argument(int part)
{
    if (part != 0 && part != 1) throw std::invalid_argument("Mechanical part must be 0 (tray) or 1 (lid)");
}

std::array<double, 8> values(cc_settings settings)
{
    return {settings.margin, settings.wall, settings.floor, settings.board_clearance,
            settings.standoff_height, settings.standoff_diameter, settings.lid_thickness, settings.lid_gap};
}

Object settings_object(cc_settings settings)
{
    Object result(checked(PyDict_New(), "Create enclosure settings"));
    const auto dimensions = values(settings);
    for (size_t index = 0; index < dimensions.size(); ++index) {
        if (!std::isfinite(dimensions[index])) throw std::invalid_argument("Enclosure settings must be finite");
        Object value(checked(PyFloat_FromDouble(dimensions[index]), "Create enclosure dimension"));
        if (PyDict_SetItemString(result.get(), parameter_names[index], value.get()) < 0)
            python_error("Create enclosure settings");
    }
    return result;
}

bool within(const fs::path& candidate, const fs::path& parent)
{
    auto entry = candidate.begin();
    for (auto component = parent.begin(); component != parent.end(); ++component, ++entry) {
        if (entry == candidate.end() || *entry != *component) return false;
    }
    return true;
}

fs::path absolute_path(const char* path, const char* name)
{
    fs::path result = fs::u8path(text_argument(path, name));
    if (!result.is_absolute()) throw std::invalid_argument(std::string(name) + " must be absolute");
    return result;
}

std::string utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void environment(const char* name, const fs::path& value)
{
    const std::string text = utf8(value);
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Runtime path exceeds native environment capacity");
    if (!qputenv(name, QByteArray(text.data(), static_cast<int>(text.size()))))
        throw std::runtime_error(std::string("Could not configure ") + name);
}

void validate_runtime(const fs::path& root)
{
    for (const char* name : {"lib/python3.13/encodings/__init__.py", "harness/session_runtime.py",
                              "harness/model.py", "harness/inputs.py", "harness/checks.py",
                              "Mod/PartDesign/__init__.py", "Ext/freecad/__init__.py"}) {
        const fs::path candidate = root / name;
        if (!fs::is_regular_file(candidate) || !within(fs::canonical(candidate), root))
            throw std::invalid_argument(std::string("Missing or external bundled mechanical resource: ") + name);
    }
}

void configure_bundled_directories(const fs::path& root)
{
    // FreeCAD 1.1.4 caches ApplicationDirectories before processing --set-config.
    // A CLI located under runtime/bin gets the right root by coincidence. An
    // embedded app/probe runs elsewhere, so the option changes Config() without
    // changing getHomePath()/getResourceDir() or the module bootstrap's paths.
    // Refresh the existing directory value through its public constructor,
    // before importing native workbenches or creating any model documents.
    if (fs::weakly_canonical(fs::u8path(App::Application::getHomePath())) == root &&
            fs::weakly_canonical(fs::u8path(App::Application::getResourceDir())) == root / "data" &&
            fs::weakly_canonical(fs::u8path(App::Application::getLibraryDir())) == root / "lib") return;
    auto& config = App::Application::Config();
    config["AppHomePath"] = utf8(root) + "/";
    const auto& directories = App::Application::directories();
    if (!directories) throw std::runtime_error("FreeCAD did not create its application directories");
    *directories = App::ApplicationDirectories(config);
    // Remove paths assembled against the executable's unrelated directory.
    // The exact registered upstream script restores Mod/Ext and selected module
    // paths; it also runs their Init.py files. Merely inserting Mod here would
    // hide the underlying resource-root and initialization error.
    Object search(checked(PyList_New(0), "Create bundled bootstrap search path"));
    Object standard(checked(PyUnicode_DecodeFSDefault(utf8(root / "lib/python3.13").c_str()), "Create bundled standard-library path"));
    if (PyList_Append(search.get(), standard.get()) < 0 || PySys_SetObject("path", search.get()) < 0)
        python_error("Configure bundled bootstrap search path");
    const char* script = Base::ScriptFactory().ProduceScript("FreeCADInit");
    if (!script || !*script) throw std::runtime_error("FreeCAD did not register its module initialization script");
    Base::Interpreter().runString(script);
    if (fs::canonical(fs::u8path(App::Application::getHomePath())) != root ||
            fs::canonical(fs::u8path(App::Application::getResourceDir())) != root / "data" ||
            fs::canonical(fs::u8path(App::Application::getLibraryDir())) != root / "lib")
        throw std::runtime_error("FreeCAD application directories do not match the bundled runtime");
}

void validate_interpreter()
{
    const char* version = Py_GetVersion();
    if (std::strncmp(version, "3.13.7 ", 7) != 0)
        throw std::runtime_error("Mechanical runtime requires the reviewed CPython 3.13.7");
    Object sys(checked(PyImport_ImportModule("sys"), "Import sys"));
    Object flags(checked(PyObject_GetAttrString(sys.get(), "flags"), "Read Python flags"));
    for (const char* name : {"isolated", "ignore_environment", "no_site", "dont_write_bytecode", "safe_path"}) {
        Object value(checked(PyObject_GetAttrString(flags.get(), name), "Read isolation flag"));
        if (PyObject_IsTrue(value.get()) != 1) throw std::runtime_error("Mechanical Python must use the reviewed isolated configuration");
    }
    Object modules(checked(PyObject_GetAttrString(sys.get(), "modules"), "Read imported modules"));
    if (PyDict_GetItemString(modules.get(), "FreeCADGui")) throw std::runtime_error("GUI module in headless mechanical runtime");
    for (const char* name : {"FreeCAD", "__FreeCADBase__", "Materials", "Part", "Mesh", "Sketcher",
                              "_PartDesign", "Spreadsheet", "Import"}) {
        Object module(checked(PyImport_ImportModule(name), "Import built-in FreeCAD module"));
        Object spec(checked(PyObject_GetAttrString(module.get(), "__spec__"), "Read built-in module spec"));
        Object origin(checked(PyObject_GetAttrString(spec.get(), "origin"), "Read built-in module origin"));
        if (unicode(origin.get()) != "built-in") throw std::runtime_error("FreeCAD module did not resolve from the static inittab");
    }
    Object application(checked(PyImport_ImportModule("FreeCAD"), "Import FreeCAD"));
    Object version_list = no_args(application.get(), "Version");
    Object version_sequence(checked(PySequence_Fast(version_list.get(), "Invalid FreeCAD version"), "Read FreeCAD version"));
    const auto count = PySequence_Fast_GET_SIZE(version_sequence.get());
    if (count < 4 || unicode(PySequence_Fast_GET_ITEM(version_sequence.get(), 0)) != "1" ||
            unicode(PySequence_Fast_GET_ITEM(version_sequence.get(), 1)) != "1" ||
            unicode(PySequence_Fast_GET_ITEM(version_sequence.get(), 2)) != "4" ||
            unicode(PySequence_Fast_GET_ITEM(version_sequence.get(), count - 1)) != freecad_pin)
        throw std::runtime_error("Mechanical FreeCAD source pin differs from the reviewed version");
}

int initialize(Runtime& state, const char* runtime_root, const char* user_root)
{
    const fs::path root = fs::canonical(absolute_path(runtime_root, "Runtime root"));
    const fs::path user_input = absolute_path(user_root, "User root");
    const fs::path user = fs::weakly_canonical(user_input);
    if (!fs::is_directory(root) || within(user, root) || within(root, user))
        throw std::invalid_argument("Bundled runtime and writable user roots must be separate directories");
    if (state.phase == Phase::ready) {
        if (state.root != root || state.user != user) throw std::runtime_error("Mechanical runtime is already initialized with other roots");
        return 1;
    }
    if (state.phase != Phase::uninitialized) throw std::runtime_error("Mechanical initialization previously failed; restart the process");
    validate_runtime(root);
    if (Py_IsInitialized()) throw std::runtime_error("Mechanical initialization must precede any other Python initialization");
    fs::create_directories(user);
    for (const char* name : {"home", "data", "temp"}) {
        const auto directory = user / name;
        if (fs::is_symlink(fs::symlink_status(directory))) throw std::invalid_argument("Linked writable mechanical directory");
        fs::create_directories(directory);
        if (!within(fs::canonical(directory), user)) throw std::invalid_argument("Writable mechanical directory escapes user root");
    }
    state.phase = Phase::initializing;
    try {
        state.root = root;
        state.user = user;
        environment("CORVCAD_PYTHON_STDLIB", root / "lib/python3.13");
        environment("FREECAD_USER_HOME", user / "home");
        environment("FREECAD_USER_DATA", user / "data");
        environment("FREECAD_USER_TEMP", user / "temp");
        if (!corvcad_register_freecad_modules()) throw std::runtime_error("Could not register static FreeCAD modules before Python initialization");
        state.arguments = {utf8(root / "bin/CorvCADMechanical"), "--set-config",
                           "AppHomePath=" + utf8(root) + "/", "-u", utf8(user / "data/user.cfg"),
                           "-s", utf8(user / "data/system.cfg")};
        state.argv.reserve(state.arguments.size() + 1);
        for (auto& value : state.arguments) state.argv.push_back(value.data());
        state.argv.push_back(nullptr);
        if (!QCoreApplication::instance()) {
            state.qt_argv[0] = state.argv[0];
            state.qt = new QCoreApplication(state.qt_argc, state.qt_argv.data());
        }
        std::setlocale(LC_NUMERIC, "C");
        auto& config = App::Application::Config();
        config["ExeName"] = "CorvCAD";
        config["ExeVendor"] = "CorvCAD";
        config["AppDataSkipVendor"] = "true";
        config["RunMode"] = "Exit";
        config["LoggingConsole"] = "0";
        App::Application::init(static_cast<int>(state.arguments.size()), state.argv.data());
        if (!Py_IsInitialized()) throw std::runtime_error("FreeCAD did not initialize the bundled interpreter");
        GIL gil;
        configure_bundled_directories(root);
        validate_interpreter();
        Object helper_path(checked(PyUnicode_DecodeFSDefault(utf8(root / "harness").c_str()), "Create helper path"));
        PyObject* search = PySys_GetObject("path");
        if (!search || !PyList_Check(search) || PyList_Insert(search, 0, helper_path.get()) < 0)
            python_error("Configure bundled mechanical helper path");
        Object module(checked(PyImport_ImportModule("session_runtime"), "Import mechanical session backend"));
        Object filename(checked(PyObject_GetAttrString(module.get(), "__file__"), "Read session backend origin"));
        if (fs::canonical(fs::u8path(unicode(filename.get()))) != fs::canonical(root / "harness/session_runtime.py"))
            throw std::runtime_error("Mechanical session backend was loaded outside the bundled runtime");
        Object session_class(checked(PyObject_GetAttrString(module.get(), "Session"), "Read mechanical Session class"));
        if (!PyCallable_Check(session_class.get())) throw std::runtime_error("Mechanical Session backend is not callable");
        state.session_class = session_class.release();
        state.phase = Phase::ready;
        return 1;
    }
    catch (...) { state.phase = Phase::failed; throw; }
}

int simple(Runtime& state, cc_mechanical_session* handle, const char* method)
{
    auto& current = session(state, handle);
    GIL gil;
    Object result = no_args(current.object, method);
    require_none(result.get());
    return 1;
}

int export_part(Runtime& state, cc_mechanical_session* handle, int part, const char* path, const char* method)
{
    part_argument(part);
    text_argument(path, "export path");
    auto& current = session(state, handle);
    GIL gil;
    Object result(checked(PyObject_CallMethod(current.object, method, "is", part, path), method));
    require_none(result.get());
    return 1;
}
} // namespace

extern "C" {
int cc_mechanical_available(void) { return 1; }

int cc_mechanical_initialize(const char* root, const char* user, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return initialize(state, root, user); });
}

cc_mechanical_session* cc_mechanical_open(const char* document, const char* input, cc_settings initial, char* error, size_t size)
{
    return boundary(error, size, static_cast<cc_mechanical_session*>(nullptr), [&](Runtime& state) {
        ready(state);
        text_argument(document, "document path");
        text_argument(input, "board input JSON");
        GIL gil;
        Object settings = settings_object(initial);
        state.handles.emplace_back();
        auto& result = state.handles.back();
        Object instance(checked(PyObject_CallFunction(state.session_class, "ssO", document, input, settings.get()), "Open mechanical document"));
        result.object = instance.release();
        return &result;
    });
}

void cc_mechanical_close(cc_mechanical_session* handle)
{
    boundary(nullptr, 0, 0, [&](Runtime& state) {
        auto& current = session(state, handle);
        GIL gil;
        Object instance(current.object);
        current.object = nullptr;
        Object result = no_args(instance.get(), "close");
        require_none(result.get());
        return 1;
    });
}

int cc_mechanical_get_settings(cc_mechanical_session* handle, cc_settings* output, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) {
        if (!output) throw std::invalid_argument("Missing enclosure settings output");
        auto& current = session(state, handle);
        GIL gil;
        Object settings = no_args(current.object, "get_settings");
        if (!PyDict_Check(settings.get()) || PyDict_Size(settings.get()) != 8) throw std::runtime_error("Unexpected enclosure settings schema");
        std::array<double, 8> dimensions{};
        for (size_t index = 0; index < dimensions.size(); ++index)
            dimensions[index] = number(PyDict_GetItemString(settings.get(), parameter_names[index]));
        *output = {dimensions[0], dimensions[1], dimensions[2], dimensions[3], dimensions[4], dimensions[5], dimensions[6], dimensions[7]};
        return 1;
    });
}

int cc_mechanical_set_parameter(cc_mechanical_session* handle, const char* name, double value, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) {
        text_argument(name, "parameter name");
        if (std::none_of(parameter_names.begin(), parameter_names.end(), [&](const char* item) { return std::strcmp(item, name) == 0; }))
            throw std::invalid_argument("Unknown enclosure parameter");
        if (!std::isfinite(value)) throw std::invalid_argument("Enclosure parameter must be finite");
        auto& current = session(state, handle);
        GIL gil;
        Object result(checked(PyObject_CallMethod(current.object, "set_parameter", "sd", name, value), "Set enclosure parameter"));
        require_none(result.get());
        return 1;
    });
}

int cc_mechanical_recompute(cc_mechanical_session* handle, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return simple(state, handle, "recompute"); });
}

int cc_mechanical_can_undo(cc_mechanical_session* handle)
{
    return boundary(nullptr, 0, 0, [&](Runtime& state) {
        auto& current = session(state, handle);
        GIL gil;
        Object result = no_args(current.object, "can_undo");
        if (!PyBool_Check(result.get())) throw std::runtime_error("Invalid undo availability result");
        return result.get() == Py_True ? 1 : 0;
    });
}

int cc_mechanical_can_redo(cc_mechanical_session* handle)
{
    return boundary(nullptr, 0, 0, [&](Runtime& state) {
        auto& current = session(state, handle);
        GIL gil;
        Object result = no_args(current.object, "can_redo");
        if (!PyBool_Check(result.get())) throw std::runtime_error("Invalid redo availability result");
        return result.get() == Py_True ? 1 : 0;
    });
}

int cc_mechanical_undo(cc_mechanical_session* handle, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return simple(state, handle, "undo"); });
}

int cc_mechanical_redo(cc_mechanical_session* handle, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return simple(state, handle, "redo"); });
}

size_t cc_mechanical_triangles(cc_mechanical_session* handle, int part, cc_triangle* output, size_t capacity, char* error, size_t size)
{
    return boundary(error, size, size_t{0}, [&](Runtime& state) {
        part_argument(part);
        if (!output && capacity) throw std::invalid_argument("Null triangle buffer with nonzero capacity");
        auto& current = session(state, handle);
        GIL gil;
        Object result(checked(PyObject_CallMethod(current.object, "triangles", "i", part), "Tessellate exact enclosure part"));
        if (!PyList_Check(result.get())) throw std::runtime_error("Expected a triangle list");
        const auto count = PyList_GET_SIZE(result.get());
        if (count <= 0) throw std::runtime_error("Exact enclosure part produced an empty preview");
        std::vector<cc_triangle> triangles;
        triangles.reserve(static_cast<size_t>(count));
        for (Py_ssize_t index = 0; index < count; ++index) {
            PyObject* item = PyList_GET_ITEM(result.get(), index);
            if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 9) throw std::runtime_error("Invalid preview triangle schema");
            std::array<float, 9> coordinates{};
            for (size_t coordinate = 0; coordinate < coordinates.size(); ++coordinate) {
                const double value = number(PyTuple_GET_ITEM(item, static_cast<Py_ssize_t>(coordinate)));
                if (std::abs(value) > std::numeric_limits<float>::max()) throw std::runtime_error("Preview coordinate exceeds float range");
                coordinates[coordinate] = static_cast<float>(value);
            }
            triangles.push_back({coordinates[0], coordinates[1], coordinates[2], coordinates[3], coordinates[4],
                                 coordinates[5], coordinates[6], coordinates[7], coordinates[8]});
        }
        if (output && capacity >= triangles.size()) std::copy(triangles.begin(), triangles.end(), output);
        return triangles.size();
    });
}

int cc_mechanical_save(cc_mechanical_session* handle, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return simple(state, handle, "save"); });
}

int cc_mechanical_export_step(cc_mechanical_session* handle, int part, const char* path, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return export_part(state, handle, part, path, "export_step"); });
}

int cc_mechanical_export_stl(cc_mechanical_session* handle, int part, const char* path, char* error, size_t size)
{
    return boundary(error, size, 0, [&](Runtime& state) { return export_part(state, handle, part, path, "export_stl"); });
}

char* cc_mechanical_call_json(cc_mechanical_session* handle, const char* method, const char* arguments, char* error,
                              size_t size)
{
    return boundary(error, size, static_cast<char*>(nullptr), [&](Runtime& state) {
        text_argument(method, "JSON method");
        auto& current = session(state, handle);
        GIL gil;
        // The session refuses methods outside its JSON whitelist.
        Object result(checked(PyObject_CallMethod(current.object, "call_json", "ss", method, arguments ? arguments : "{}"),
                              "Mechanical JSON request"));
        const std::string text = unicode(result.get());
        char* copy = static_cast<char*>(std::malloc(text.size() + 1));
        if (!copy) throw std::bad_alloc();
        std::memcpy(copy, text.c_str(), text.size() + 1);
        return copy;
    });
}
} // extern "C"

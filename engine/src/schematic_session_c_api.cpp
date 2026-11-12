#include "corvcad/schematic_session_c_api.h"
#include "corvcad/schematic_session.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <locale>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>

struct cc_sch_session { std::shared_ptr<corvcad::SchematicSession> value; };
struct cc_sch_transaction { std::shared_ptr<corvcad::SchematicSession> value; bool active = true; };

namespace {
char *owned(const std::string &value) {
  auto *text = static_cast<char *>(std::malloc(value.size() + 1));
  if (!text) throw std::bad_alloc();
  std::memcpy(text, value.c_str(), value.size() + 1);
  return text;
}
void success(cc_sch_result *result) {
  if (result) { std::free(result->message); result->code = 0; result->message = nullptr; }
}
void failure(cc_sch_result *result, const std::exception &error) {
  if (result) {
    std::free(result->message);
    const auto *typed = dynamic_cast<const corvcad::SchematicError *>(&error);
    result->code = typed ? static_cast<int>(typed->code()) : CC_SCH_INVALID;
    try { result->message = owned(error.what()); }
    catch (...) { result->message = nullptr; }
  }
}
std::string required(const char *text, const char *name) {
  if (!text) throw std::runtime_error(std::string("Missing ") + name);
  return text;
}
corvcad::SchematicSession &active(cc_sch_transaction *transaction) {
  if (!transaction || !transaction->active) throw std::runtime_error("Transaction is closed");
  return *transaction->value;
}
template<class F> int invoke(F &&operation, cc_sch_result *result) {
  try { operation(); success(result); return 1; }
  catch (const std::exception &error) { failure(result, error); return 0; }
}
std::string quote(const std::string &value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    switch (c) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                      << static_cast<int>(c) << std::dec;
      else out << static_cast<char>(c);
    }
  }
  return out.str() + '"';
}
std::string snapshot(const corvcad::SchematicSession &session) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17)
      << "{\"backend\":\"compatible-source-edit\",\"kiCadErcAvailable\":false,\"sheets\":[";
  bool first_sheet = true;
  for (const auto &instance : session.sheets()) {
    if (!first_sheet) out << ',';
    first_sheet = false;
    const auto &sheet = session.sheet(instance.path);
    const bool shared_file = std::count_if(session.sheets().begin(), session.sheets().end(),
        [&](const corvcad::SheetInstance &item) { return item.file == instance.file; }) > 1;
    out << "{\"path\":" << quote(instance.path) << ",\"file\":" << quote(instance.file)
        << ",\"name\":" << quote(instance.name) << ",\"uuid\":" << quote(sheet.uuid)
        << ",\"version\":" << sheet.version << ",\"sharedFile\":"
        << (shared_file ? "true" : "false") << ",\"unsupportedConstructs\":[";
    for (size_t i = 0; i < sheet.unknown_top_level.size(); ++i) {
      if (i) out << ',';
      out << quote(sheet.unknown_top_level[i]);
    }
    out << "],\"symbols\":[";
    bool first = true;
    for (const auto &item : sheet.symbols) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"reference\":" << quote(item.reference)
          << ",\"value\":" << quote(item.value) << ",\"libraryId\":" << quote(item.library_id)
          << ",\"footprint\":" << quote(item.footprint)
          << ",\"x\":" << item.position.x << ",\"y\":" << item.position.y
          << ",\"rotation\":" << item.rotation << '}';
    }
    out << "],\"wires\":[";
    first = true;
    for (const auto &item : sheet.wires) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"x0\":" << item.start.x
          << ",\"y0\":" << item.start.y << ",\"x1\":" << item.end.x
          << ",\"y1\":" << item.end.y << '}';
    }
    out << "],\"buses\":[";
    first = true;
    for (const auto &item : sheet.buses) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"x0\":" << item.start.x
          << ",\"y0\":" << item.start.y << ",\"x1\":" << item.end.x
          << ",\"y1\":" << item.end.y << '}';
    }
    out << "],\"labels\":[";
    first = true;
    for (const auto &item : sheet.labels) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"text\":" << quote(item.text)
          << ",\"kind\":" << quote(item.kind) << ",\"x\":" << item.position.x
          << ",\"y\":" << item.position.y << '}';
    }
    out << "],\"junctions\":[";
    first = true;
    for (const auto &item : sheet.junctions) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"x\":" << item.position.x
          << ",\"y\":" << item.position.y << '}';
    }
    out << "],\"noConnects\":[";
    first = true;
    for (const auto &item : sheet.no_connects) {
      if (!first) out << ',';
      first = false;
      out << "{\"id\":" << quote(instance.path + ":" + item.uuid)
          << ",\"uuid\":" << quote(item.uuid) << ",\"x\":" << item.position.x
          << ",\"y\":" << item.position.y << '}';
    }
    out << "]}";
  }
  return out.str() + "]}";
}
} // namespace

extern "C" {
int cc_sch_api_version(void) { return 11; }
void cc_sch_result_clear(cc_sch_result *result) {
  if (!result) return;
  std::free(result->message);
  result->message = nullptr;
  result->code = 0;
}
cc_sch_session *cc_sch_session_open(const char *path, cc_sch_result *result) {
  try {
    auto *session = new cc_sch_session{std::make_shared<corvcad::SchematicSession>(
        corvcad::SchematicSession::open(required(path, "schematic path")))};
    success(result);
    return session;
  } catch (const std::exception &error) { failure(result, error); return nullptr; }
}
void cc_sch_session_free(cc_sch_session *session) { delete session; }
const char *cc_sch_session_backend(const cc_sch_session *session) {
  return session ? "compatible-source-edit" : nullptr;
}
unsigned cc_sch_session_capabilities(const cc_sch_session *session) {
  return session ? CC_SCH_TRANSACTIONS | CC_SCH_HIERARCHY | CC_SCH_SOURCE_PRESERVING : 0;
}
char *cc_sch_session_snapshot_json(const cc_sch_session *session, cc_sch_result *result) {
  try {
    if (!session) throw std::runtime_error("Missing schematic session");
    char *text = owned(snapshot(*session->value));
    success(result);
    return text;
  } catch (const std::exception &error) { failure(result, error); return nullptr; }
}
cc_sch_transaction *cc_sch_transaction_begin(cc_sch_session *session, cc_sch_result *result) {
  try {
    if (!session) throw std::runtime_error("Missing schematic session");
    auto transaction = std::make_unique<cc_sch_transaction>(cc_sch_transaction{session->value});
    session->value->begin();
    success(result);
    return transaction.release();
  } catch (const std::exception &error) { failure(result, error); return nullptr; }
}
int cc_sch_transaction_move_symbol(cc_sch_transaction *t, const char *path, const char *uuid,
                                   double x, double y, cc_sch_result *result) {
  return invoke([&] { active(t).move_symbol(required(path, "sheet path"),
                                            required(uuid, "symbol UUID"), x, y); }, result);
}
int cc_sch_transaction_rotate_symbol(cc_sch_transaction *t, const char *path, const char *uuid,
                                     cc_sch_result *result) {
  return invoke([&] { active(t).rotate_symbol(required(path, "sheet path"),
                                              required(uuid, "symbol UUID")); }, result);
}
int cc_sch_transaction_set_property(cc_sch_transaction *t, const char *path, const char *uuid,
                                    const char *property, const char *value, cc_sch_result *result) {
  return invoke([&] { active(t).set_property(required(path, "sheet path"),
      required(uuid, "symbol UUID"), required(property, "property"), required(value, "value")); }, result);
}
int cc_sch_transaction_rename_symbol(cc_sch_transaction *t, const char *path, const char *uuid,
                                     const char *reference, cc_sch_result *result) {
  return invoke([&] { active(t).rename_symbol(required(path, "sheet path"),
      required(uuid, "symbol UUID"), required(reference, "reference")); }, result);
}
int cc_sch_transaction_delete_item(cc_sch_transaction *t, const char *path, const char *uuid,
                                   cc_sch_result *result) {
  return invoke([&] { active(t).delete_item(required(path, "sheet path"),
                                             required(uuid, "item UUID")); }, result);
}
int cc_sch_transaction_add_wire(cc_sch_transaction *t, const char *path, double x0, double y0,
                                double x1, double y1, cc_sch_result *result) {
  return invoke([&] { active(t).add_wire(required(path, "sheet path"), x0, y0, x1, y1); }, result);
}
int cc_sch_transaction_add_label(cc_sch_transaction *t, const char *path, const char *name,
                                 double x, double y, cc_sch_result *result) {
  return invoke([&] { active(t).add_label(required(path, "sheet path"),
                                           required(name, "label"), x, y); }, result);
}
int cc_sch_transaction_add_two_pin(cc_sch_transaction *t, const char *path, double x, double y,
                                   cc_sch_result *result) {
  return invoke([&] { active(t).add_two_pin(required(path, "sheet path"), x, y); }, result);
}
// Library symbols need KiCad's libraries (NATIVE_KICAD session).
int cc_sch_transaction_add_library_symbol(cc_sch_transaction *, const char *, const char *, double,
                                          double, cc_sch_result *result) {
  failure(result, corvcad::SchematicError(corvcad::SchematicErrorCode::Unsupported,
                                          "Library symbols need the KiCad edition"));
  return 0;
}
int cc_sch_transaction_add_routed_wire(cc_sch_transaction *t, const char *path, double x0,
                                       double y0, double x1, double y1, cc_sch_result *result) {
  return invoke([&] { active(t).add_routed_wire(required(path, "sheet path"), x0, y0, x1, y1); },
                result);
}
int cc_sch_transaction_move_label(cc_sch_transaction *t, const char *path, const char *uuid,
                                  double x, double y, cc_sch_result *result) {
  return invoke([&] { active(t).move_label(required(path, "sheet path"),
                                           required(uuid, "label UUID"), x, y); }, result);
}
int cc_sch_transaction_rename_label(cc_sch_transaction *t, const char *path, const char *uuid,
                                    const char *name, cc_sch_result *result) {
  return invoke([&] { active(t).rename_label(required(path, "sheet path"),
      required(uuid, "label UUID"), required(name, "label")); }, result);
}
int cc_sch_transaction_commit(cc_sch_transaction *t, cc_sch_result *result) {
  return invoke([&] { active(t).commit(); t->active = false; }, result);
}
void cc_sch_transaction_free(cc_sch_transaction *t) {
  if (!t) return;
  if (t->active) t->value->abort();
  delete t;
}
int cc_sch_session_can_undo(const cc_sch_session *session) {
  return session && session->value->can_undo() ? 1 : 0;
}
int cc_sch_session_can_redo(const cc_sch_session *session) {
  return session && session->value->can_redo() ? 1 : 0;
}
char *cc_sch_session_sheet_source(const cc_sch_session *session, const char *path,
                                  cc_sch_result *result) {
  try {
    if (!session) throw std::runtime_error("Missing schematic session");
    char *text = owned(session->value->source(required(path, "sheet path")));
    success(result);
    return text;
  } catch (const std::exception &error) { failure(result, error); return nullptr; }
}
size_t cc_sch_session_route(const cc_sch_session *session, const char *path, double x0,
                            double y0, double x1, double y1, double *xy, size_t capacity,
                            cc_sch_result *result) {
  try {
    if (!session) throw std::runtime_error("Missing schematic session");
    const auto route = session->value->route(required(path, "sheet path"), x0, y0, x1, y1);
    for (size_t i = 0; i < route.size() && i < capacity && xy; ++i) {
      xy[2 * i] = route[i].x;
      xy[2 * i + 1] = route[i].y;
    }
    success(result);
    return route.size();
  } catch (const std::exception &error) { failure(result, error); return 0; }
}
int cc_sch_session_undo(cc_sch_session *session, cc_sch_result *result) {
  return invoke([&] { if (!session || !session->value->undo())
      throw std::runtime_error("Nothing to undo"); }, result);
}
int cc_sch_session_redo(cc_sch_session *session, cc_sch_result *result) {
  return invoke([&] { if (!session || !session->value->redo())
      throw std::runtime_error("Nothing to redo"); }, result);
}
int cc_sch_session_validate_structure(cc_sch_session *session, cc_sch_result *result) {
  return invoke([&] { if (!session) throw std::runtime_error("Missing schematic session");
                       session->value->validate_structure(); }, result);
}
int cc_sch_session_save(cc_sch_session *session, cc_sch_result *result) {
  return invoke([&] { if (!session) throw std::runtime_error("Missing schematic session");
                       session->value->save(); }, result);
}
int cc_sch_session_save_copy(cc_sch_session *session, const char *directory,
                             cc_sch_result *result) {
  return invoke([&] { if (!session) throw std::runtime_error("Missing schematic session");
                       session->value->save_copy(required(directory, "destination")); }, result);
}
char *cc_sch_session_netlist(cc_sch_session *, cc_sch_result *result) {
  failure(result, corvcad::SchematicError(corvcad::SchematicErrorCode::Unsupported,
                                          "This backend does not calculate KiCad connectivity"));
  return nullptr;
}
// API version 6: KiCad's editing tools, only with CC_SCH_KICAD_TOOLS (the KiCad edition).
namespace {
int kicadToolsOnly(cc_sch_result *result) {
  failure(result, corvcad::SchematicError(corvcad::SchematicErrorCode::Unsupported,
                                          "This tool needs the KiCad edition"));
  return 0;
}
} // namespace
int cc_sch_transaction_annotate(cc_sch_transaction *, int, cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_bus(cc_sch_transaction *, const char *, double, double, double, double,
                               cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_bus_entry(cc_sch_transaction *, const char *, double, double, int,
                                     cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_scoped_label(cc_sch_transaction *, const char *, const char *, double, double, int,
                                        cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_sheet(cc_sch_transaction *, const char *, const char *, double, double, double, double,
                                 cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_sheet_pins(cc_sch_transaction *, const char *, const char *, cc_sch_result *result) {
  return kicadToolsOnly(result);
}
int cc_sch_transaction_add_no_connect(cc_sch_transaction *, const char *, double, double, cc_sch_result *result) {
  return kicadToolsOnly(result);
}
int cc_sch_transaction_mirror_symbol(cc_sch_transaction *, const char *, const char *, int, cc_sch_result *result) {
  return kicadToolsOnly(result);
}
int cc_sch_transaction_add_text(cc_sch_transaction *, const char *, const char *, double, double, cc_sch_result *result) {
  return kicadToolsOnly(result);
}
int cc_sch_transaction_add_note_line(cc_sch_transaction *, const char *, double, double, double, double,
                                     cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_add_bus_bus_entry(cc_sch_transaction *, const char *, double, double, int,
                                         cc_sch_result *result) { return kicadToolsOnly(result); }
int cc_sch_transaction_resize_sheet(cc_sch_transaction *, const char *, const char *, double, double,
                                    cc_sch_result *result) { return kicadToolsOnly(result); }
char *cc_sch_import(const char *, const char *, const char *, cc_sch_result *result) {
  kicadToolsOnly(result);
  return nullptr;
}
char *cc_sch_session_bom_csv(cc_sch_session *, cc_sch_result *result) {
  kicadToolsOnly(result);
  return nullptr;
}
char *cc_sch_session_simulate(cc_sch_session *, const char *, size_t, cc_sch_result *result) {
  kicadToolsOnly(result);
  return nullptr;
}
int cc_sch_simulate_stop(void) { return 0; }
char *cc_sch_session_spice_netlist(cc_sch_session *, const char *, cc_sch_result *result) {
  kicadToolsOnly(result);
  return nullptr;
}
int cc_sch_session_plot(cc_sch_session *, int, const char *, cc_sch_result *result) {
  kicadToolsOnly(result);
  return 0;
}
char *cc_sch_session_erc_json(cc_sch_session *, cc_sch_result *result) {
  failure(result, corvcad::SchematicError(corvcad::SchematicErrorCode::Unsupported,
                                          "This backend does not run KiCad ERC"));
  return nullptr;
}
}

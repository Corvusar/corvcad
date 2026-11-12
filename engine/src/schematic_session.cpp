#include "corvcad/schematic_session.hpp"
#include "corvcad/authoring.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace corvcad {
namespace {
namespace fs = std::filesystem;
constexpr int kKiCad1006SchematicVersion = 20260306;
constexpr size_t kUndoBudgetBytes = 64 * 1024 * 1024;

std::string read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw SchematicError(SchematicErrorCode::Io,
                                   "Cannot read schematic: " + utf8_string(path));
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write(const fs::path &path, const std::string &contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) throw SchematicError(SchematicErrorCode::Io,
                                   "Cannot write schematic: " + utf8_string(path));
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!stream) throw SchematicError(SchematicErrorCode::Io,
                                   "Incomplete schematic write: " + utf8_string(path));
}

bool within(const fs::path &path, const fs::path &root) {
  auto a = path.begin(), b = root.begin();
  for (; b != root.end(); ++a, ++b)
    if (a == path.end() || *a != *b) return false;
  return true;
}

std::string relative_file(const fs::path &path, const fs::path &root) {
  const fs::path canonical = fs::weakly_canonical(path);
  if (!within(canonical, root) || !fs::is_regular_file(canonical))
    throw SchematicError(SchematicErrorCode::Unsupported,
                         "Sheet file must exist inside the project directory");
  return generic_utf8_string(canonical.lexically_relative(root));
}
}

SchematicSession SchematicSession::open(const std::string &project_or_schematic) {
  fs::path root = fs::absolute(utf8_path(project_or_schematic));
  if (root.extension() == ".kicad_pro") root.replace_extension(".kicad_sch");
  if (root.extension() != ".kicad_sch" || !fs::is_regular_file(root))
    throw SchematicError(SchematicErrorCode::Invalid,
                         "Open a KiCad .kicad_pro or .kicad_sch file");

  SchematicSession session;
  session.folder_ = fs::canonical(root.parent_path());
  const std::string root_file = relative_file(root, session.folder_);
  std::set<std::string> visiting;
  std::set<std::string> seen_instances;
  // KiCad instance paths: "/<root uuid>" for the root, then "/<sheet symbol uuid>" per level.
  std::function<void(const std::string &, const std::string &, const std::string &,
                     const std::string &)> load;
  load = [&](const std::string &file, const std::string &parent,
             const std::string &sheet_uuid, const std::string &name) {
    if (!visiting.insert(file).second)
      throw SchematicError(SchematicErrorCode::Unsupported,
                           "Cyclic schematic sheet hierarchy");
    auto found = session.documents_.find(file);
    if (found == session.documents_.end()) {
      const std::string contents = read(session.folder_ / utf8_path(file));
      found = session.documents_.emplace(file, Document{contents,
                                   parse_kicad_schematic(contents)}).first;
    }
    if (found->second.model.uuid.empty())
      throw std::runtime_error("Schematic sheet has no UUID: " + file);
    const std::string instance_path = parent + "/" +
        (parent.empty() ? found->second.model.uuid : sheet_uuid);
    if (!seen_instances.insert(instance_path).second)
      throw SchematicError(SchematicErrorCode::Invalid,
                           "Duplicate schematic sheet instance path");
    session.instances_.push_back({instance_path, file, name});
    for (const auto &child : found->second.model.sheets) {
      if (child.uuid.empty() || child.file.empty())
        throw std::runtime_error("Schematic child sheet lacks UUID or filename");
      const std::string child_file = relative_file(
          session.folder_ / utf8_path(file).parent_path() / utf8_path(child.file),
          session.folder_);
      load(child_file, instance_path, child.uuid, child.name.empty() ? child.file : child.name);
    }
    visiting.erase(file);
  };
  session.project_ = utf8_string(root.stem());
  load(root_file, "", "", session.project_);
  session.saved_ = session.documents_;
  session.history_.push_back(session.documents_);
  return session;
}

const SheetInstance &SchematicSession::instance(const std::string &path) const {
  const auto found = std::find_if(instances_.begin(), instances_.end(),
                                  [&](const SheetInstance &item) { return item.path == path; });
  if (found == instances_.end()) throw SchematicError(SchematicErrorCode::NotFound,
                                                      "Unknown schematic sheet path");
  return *found;
}

const Schematic &SchematicSession::sheet(const std::string &path) const {
  return documents_.at(instance(path).file).model;
}

const std::string &SchematicSession::source(const std::string &path) const {
  return documents_.at(instance(path).file).source;
}

void SchematicSession::begin() {
  if (pending_) throw std::runtime_error("A schematic transaction is already open");
  pending_ = documents_;
}

SchematicSession::Document &SchematicSession::pending_document(const std::string &path) {
  if (!pending_) throw std::runtime_error("Begin a schematic transaction first");
  return pending_->at(instance(path).file);
}

void SchematicSession::move_symbol(const std::string &path, const std::string &uuid,
                                   double x, double y) {
  auto &doc = pending_document(path);
  const auto edited = move_schematic_symbol(doc.source, uuid, x, y);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

void SchematicSession::rotate_symbol(const std::string &path, const std::string &uuid) {
  auto &doc = pending_document(path);
  replace(doc, rotate_schematic_symbol(doc.source, uuid));
}

void SchematicSession::set_property(const std::string &path, const std::string &uuid,
                                    const std::string &property, const std::string &value) {
  auto &doc = pending_document(path);
  const auto edited = set_schematic_property(doc.source, uuid, property, value);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

void SchematicSession::rename_symbol(const std::string &path, const std::string &uuid,
                                     const std::string &reference) {
  const std::string &file = instance(path).file;
  if (std::count_if(instances_.begin(), instances_.end(),
                    [&](const SheetInstance &item) { return item.file == file; }) > 1)
    throw SchematicError(SchematicErrorCode::Unsupported,
                         "Reference rename in a shared sheet needs KiCad instance semantics");
  auto &doc = pending_document(path);
  const auto edited = rename_schematic_symbol(doc.source, uuid, reference);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

void SchematicSession::delete_item(const std::string &path, const std::string &uuid) {
  auto &doc = pending_document(path);
  const auto edited = delete_schematic_item(doc.source, uuid);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

void SchematicSession::add_wire(const std::string &path, double x0, double y0,
                                double x1, double y1) {
  auto &doc = pending_document(path);
  const auto edited = add_schematic_wire(doc.source, x0, y0, x1, y1);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

void SchematicSession::add_label(const std::string &path, const std::string &name,
                                 double x, double y) {
  auto &doc = pending_document(path);
  const auto edited = add_schematic_label(doc.source, name, x, y);
  doc.model = parse_kicad_schematic(edited);
  doc.source = edited;
}

bool SchematicSession::shared_file(const std::string &file) const {
  return std::count_if(instances_.begin(), instances_.end(),
                       [&](const SheetInstance &item) { return item.file == file; }) > 1;
}

void SchematicSession::replace(Document &doc, std::string edited) {
  doc.model = parse_kicad_schematic(edited);
  doc.source = std::move(edited);
}

void SchematicSession::add_two_pin(const std::string &path, double x, double y) {
  if (shared_file(instance(path).file))
    throw SchematicError(SchematicErrorCode::Unsupported,
                         "Adding a part to a shared sheet needs KiCad per-instance references");
  auto &doc = pending_document(path);
  std::vector<std::string> taken;
  for (const auto &entry : *pending_)
    for (const auto &symbol : entry.second.model.symbols) taken.push_back(symbol.reference);
  replace(doc, add_schematic_two_pin(doc.source, project_, x, y, path, taken));
}

void SchematicSession::add_routed_wire(const std::string &path, double x0, double y0,
                                       double x1, double y1) {
  auto &doc = pending_document(path);
  replace(doc, add_schematic_orthogonal_wire(doc.source, x0, y0, x1, y1));
}

void SchematicSession::move_label(const std::string &path, const std::string &uuid,
                                  double x, double y) {
  auto &doc = pending_document(path);
  replace(doc, move_schematic_label(doc.source, uuid, x, y));
}

void SchematicSession::rename_label(const std::string &path, const std::string &uuid,
                                    const std::string &name) {
  auto &doc = pending_document(path);
  replace(doc, rename_schematic_label(doc.source, uuid, name));
}

std::vector<Point> SchematicSession::route(const std::string &path, double x0, double y0,
                                           double x1, double y1) const {
  return route_schematic_wire(source(path), x0, y0, x1, y1);
}

void SchematicSession::commit() {
  if (!pending_) throw std::runtime_error("No schematic transaction is open");
  validate_save(*pending_);
  const bool changed = std::any_of(pending_->begin(), pending_->end(),
      [&](const auto &entry) { return entry.second.source != documents_.at(entry.first).source; });
  if (changed) {
    documents_ = std::move(*pending_);
    history_.resize(history_index_ + 1);
    history_.push_back(documents_);
    ++history_index_;
    auto history_bytes = [&] {
      size_t total = 0;
      for (const auto &state : history_)
        for (const auto &entry : state) total += entry.second.source.size();
      return total;
    };
    while (history_index_ > 0 &&
           (history_.size() > 50 || history_bytes() > kUndoBudgetBytes)) {
      history_.erase(history_.begin());
      --history_index_;
    }
  }
  pending_.reset();
}

void SchematicSession::abort() { pending_.reset(); }

bool SchematicSession::undo() {
  if (pending_) throw std::runtime_error("Finish the schematic transaction first");
  if (history_index_ == 0) return false;
  documents_ = history_[--history_index_];
  return true;
}

bool SchematicSession::redo() {
  if (pending_) throw std::runtime_error("Finish the schematic transaction first");
  if (history_index_ + 1 == history_.size()) return false;
  documents_ = history_[++history_index_];
  return true;
}

void SchematicSession::validate_save(const Documents &documents) {
  for (const auto &entry : documents) {
    if (entry.second.model.version <= 0 ||
        entry.second.model.version > kKiCad1006SchematicVersion)
      throw SchematicError(SchematicErrorCode::Unsupported,
                           "Schematic version is newer than KiCad 10.0.6 or missing");
    if (entry.second.model.uuid.empty())
      throw std::runtime_error("Schematic sheet lacks a UUID");
    if (!entry.second.model.unknown_top_level.empty())
      throw SchematicError(SchematicErrorCode::Unsupported,
                           "Unsupported KiCad 10 schematic construct in " + entry.first +
                           ": " + entry.second.model.unknown_top_level.front());
  }
}

void SchematicSession::validate_structure() const { validate_save(documents_); }

void SchematicSession::save() {
  if (pending_) throw std::runtime_error("Finish the schematic transaction first");
  validate_save(documents_);
  struct Swap { fs::path original, staged, backup; bool backed = false, installed = false; };
  std::vector<Swap> swaps;
  const std::string marker = ".corvcad-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  try {
    for (const auto &entry : documents_) {
      if (entry.second.source == saved_.at(entry.first).source) continue;
      const fs::path original = folder_ / utf8_path(entry.first);
      if (read(original) != saved_.at(entry.first).source)
        throw SchematicError(SchematicErrorCode::Conflict,
                             "Schematic changed on disk; reopen before saving: " + entry.first);
      swaps.push_back({original, utf8_string(original) + marker + ".tmp",
                       utf8_string(original) + marker + ".bak"});
      write(swaps.back().staged, entry.second.source);
    }
    for (auto &swap : swaps) {
      fs::rename(swap.original, swap.backup);
      swap.backed = true;
      fs::rename(swap.staged, swap.original);
      swap.installed = true;
    }
    saved_ = documents_;
  } catch (...) {
    bool rollback_failed = false;
    for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
      std::error_code ignored;
      if (it->installed) fs::remove(it->original, ignored);
      if (ignored) rollback_failed = true;
      ignored.clear();
      if (it->backed) fs::rename(it->backup, it->original, ignored);
      if (ignored) rollback_failed = true;
      ignored.clear();
      fs::remove(it->staged, ignored);
    }
    if (rollback_failed)
      throw SchematicError(SchematicErrorCode::Io,
                           "Schematic save failed and rollback was incomplete; inspect .corvcad backups");
    throw;
  }
  for (const auto &swap : swaps) {
    std::error_code ignored;
    fs::remove(swap.backup, ignored);
  }
}

void SchematicSession::save_copy(const std::string &new_directory) const {
  if (pending_) throw std::runtime_error("Finish the schematic transaction first");
  validate_save(documents_);
  const fs::path target = fs::weakly_canonical(utf8_path(new_directory));
  if (fs::exists(target) || within(target, folder_) || target.filename().empty())
    throw SchematicError(SchematicErrorCode::Invalid,
                         "Choose a new directory outside the source project");
  for (const auto &entry : fs::recursive_directory_iterator(folder_))
    if (entry.is_symlink())
      throw SchematicError(SchematicErrorCode::Unsupported,
                           "Save-copy cannot preserve a linked project file");
  const fs::path staged = target.parent_path() /
      (utf8_string(target.filename()) + ".corvcad-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
  if (fs::exists(staged))
    throw SchematicError(SchematicErrorCode::Conflict,
                         "Temporary save-copy directory already exists");
  try {
    fs::copy(folder_, staged, fs::copy_options::recursive | fs::copy_options::skip_symlinks);
    for (const auto &entry : documents_)
      write(staged / utf8_path(entry.first), entry.second.source);
    if (fs::exists(target))
      throw SchematicError(SchematicErrorCode::Conflict,
                           "Save-copy destination appeared during the write");
    fs::rename(staged, target);
  } catch (...) {
    std::error_code ignored;
    fs::remove_all(staged, ignored);
    throw;
  }
}

} // namespace corvcad

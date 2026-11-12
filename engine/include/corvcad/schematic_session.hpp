#pragma once

#include "corvcad/eda.hpp"
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace corvcad {

enum class SchematicErrorCode { Invalid = 1, NotFound = 2, Conflict = 3,
                                Unsupported = 4, Io = 5 };
class SchematicError : public std::runtime_error {
public:
  SchematicError(SchematicErrorCode code, const std::string &message) :
      std::runtime_error(message), code_(code) {}
  SchematicErrorCode code() const { return code_; }
private:
  SchematicErrorCode code_;
};

struct SheetInstance {
  std::string path;       // KiCad sheet-instance UUID path, including the root UUID.
  std::string file;       // Project-relative .kicad_sch file.
  std::string name;
};

// Transactional, source-preserving compatibility backend. This is deliberately
// separate from the planned KiCad-source backend and does not provide KiCad ERC.
class SchematicSession {
public:
  static SchematicSession open(const std::string &project_or_schematic);
  const std::vector<SheetInstance> &sheets() const { return instances_; }
  const Schematic &sheet(const std::string &instance_path) const;
  const std::string &source(const std::string &instance_path) const;

  void begin();
  void move_symbol(const std::string &instance_path, const std::string &uuid,
                   double x, double y);
  void rotate_symbol(const std::string &instance_path, const std::string &uuid);
  void set_property(const std::string &instance_path, const std::string &uuid,
                    const std::string &property, const std::string &value);
  void rename_symbol(const std::string &instance_path, const std::string &uuid,
                     const std::string &reference);
  void delete_item(const std::string &instance_path, const std::string &uuid);
  void add_wire(const std::string &instance_path, double x0, double y0,
                double x1, double y1);
  void add_label(const std::string &instance_path, const std::string &name,
                 double x, double y);
  // Touch-authoring operations. add_two_pin writes the symbol's KiCad instance path and
  // picks a reference unused across every sheet; it is refused in a shared sheet file.
  void add_two_pin(const std::string &instance_path, double x, double y);
  void add_routed_wire(const std::string &instance_path, double x0, double y0,
                       double x1, double y1);
  void move_label(const std::string &instance_path, const std::string &uuid, double x, double y);
  void rename_label(const std::string &instance_path, const std::string &uuid,
                    const std::string &name);
  // Route add_routed_wire would write, against the committed sheet.
  std::vector<Point> route(const std::string &instance_path, double x0, double y0,
                           double x1, double y1) const;
  bool can_undo() const { return !pending_ && history_index_ > 0; }
  bool can_redo() const { return !pending_ && history_index_ + 1 < history_.size(); }
  const std::string &project_name() const { return project_; }
  void commit();
  void abort();
  bool undo();
  bool redo();
  void validate_structure() const;
  void save();
  void save_copy(const std::string &new_directory) const;

private:
  struct Document { std::string source; Schematic model; };
  using Documents = std::map<std::string, Document>;
  std::filesystem::path folder_;
  std::string project_;
  std::vector<SheetInstance> instances_;
  Documents documents_;
  Documents saved_;
  std::optional<Documents> pending_;
  std::vector<Documents> history_;
  size_t history_index_ = 0;

  const SheetInstance &instance(const std::string &path) const;
  Document &pending_document(const std::string &path);
  bool shared_file(const std::string &file) const;
  void replace(Document &doc, std::string edited);
  static void validate_save(const Documents &documents);
};

} // namespace corvcad

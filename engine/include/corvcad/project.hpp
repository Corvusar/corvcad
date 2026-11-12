#pragma once

#include <string>

namespace corvcad {
// Creates a new KiCad-compatible starter project in parent/name. Never overwrites
// an existing directory. The board starts with a rectangular Edge.Cuts outline.
void create_project(const std::string &parent, const std::string &name,
                    double board_width, double board_height);
// Renames parent/name to parent/new_name as KiCad's Save Project As would in place: its
// "<name>.<ext>" files become "<new_name>.<ext>", and the project file and board refer to the
// renamed files. Never overwrites an existing directory.
void rename_project(const std::string &parent, const std::string &name, const std::string &new_name);
}

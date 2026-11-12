#pragma once

#include <string>
#include <vector>

namespace corvcad {
// Project libraries: the rows of a project's sym-lib-table and fp-lib-table, their symbols and
// footprints, and simple generated parts. Library files are written in place (atomically);
// libraries outside the project folder are listed but never written.

// A box symbol: pins on the left and right sides, 2.54 mm apart.
struct SymbolPin { std::string number, name, type = "passive"; bool right = false; };
struct SymbolSpec {
  std::string name, reference = "U", value, footprint, description;
  std::vector<SymbolPin> pins;
};
// A row (rows 1) or two facing rows (rows 2, numbered down the left and up the right, as DIP
// and SOIC) of identical pads, centred on the origin. `span` is the distance between the rows'
// centres; `drill` applies to through-hole pads.
struct FootprintSpec {
  std::string name, description;
  bool smd = true;
  int pads = 2, rows = 1;
  double pitch = 2.54, span = 0, pad_width = 1.7, pad_height = 1.7, drill = 1;
};

// Tab-separated specs, one field per line: "name\tChip", "reference\tU", "value\t...",
// "footprint\tLib:Name", "description\t...", "pin\t<number>\t<name>\t<type>\t<left|right>";
// footprints: "name", "description", "type\tsmd|tht", "pads", "rows", "pitch", "span",
// "pad\t<width>\t<height>", "drill".
SymbolSpec parse_symbol_spec(const std::string &text);
FootprintSpec parse_footprint_spec(const std::string &text);
// The KiCad 10 text of one library symbol ((symbol "Name" ...)) or footprint file.
std::string library_symbol_text(const SymbolSpec &spec);
std::string library_footprint_text(const FootprintSpec &spec);

// JSON: {"symbols":[library...],"footprints":[library...]}, each library
// {"name","uri","editable","error","items":[...]} in table order. A symbol item has its
// fields, "pins" ([number,name,type,side]) and "shape" (drawing in symbol millimetres, y down:
// "graphics" [[closed,fill,x,y,...]], "pins" [[x,y,endX,endY,number,name]]); a footprint item
// has "description", "smd", "pads" ([x,y,width,height,rotation,shape,number,type]).
std::string project_libraries_json(const std::string &folder);

// Editing works on an item's KiCad text: a library symbol ((symbol "Name" ...)) or a footprint
// file. Edits patch that text and keep everything they do not touch (other shapes, text,
// properties, body styles, 3D models, pad attributes). A template: library_generate.
std::string library_generate(bool footprint, const std::string &spec_text);
std::string library_item_text(const std::string &folder, bool footprint, const std::string &library,
                              const std::string &name);
// One tab-separated edit; symbol coordinates are KiCad's library millimetres (y up), footprint
// coordinates footprint millimetres (y down). Indices are the editor form's "i".
//   symbols:    field <key> <value> | name <new> | units <n> |
//               pin add <unit> <number> <name> <type> <x> <y> <angle> <length> |
//               pin set <i> <number> <name> <type> [<length>] | pin move <i> <x> <y> <angle> | pin delete <i> |
//               shape add <unit> rect <fill> <x0> <y0> <x1> <y1> | ... circle <fill> <cx> <cy> <r> |
//               ... poly <fill> <x> <y> <x> <y>... | ... arc <fill> <sx> <sy> <mx> <my> <ex> <ey> |
//               shape move <i> <dx> <dy> | shape fill <i> <fill> | shape delete <i>
//               (unit 0 is shared by every unit; fill 0 none, 1 outline, 2 background)
//   footprints: name <new> | field descr <text> | attr smd|through_hole |
//               pad add <number> <smd|thru_hole|np_thru_hole> <rect|roundrect|circle|oval> <x> <y> <w> <h> [<drill>] |
//               pad set <i> <number> <type> <shape> <w> <h> [<drill>] | pad move <i> <x> <y> | pad delete <i> |
//               shape add line|rect <layer> <x0> <y0> <x1> <y1> <width> | shape add circle <layer> <cx> <cy> <r> <width> |
//               shape move <i> <dx> <dy> | shape delete <i>
// A derived symbol takes only field and name edits.
std::string library_edit(bool footprint, const std::string &text, const std::string &op);
// The editor form as JSON. Symbols: name, derived, base, units, fields, shapes
// [{i,unit,kind,fill,closed,text,points}], pins [{i,unit,number,name,type,x,y,angle,length,hidden}]
// (a derived symbol's shapes and pins come from `base_text`). Footprints: name, description,
// attr, pads [{i,number,type,shape,x,y,rot,w,h,drill}], shapes [{i,kind,layer,width,closed,text,points}].
std::string library_item_json(bool footprint, const std::string &text, const std::string &base_text = "");
// Writes an item's text into an editable library, replacing `original` (renamed when the name
// changed; "" for a new item). Without `replace`, an existing item of the new name is refused.
void save_library_item_text(const std::string &folder, bool footprint, const std::string &library,
                            const std::string &original, const std::string &text, bool replace);
// Copies a .kicad_mod file into an editable footprint library.
void import_library_footprint(const std::string &folder, const std::string &library, const std::string &source);

// Copies a .kicad_sym file or a .pretty folder into the project and adds its table row (named
// after the file). Refuses a name the table already has.
void import_project_library(const std::string &folder, const std::string &source);
void create_project_library(const std::string &folder, bool footprint, const std::string &name);
// Removes the row and, for a library inside the project folder, its file or folder.
// CorvCAD's starter library stays.
void remove_project_library(const std::string &folder, bool footprint, const std::string &name);
// Adds or (with replace) overwrites a symbol or footprint in an editable project library.
void save_library_symbol(const std::string &folder, const std::string &library, const SymbolSpec &spec,
                         bool replace);
void save_library_footprint(const std::string &folder, const std::string &library, const FootprintSpec &spec,
                            bool replace);
void delete_library_item(const std::string &folder, bool footprint, const std::string &library,
                         const std::string &name);
}

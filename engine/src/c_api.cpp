#include "corvcad/c_api.h"
#include "corvcad/engine.hpp"
#include "corvcad/board_input.hpp"
#include "corvcad/eda.hpp"
#include "corvcad/project.hpp"
#include "corvcad/authoring.hpp"
#include "corvcad/library.hpp"
#include "corvcad/gerber.hpp"
#include "corvcad/calculator.hpp"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <stdexcept>

struct cc_board { corvcad::Board value; double thickness = 0; };
struct cc_schematic { corvcad::Schematic value; std::vector<corvcad::SchNet> nets; };
namespace {
void copy(char *dest,size_t size,const std::string &source) {
  if(!dest||!size)return;
  size_t n=std::min(size-1,source.size()); std::memcpy(dest,source.data(),n); dest[n]=0;
}
corvcad::Settings convert(cc_settings s) {
  return {s.margin,s.wall,s.floor,s.board_clearance,s.standoff_height,
          s.standoff_diameter,s.lid_thickness,s.lid_gap};
}
char *owned_text(const std::string &value) {
  auto *result=static_cast<char *>(std::malloc(value.size()+1));
  if(!result)throw std::bad_alloc();
  std::memcpy(result,value.c_str(),value.size()+1);
  return result;
}
// Shape corners passed as x,y pairs.
std::vector<corvcad::Point> shape_points(const double *xy,size_t count) {
  if(!xy&&count)throw std::runtime_error("Missing shape corners");
  std::vector<corvcad::Point> points;
  for(size_t i=0;i<count;++i)points.push_back({xy[2*i],xy[2*i+1]});
  return points;
}
}
extern "C" {
int cc_create_project(const char *parent,const char *name,double board_width,
                      double board_height,char *error,size_t error_size) {
  try {
    if(!parent||!name)throw std::runtime_error("Missing project location or name");
    corvcad::create_project(parent,name,board_width,board_height);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_rename_project(const char *parent,const char *name,const char *new_name,char *error,size_t error_size) {
  try {
    if(!parent||!name||!new_name)throw std::runtime_error("Missing project location or name");
    corvcad::rename_project(parent,name,new_name);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_prepare_project_libraries(const char *folder,char *error,size_t error_size) {
  try {
    if(!folder)throw std::runtime_error("Missing project folder");
    corvcad::ensure_authoring_libraries(folder);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
char *cc_gerber_json(const char *text,const char *name,char *error,size_t error_size) {
  try {
    if(!text)throw std::runtime_error("Missing Gerber text");
    auto *result=owned_text(corvcad::gerber_json(corvcad::parse_gerber_file(text,name?name:"")));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
int cc_calc_track(double current_a,double width_mm,double thickness_um,double rise_c,double length_mm,int internal,
                  cc_track_calc *out,char *error,size_t error_size) {
  try {
    if(!out)throw std::runtime_error("Missing result");
    const auto r=corvcad::track(current_a,width_mm,thickness_um,rise_c,length_mm,internal!=0);
    *out={r.current_a,r.width_mm,r.area_mm2,r.resistance_ohm,r.voltage_drop_v,r.power_w};
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_calc_via(cc_via_calc_input in,cc_via_calc *out,char *error,size_t error_size) {
  try {
    if(!out)throw std::runtime_error("Missing result");
    const auto r=corvcad::via({in.hole_mm,in.plating_um,in.length_mm,in.pad_mm,in.clearance_mm,in.impedance_ohm,
                               in.current_a,in.resistivity,in.permittivity,in.rise_c,in.rise_time_ns});
    *out={r.resistance_ohm,r.voltage_drop_v,r.power_w,r.ampacity_a,r.thermal_c_per_w,r.capacitance_pf,
          r.rise_degradation_ps,r.inductance_nh,r.reactance_ohm};
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_calc_spacing(double volts,double out[7],char *error,size_t error_size) {
  try {
    if(!out)throw std::runtime_error("Missing result");
    const auto r=corvcad::electrical_spacing(volts);
    for(int i=0;i<7;++i)out[i]=r[static_cast<size_t>(i)];
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_calc_regulator(double vref,double r1,double r2,double vout,double iadj,int solve_r2,double *result,
                      char *error,size_t error_size) {
  try {
    if(!result)throw std::runtime_error("Missing result");
    *result=solve_r2?corvcad::regulator_r2(vref,vout,r1,iadj):corvcad::regulator_vout(vref,r1,r2,iadj);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_calc_resistor(double ohms,int series,double *value,char *bands,size_t bands_size,char *error,size_t error_size) {
  try {
    if(!value||!bands)throw std::runtime_error("Missing result");
    *value=corvcad::nearest_eseries(ohms,series);
    std::string text;
    for(const auto &band:corvcad::resistor_colours(ohms,series))text+=(text.empty()?"":",")+band;
    copy(bands,bands_size,text);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
char *cc_project_libraries_json(const char *folder,char *error,size_t error_size) {
  try {
    if(!folder)throw std::runtime_error("Missing project folder");
    auto *result=owned_text(corvcad::project_libraries_json(folder));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_library_generate(int footprint,const char *spec,char *error,size_t error_size) {
  try {
    if(!spec)throw std::runtime_error("Missing part");
    auto *result=owned_text(corvcad::library_generate(footprint!=0,spec));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_library_item_text(const char *folder,int footprint,const char *library,const char *name,char *error,
                           size_t error_size) {
  try {
    if(!folder||!library||!name)throw std::runtime_error("Missing part");
    auto *result=owned_text(corvcad::library_item_text(folder,footprint!=0,library,name));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_library_edit(int footprint,const char *text,const char *op,char *error,size_t error_size) {
  try {
    if(!text||!op)throw std::runtime_error("Missing edit");
    auto *result=owned_text(corvcad::library_edit(footprint!=0,text,op));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_library_item_json(int footprint,const char *text,const char *base,char *error,size_t error_size) {
  try {
    if(!text)throw std::runtime_error("Missing part");
    auto *result=owned_text(corvcad::library_item_json(footprint!=0,text,base?base:""));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
int cc_save_library_item_text(const char *folder,int footprint,const char *library,const char *original,
                              const char *text,int replace,char *error,size_t error_size) {
  try {
    if(!folder||!library||!text)throw std::runtime_error("Missing part");
    corvcad::save_library_item_text(folder,footprint!=0,library,original?original:"",text,replace!=0);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_import_library_footprint(const char *folder,const char *library,const char *source,char *error,
                                size_t error_size) {
  try {
    if(!folder||!library||!source)throw std::runtime_error("Missing footprint");
    corvcad::import_library_footprint(folder,library,source);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_import_project_library(const char *folder,const char *source,char *error,size_t error_size) {
  try {
    if(!folder||!source)throw std::runtime_error("Missing library");
    corvcad::import_project_library(folder,source);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_create_project_library(const char *folder,int footprint,const char *name,char *error,size_t error_size) {
  try {
    if(!folder||!name)throw std::runtime_error("Missing library");
    corvcad::create_project_library(folder,footprint!=0,name);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_remove_project_library(const char *folder,int footprint,const char *name,char *error,size_t error_size) {
  try {
    if(!folder||!name)throw std::runtime_error("Missing library");
    corvcad::remove_project_library(folder,footprint!=0,name);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_delete_library_item(const char *folder,int footprint,const char *library,const char *name,char *error,
                           size_t error_size) {
  try {
    if(!folder||!library||!name)throw std::runtime_error("Missing part");
    corvcad::delete_library_item(folder,footprint!=0,library,name);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
char *cc_add_schematic_two_pin(const char *source,const char *project,
                                double x,double y,char *error,size_t error_size) {
  try {
    if(!source||!project)throw std::runtime_error("Missing schematic or project");
    auto *result=owned_text(corvcad::add_schematic_two_pin(source,project,x,y));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_schematic_wire(const char *source,double x0,double y0,
                            double x1,double y1,char *error,size_t error_size) {
  try {
    if(!source)throw std::runtime_error("Missing schematic");
    auto *result=owned_text(corvcad::add_schematic_wire(source,x0,y0,x1,y1));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
size_t cc_route_schematic_wire(const char *source,double x0,double y0,double x1,double y1,
                               double *xy,size_t capacity,char *error,size_t error_size) {
  try {
    if(!source)throw std::runtime_error("Missing schematic");
    const auto path=corvcad::route_schematic_wire(source,x0,y0,x1,y1);
    for(size_t i=0;i<path.size()&&i<capacity&&xy;++i){xy[2*i]=path[i].x;xy[2*i+1]=path[i].y;}
    copy(error,error_size,"");return path.size();
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
size_t cc_sch_route(const cc_schematic *s,double x0,double y0,double x1,double y1,
                    double *xy,size_t capacity,char *error,size_t error_size) {
  try {
    if(!s)throw std::runtime_error("Missing schematic");
    const auto path=corvcad::route_schematic_wire(s->value,x0,y0,x1,y1);
    for(size_t i=0;i<path.size()&&i<capacity&&xy;++i){xy[2*i]=path[i].x;xy[2*i+1]=path[i].y;}
    copy(error,error_size,"");return path.size();
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
char *cc_add_schematic_orthogonal_wire(const char *source,double x0,double y0,
                                       double x1,double y1,char *error,size_t error_size) {
  try {
    if(!source)throw std::runtime_error("Missing schematic");
    auto *result=owned_text(corvcad::add_schematic_orthogonal_wire(source,x0,y0,x1,y1));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_schematic_label(const char *source,const char *name,
                             double x,double y,char *error,size_t error_size) {
  try {
    if(!source||!name)throw std::runtime_error("Missing schematic or label");
    auto *result=owned_text(corvcad::add_schematic_label(source,name,x,y));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_two_pad(const char *source,const char *reference,
                           const char *value,double x,double y,
                           char *error,size_t error_size) {
  try {
    if(!source||!reference||!value)throw std::runtime_error("Missing board part data");
    auto *result=owned_text(corvcad::add_board_two_pad(source,reference,value,x,y));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_mounting_hole(const char *source,double x,double y,
                                  char *error,size_t error_size) {
  try {
    if(!source)throw std::runtime_error("Missing board");
    auto *result=owned_text(corvcad::add_board_mounting_hole(source,x,y));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_delete_schematic_item(const char *source,const char *uuid,
                                char *error,size_t error_size) {
  try {
    if(!source||!uuid)throw std::runtime_error("Missing schematic item");
    auto *result=owned_text(corvcad::delete_schematic_item(source,uuid));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_rename_schematic_symbol(const char *source,const char *uuid,
                                  const char *reference,char *error,size_t error_size) {
  try {
    if(!source||!uuid||!reference)throw std::runtime_error("Missing schematic reference");
    auto *result=owned_text(corvcad::rename_schematic_symbol(source,uuid,reference));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_rename_schematic_label(const char *source,const char *uuid,
                                 const char *name,char *error,size_t error_size) {
  try {
    if(!source||!uuid||!name)throw std::runtime_error("Missing schematic label");
    auto *result=owned_text(corvcad::rename_schematic_label(source,uuid,name));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_move_schematic_label(const char *source,const char *uuid,
                               double x,double y,char *error,size_t error_size) {
  try {
    if(!source||!uuid)throw std::runtime_error("Missing schematic label");
    auto *result=owned_text(corvcad::move_schematic_label(source,uuid,x,y));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_delete_board_footprint(const char *source,const char *reference,
                                 char *error,size_t error_size) {
  try {
    if(!source||!reference)throw std::runtime_error("Missing footprint reference");
    auto *result=owned_text(corvcad::delete_footprint(source,reference));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_rename_board_footprint(const char *source,const char *reference,
                                 const char *new_reference,char *error,size_t error_size) {
  try {
    if(!source||!reference||!new_reference)
      throw std::runtime_error("Missing footprint reference");
    auto *result=owned_text(corvcad::rename_footprint(source,reference,new_reference));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_rotate_board_footprint(const char *source,const char *reference,
                                char *error,size_t error_size) {
  try {
    if(!source||!reference)throw std::runtime_error("Missing footprint reference");
    auto *result=owned_text(corvcad::rotate_footprint(source,reference));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_track(const char *source,double x0,double y0,double x1,double y1,
                         const char *layer,const char *net,double width,char *error,size_t error_size) {
  try {
    if(!source||!layer||!net)throw std::runtime_error("Missing track data");
    auto *result=owned_text(corvcad::add_board_track(source,x0,y0,x1,y1,layer,net,width>0?width:0.25));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_via(const char *source,double x,double y,const char *net,double size,double drill,
                       char *error,size_t error_size) {
  try {
    if(!source||!net)throw std::runtime_error("Missing via data");
    auto *result=owned_text(corvcad::add_board_via(source,x,y,net,size>0?size:0.6,drill>0?drill:0.3));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_delete_board_copper(const char *source,const char *uuid,char *error,size_t error_size) {
  try {
    if(!source||!uuid)throw std::runtime_error("Missing track or via UUID");
    auto *result=owned_text(corvcad::delete_board_copper(source,uuid));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_move_board_drawing(const char *source,const char *uuid,double dx,double dy,char *error,size_t error_size) {
  try {
    if(!source||!uuid)throw std::runtime_error("Missing board or item UUID");
    auto *result=owned_text(corvcad::move_board_drawing(source,uuid,dx,dy));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_set_board_text(const char *source,const char *uuid,const char *text,double size,char *error,size_t error_size) {
  try {
    if(!source||!uuid||!text)throw std::runtime_error("Missing board, item UUID or text");
    auto *result=owned_text(corvcad::set_board_text(source,uuid,text,size));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_set_board_outline(const char *source,const double *xy,size_t count,char *error,size_t error_size) {
  try {
    if(!source)throw std::runtime_error("Missing board");
    auto *result=owned_text(corvcad::set_board_outline(source,shape_points(xy,count)));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_zone(const char *source,const double *xy,size_t count,const char *layer,const char *net,
                        char *error,size_t error_size) {
  try {
    if(!source||!layer||!net)throw std::runtime_error("Missing zone data");
    auto *result=owned_text(corvcad::add_board_zone(source,shape_points(xy,count),layer,net));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_line(const char *source,double x0,double y0,double x1,double y1,const char *layer,
                        double width,char *error,size_t error_size) {
  try {
    if(!source||!layer)throw std::runtime_error("Missing line data");
    auto *result=owned_text(corvcad::add_board_line(source,x0,y0,x1,y1,layer,width>0?width:0.15));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_add_board_text(const char *source,const char *text,double x,double y,const char *layer,double size,
                        char *error,size_t error_size) {
  try {
    if(!source||!text||!layer)throw std::runtime_error("Missing text data");
    auto *result=owned_text(corvcad::add_board_text(source,text,x,y,layer,size>0?size:1));
    copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
char *cc_update_board_from_schematic(const char *board,const char *schematic,char *report,
                                     size_t report_size,char *error,size_t error_size) {
  copy(report,report_size,"");
  try {
    if(!board||!schematic)throw std::runtime_error("Missing board or schematic");
    const auto update=corvcad::update_board_from_schematic(board,schematic);
    auto list=[](const std::vector<std::string> &items) {
      std::string out;
      for(const auto &item:items)out+=(out.empty()?"":", ")+item;
      return out;
    };
    std::string text;
    if(!update.added.empty())text+="Added "+list(update.added)+".";
    if(!update.skipped.empty())text+=(text.empty()?"":" ")+std::string("Not updated: ")+list(update.skipped)+".";
    auto *result=owned_text(update.board);
    copy(report,report_size,text);copy(error,error_size,"");return result;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
void cc_free_text(char *value) {std::free(value);}
cc_schematic *cc_schematic_parse(const char *utf8,char *error,size_t error_size) {
  try {
    if(!utf8)throw std::runtime_error("Missing schematic data");
    auto *s=new cc_schematic{corvcad::parse_kicad_schematic(utf8),{}};
    s->nets=corvcad::schematic_nets(s->value);
    copy(error,error_size,"");return s;
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
void cc_schematic_free(cc_schematic *s) {delete s;}
size_t cc_sch_symbol_count(const cc_schematic *s) {return s?s->value.symbols.size():0;}
cc_sch_symbol cc_sch_symbol_at(const cc_schematic *s,size_t i) {
  cc_sch_symbol result{};if(!s||i>=s->value.symbols.size())return result;
  const auto &v=s->value.symbols[i];result.x=v.position.x;result.y=v.position.y;
  result.rotation=v.rotation;
  copy(result.uuid,sizeof(result.uuid),v.uuid);
  copy(result.reference,sizeof(result.reference),v.reference);
  copy(result.value,sizeof(result.value),v.value);
  copy(result.footprint,sizeof(result.footprint),v.footprint);
  copy(result.library_id,sizeof(result.library_id),v.library_id);
  return result;
}
size_t cc_sch_symbol_graphic_count(const cc_schematic *s,size_t symbol) {
  return s&&symbol<s->value.symbols.size()?s->value.symbols[symbol].graphics.size():0;
}
cc_sch_graphic cc_sch_symbol_graphic_at(const cc_schematic *s,size_t symbol,size_t i) {
  cc_sch_graphic result{};if(i>=cc_sch_symbol_graphic_count(s,symbol))return result;
  const auto &g=s->value.symbols[symbol].graphics[i];
  result.closed=g.closed;result.fill=g.fill;result.point_count=g.points.size();return result;
}
void cc_sch_symbol_graphic_point(const cc_schematic *s,size_t symbol,size_t graphic,size_t i,double *x,double *y) {
  if(graphic>=cc_sch_symbol_graphic_count(s,symbol))return;
  const auto &points=s->value.symbols[symbol].graphics[graphic].points;
  if(i>=points.size())return;
  if(x)*x=points[i].x;if(y)*y=points[i].y;
}
size_t cc_sch_symbol_pin_count(const cc_schematic *s,size_t symbol) {
  return s&&symbol<s->value.symbols.size()?s->value.symbols[symbol].pins.size():0;
}
cc_sch_symbol_text cc_sch_symbol_text_at(const cc_schematic *s,size_t symbol) {
  cc_sch_symbol_text result{};if(!s||symbol>=s->value.symbols.size())return result;
  const auto &v=s->value.symbols[symbol];
  result.reference_x=v.reference_field.position.x;result.reference_y=v.reference_field.position.y;
  result.reference_visible=v.reference_field.visible;result.reference_vertical=v.reference_field.vertical;
  result.value_x=v.value_field.position.x;result.value_y=v.value_field.position.y;
  result.value_visible=v.value_field.visible;result.value_vertical=v.value_field.vertical;
  result.pin_name_offset=v.pin_name_offset;
  result.pin_names_visible=v.pin_names_visible;result.pin_numbers_visible=v.pin_numbers_visible;
  return result;
}
cc_sch_symbol_pin cc_sch_symbol_pin_at(const cc_schematic *s,size_t symbol,size_t i) {
  cc_sch_symbol_pin result{};if(i>=cc_sch_symbol_pin_count(s,symbol))return result;
  const auto &v=s->value.symbols[symbol];
  result.x=v.pins[i].x;result.y=v.pins[i].y;result.end_x=v.pin_ends[i].x;result.end_y=v.pin_ends[i].y;
  result.visible=v.pin_visible[i];
  copy(result.number,sizeof(result.number),v.pin_numbers[i]);
  copy(result.name,sizeof(result.name),v.pin_names[i]);
  return result;
}
size_t cc_sch_wire_count(const cc_schematic *s) {return s?s->value.wires.size():0;}
cc_sch_wire cc_sch_wire_at(const cc_schematic *s,size_t i) {
  cc_sch_wire result{};if(!s||i>=s->value.wires.size())return result;
  const auto &v=s->value.wires[i];result.x0=v.start.x;result.y0=v.start.y;
  result.x1=v.end.x;result.y1=v.end.y;copy(result.uuid,sizeof(result.uuid),v.uuid);
  return result;
}
size_t cc_sch_junction_count(const cc_schematic *s) {return s?s->value.junctions.size():0;}
cc_sch_junction cc_sch_junction_at(const cc_schematic *s,size_t i) {
  cc_sch_junction result{};if(!s||i>=s->value.junctions.size())return result;
  const auto &v=s->value.junctions[i];result.x=v.position.x;result.y=v.position.y;
  copy(result.uuid,sizeof(result.uuid),v.uuid);return result;
}
cc_sch_page cc_sch_page_info(const cc_schematic *s) {
  cc_sch_page result{};if(!s)return result;
  const auto &v=s->value;result.width=v.paper.x;result.height=v.paper.y;
  copy(result.title,sizeof(result.title),v.title);copy(result.date,sizeof(result.date),v.date);
  copy(result.revision,sizeof(result.revision),v.revision);copy(result.company,sizeof(result.company),v.company);
  return result;
}
size_t cc_sch_sheet_count(const cc_schematic *s) {return s?s->value.sheets.size():0;}
cc_sch_sheet cc_sch_sheet_at(const cc_schematic *s,size_t i) {
  cc_sch_sheet result{};if(!s||i>=s->value.sheets.size())return result;
  const auto &v=s->value.sheets[i];result.x=v.position.x;result.y=v.position.y;
  result.width=v.size.x;result.height=v.size.y;copy(result.uuid,sizeof(result.uuid),v.uuid);
  copy(result.name,sizeof(result.name),v.name);copy(result.file,sizeof(result.file),v.file);return result;
}
size_t cc_sch_sheet_pin_count(const cc_schematic *s,size_t sheet) {
  return s&&sheet<s->value.sheets.size()?s->value.sheets[sheet].pins.size():0;
}
cc_sch_label cc_sch_sheet_pin_at(const cc_schematic *s,size_t sheet,size_t i) {
  cc_sch_label result{};if(i>=cc_sch_sheet_pin_count(s,sheet))return result;
  const auto &v=s->value.sheets[sheet].pins[i];result.x=v.position.x;result.y=v.position.y;
  copy(result.uuid,sizeof(result.uuid),v.uuid);
  copy(result.text,sizeof(result.text),v.name);copy(result.kind,sizeof(result.kind),v.shape);return result;
}
namespace {
cc_sch_wire line_at(const std::vector<corvcad::SchWire> &lines,size_t i) {
  cc_sch_wire result{};if(i>=lines.size())return result;
  const auto &v=lines[i];result.x0=v.start.x;result.y0=v.start.y;
  result.x1=v.end.x;result.y1=v.end.y;copy(result.uuid,sizeof(result.uuid),v.uuid);
  return result;
}
}
size_t cc_sch_bus_count(const cc_schematic *s) {return s?s->value.buses.size():0;}
cc_sch_wire cc_sch_bus_at(const cc_schematic *s,size_t i) {return s?line_at(s->value.buses,i):cc_sch_wire{};}
size_t cc_sch_bus_entry_count(const cc_schematic *s) {return s?s->value.bus_entries.size():0;}
cc_sch_wire cc_sch_bus_entry_at(const cc_schematic *s,size_t i) {return s?line_at(s->value.bus_entries,i):cc_sch_wire{};}
size_t cc_sch_net_count(const cc_schematic *s) {return s?s->nets.size():0;}
cc_sch_net cc_sch_net_at(const cc_schematic *s,size_t i) {
  cc_sch_net result{};if(!s||i>=s->nets.size())return result;
  copy(result.name,sizeof(result.name),s->nets[i].name);result.pin_count=s->nets[i].pins.size();return result;
}
cc_sch_pin cc_sch_net_pin_at(const cc_schematic *s,size_t net,size_t i) {
  cc_sch_pin result{};if(!s||net>=s->nets.size()||i>=s->nets[net].pins.size())return result;
  const auto &v=s->nets[net].pins[i];result.symbol=v.symbol;result.x=v.position.x;result.y=v.position.y;
  copy(result.number,sizeof(result.number),v.number);return result;
}
namespace {
long net_of(const cc_schematic *s,size_t item,std::vector<size_t> corvcad::SchNet::*members) {
  if(!s)return -1;
  for(size_t n=0;n<s->nets.size();++n){
    const auto &list=s->nets[n].*members;
    if(std::find(list.begin(),list.end(),item)!=list.end())return static_cast<long>(n);
  }
  return -1;
}
}
long cc_sch_wire_net(const cc_schematic *s,size_t wire) {return net_of(s,wire,&corvcad::SchNet::wires);}
long cc_sch_label_net(const cc_schematic *s,size_t label) {return net_of(s,label,&corvcad::SchNet::labels);}
size_t cc_sch_label_count(const cc_schematic *s) {return s?s->value.labels.size():0;}
cc_sch_label cc_sch_label_at(const cc_schematic *s,size_t i) {
  cc_sch_label result{};if(!s||i>=s->value.labels.size())return result;
  const auto &v=s->value.labels[i];result.x=v.position.x;result.y=v.position.y;
  copy(result.uuid,sizeof(result.uuid),v.uuid);
  copy(result.text,sizeof(result.text),v.text);copy(result.kind,sizeof(result.kind),v.kind);return result;
}
size_t cc_sch_text_count(const cc_schematic *s) {return s?s->value.texts.size():0;}
cc_sch_label cc_sch_text_at(const cc_schematic *s,size_t i) {
cc_sch_label result{};if(!s||i>=s->value.texts.size())return result;
const auto &v=s->value.texts[i];result.x=v.position.x;result.y=v.position.y;
copy(result.uuid,sizeof(result.uuid),v.uuid);
copy(result.text,sizeof(result.text),v.text);copy(result.kind,sizeof(result.kind),v.kind);return result;
}
size_t cc_sch_note_line_count(const cc_schematic *s) {return s?s->value.note_lines.size():0;}
cc_sch_wire cc_sch_note_line_at(const cc_schematic *s,size_t i) {
cc_sch_wire result{};if(!s||i>=s->value.note_lines.size())return result;
const auto &v=s->value.note_lines[i];result.x0=v.start.x;result.y0=v.start.y;
result.x1=v.end.x;result.y1=v.end.y;copy(result.uuid,sizeof(result.uuid),v.uuid);
return result;
}
size_t cc_sch_no_connect_count(const cc_schematic *s) {return s?s->value.no_connects.size():0;}
cc_sch_junction cc_sch_no_connect_at(const cc_schematic *s,size_t i) {
  cc_sch_junction result{};if(!s||i>=s->value.no_connects.size())return result;
  const auto &v=s->value.no_connects[i];result.x=v.position.x;result.y=v.position.y;
  copy(result.uuid,sizeof(result.uuid),v.uuid);return result;
}
int cc_move_schematic_symbol_file(const char *utf8,const char *uuid,double x,double y,
                                   const char *path,char *error,size_t error_size) {
  try {
    if(!utf8||!uuid||!path)throw std::runtime_error("Missing schematic, UUID or output path");
    const auto edited=corvcad::move_schematic_symbol(utf8,uuid,x,y);
    std::ofstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open schematic output");
    file<<edited;
    if(!file)throw std::runtime_error("Failed to write schematic");
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_set_schematic_property_file(const char *utf8,const char *uuid,const char *property,
                                    const char *value,const char *path,
                                    char *error,size_t error_size) {
  try {
    if(!utf8||!uuid||!property||!value||!path)
      throw std::runtime_error("Missing schematic property edit input");
    const auto edited=corvcad::set_schematic_property(utf8,uuid,property,value);
    std::ofstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open schematic output");
    file<<edited;
    if(!file)throw std::runtime_error("Failed to write schematic");
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
cc_board *cc_board_parse(const char *utf8,char *error,size_t error_size) {
  try { if(!utf8)throw std::runtime_error("Missing board data");
    auto *b=new cc_board{corvcad::parse_kicad_board(utf8), corvcad::board_thickness(utf8)}; copy(error,error_size,"");return b;
  } catch(const std::exception &e) {copy(error,error_size,e.what());return nullptr;}
}
void cc_board_free(cc_board *b) {delete b;}
double cc_board_thickness(const cc_board *b) {return b ? b->thickness : 0;}
void cc_board_bounds(const cc_board *b,double *min_x,double *min_y,double *max_x,double *max_y) {
  if(!b)return;
  if(min_x)*min_x=b->value.min_x;if(min_y)*min_y=b->value.min_y;
  if(max_x)*max_x=b->value.max_x;if(max_y)*max_y=b->value.max_y;
}
size_t cc_outline_count(const cc_board *b) {return b?b->value.outline.size():0;}
void cc_outline_point(const cc_board *b,size_t i,double *x,double *y) {
  if(!b||i>=b->value.outline.size())return;
  if(x)*x=b->value.outline[i].x;if(y)*y=b->value.outline[i].y;
}
size_t cc_hole_count(const cc_board *b) {return b?b->value.mounting_holes.size():0;}
cc_hole cc_hole_at(const cc_board *b,size_t i) {
  cc_hole result{};if(!b||i>=b->value.mounting_holes.size())return result;
  const auto &h=b->value.mounting_holes[i];result.x=h.center.x;result.y=h.center.y;result.diameter=h.diameter;
  copy(result.reference,sizeof(result.reference),h.reference);return result;
}
size_t cc_placement_count(const cc_board *b) {return b?b->value.placements.size():0;}
cc_placement cc_placement_at(const cc_board *b,size_t i) {
  cc_placement result{};if(!b||i>=b->value.placements.size())return result;
  const auto &p=b->value.placements[i];result.x=p.center.x;result.y=p.center.y;result.rotation=p.rotation;
  copy(result.reference,sizeof(result.reference),p.reference);copy(result.value,sizeof(result.value),p.value);
  result.back=p.back;copy(result.model,sizeof(result.model),p.model);
  for(int k=0;k<3;++k){result.offset[k]=p.offset[k];result.rotate[k]=p.rotate[k];result.scale[k]=p.scale[k];}
  for(int k=0;k<4;++k)result.extent[k]=p.extent[k];
  return result;
}
size_t cc_net_count(const cc_board *b) {return b?b->value.nets.size():0;}
cc_net cc_net_at(const cc_board *b,size_t i) {
  cc_net result{};if(!b||i>=b->value.nets.size())return result;
  const auto &n=b->value.nets[i];result.code=n.code;copy(result.name,sizeof(result.name),n.name);return result;
}
struct cc_net_flow { corvcad::NetFlow value; size_t pins = 0; };
cc_net_flow *cc_net_flow_make(const cc_board *b,int net,const size_t *pins,size_t pin_count,char *error,size_t error_size) {
  try {
    if(!b)throw std::runtime_error("No board loaded");
    auto flow=std::make_unique<cc_net_flow>();
    flow->value=corvcad::net_flow(b->value,net,std::vector<size_t>(pins,pins+pin_count));
    flow->pins=pin_count;
    copy(error,error_size,"");
    return flow.release();
  }catch(const std::exception &e){copy(error,error_size,e.what());return nullptr;}
}
void cc_net_flow_free(cc_net_flow *flow) {delete flow;}
size_t cc_net_flow_edge_count(const cc_net_flow *flow) {return flow?flow->value.edges.size():0;}
cc_flow_edge cc_net_flow_edge_at(const cc_net_flow *flow,size_t i) {
  cc_flow_edge result{};if(!flow||i>=flow->value.edges.size())return result;
  const auto &e=flow->value.edges[i];
  result.x0=e.a.x;result.y0=e.a.y;result.x1=e.b.x;result.y1=e.b.y;result.width=e.width;
  copy(result.layer,sizeof(result.layer),e.layer);return result;
}
double cc_net_flow_unit(const cc_net_flow *flow,size_t edge,size_t pin) {
  if(!flow||edge>=flow->value.edges.size()||pin>=flow->pins)return 0;
  return flow->value.unit[edge*flow->pins+pin];
}
size_t cc_track_count(const cc_board *b) {return b?b->value.tracks.size():0;}
cc_track cc_track_at(const cc_board *b,size_t i) {
  cc_track result{};if(!b||i>=b->value.tracks.size())return result;
  const auto &t=b->value.tracks[i];
  result.x0=t.start.x;result.y0=t.start.y;result.x1=t.end.x;result.y1=t.end.y;
  result.width=t.width;result.net=t.net;copy(result.layer,sizeof(result.layer),t.layer);
  copy(result.uuid,sizeof(result.uuid),t.uuid);return result;
}
size_t cc_via_count(const cc_board *b) {return b?b->value.vias.size():0;}
size_t cc_zone_count(const cc_board *b) {return b?b->value.zones.size():0;}
cc_zone cc_zone_at(const cc_board *b,size_t i) {
  cc_zone result{};if(!b||i>=b->value.zones.size())return result;
  const auto &z=b->value.zones[i];result.net=z.net;result.filled=z.filled?1:0;
  copy(result.layer,sizeof(result.layer),z.layer);result.point_count=z.points.size();
  copy(result.uuid,sizeof(result.uuid),z.uuid);
  result.priority=z.settings.priority;result.connection=z.settings.connection;result.hatched=z.settings.hatched?1:0;result.teardrop=z.settings.teardrop?1:0;
  result.clearance=z.settings.clearance;result.min_thickness=z.settings.min_thickness;
  result.thermal_gap=z.settings.thermal_gap;result.spoke_width=z.settings.spoke_width;return result;
}
size_t cc_graphic_count(const cc_board *b) {return b?b->value.graphics.size():0;}
cc_track cc_graphic_at(const cc_board *b,size_t i) {
  cc_track result{};if(!b||i>=b->value.graphics.size())return result;
  const auto &t=b->value.graphics[i];
  result.x0=t.start.x;result.y0=t.start.y;result.x1=t.end.x;result.y1=t.end.y;result.width=t.width;
  copy(result.layer,sizeof(result.layer),t.layer);copy(result.uuid,sizeof(result.uuid),t.uuid);return result;
}
size_t cc_text_count(const cc_board *b) {return b?b->value.texts.size():0;}
cc_board_text cc_text_at(const cc_board *b,size_t i) {
  cc_board_text result{};if(!b||i>=b->value.texts.size())return result;
  const auto &t=b->value.texts[i];
  result.x=t.position.x;result.y=t.position.y;result.size=t.size;result.rotation=t.rotation;
  copy(result.layer,sizeof(result.layer),t.layer);copy(result.uuid,sizeof(result.uuid),t.uuid);
  copy(result.text,sizeof(result.text),t.text);return result;
}
void cc_zone_point(const cc_board *b,size_t zone,size_t i,double *x,double *y) {
  if(!b||zone>=b->value.zones.size()||i>=b->value.zones[zone].points.size())return;
  const auto &p=b->value.zones[zone].points[i];if(x)*x=p.x;if(y)*y=p.y;
}
cc_via cc_via_at(const cc_board *b,size_t i) {
  cc_via result{};if(!b||i>=b->value.vias.size())return result;
  const auto &v=b->value.vias[i];result.x=v.center.x;result.y=v.center.y;
  result.size=v.size;result.drill=v.drill;result.net=v.net;copy(result.uuid,sizeof(result.uuid),v.uuid);return result;
}
size_t cc_pad_count(const cc_board *b) {return b?b->value.pads.size():0;}
cc_pad cc_pad_at(const cc_board *b,size_t i) {
  cc_pad result{};if(!b||i>=b->value.pads.size())return result;
  const auto &p=b->value.pads[i];result.x=p.center.x;result.y=p.center.y;
  result.size_x=p.size_x;result.size_y=p.size_y;result.rotation=p.rotation;result.net=p.net;
  copy(result.reference,sizeof(result.reference),p.reference);
  copy(result.number,sizeof(result.number),p.number);copy(result.type,sizeof(result.type),p.type);
  copy(result.shape,sizeof(result.shape),p.shape);copy(result.layers,sizeof(result.layers),p.layers);
  return result;
}
int cc_move_footprint_file(const char *utf8,const char *reference,double x,double y,
                           double rotation,const char *path,char *error,size_t error_size) {
  try {
    if(!utf8||!reference||!path)throw std::runtime_error("Missing board, reference or output path");
    auto edited=corvcad::move_footprint(utf8,reference,x,y,rotation);
    std::ofstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open KiCad board output");
    file<<edited;
    if(!file)throw std::runtime_error("Failed to write KiCad board");
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
cc_settings cc_default_settings(void) {return {3,2,2,2,4,6,2,0.25};}
size_t cc_model_triangles(const cc_board *b,cc_settings settings,int kind,cc_triangle *output,size_t capacity,char *error,size_t error_size) {
  try {
    if(!b)throw std::runtime_error("No board loaded");
    auto model=corvcad::make_model(b->value,convert(settings));
    const std::vector<corvcad::Triangle> *part=nullptr;
    switch(kind) {
      case 0: part=&model.enclosure;break;
      case 1: part=&model.board;break;
      case 2: part=&model.standoffs;break;
      case 3: part=&model.floor;break;
      case 4: part=&model.walls;break;
      case 5: part=&model.rim;break;
      case 6: part=&model.lid;break;
      default: throw std::runtime_error("Unknown mesh kind");
    }
    if(output)for(size_t i=0;i<std::min(capacity,part->size());++i) {
      const auto &t=(*part)[i];output[i]={t.a[0],t.a[1],t.a[2],t.b[0],t.b[1],t.b[2],t.c[0],t.c[1],t.c[2]};
    }
    copy(error,error_size,"");return part->size();
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_export_stl(const cc_board *b,cc_settings settings,const char *path,char *error,size_t error_size) {
  try {if(!b||!path)throw std::runtime_error("Missing board or output path");
    corvcad::write_stl(corvcad::make_model(b->value,convert(settings)),path);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_export_lid_stl(const cc_board *b,cc_settings settings,const char *path,char *error,size_t error_size) {
  try {if(!b||!path)throw std::runtime_error("Missing board or output path");
    corvcad::write_lid_stl(corvcad::make_model(b->value,convert(settings)),path);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
int cc_step_available(void) {return corvcad::step_available()?1:0;}
int cc_export_step(const cc_board *b,cc_settings settings,const char *path,char *error,size_t error_size) {
  try {if(!b||!path)throw std::runtime_error("Missing board or output path");
    corvcad::write_step(b->value,convert(settings),path);
    copy(error,error_size,"");return 1;
  }catch(const std::exception &e){copy(error,error_size,e.what());return 0;}
}
}

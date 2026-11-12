#include "corvcad/engine.hpp"
#include "corvcad/c_api.h"
#include "corvcad/board_native_c_api.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef CORVCAD_FIXTURE
#define CORVCAD_FIXTURE "engine/tests/fixtures/demo.kicad_pcb"
#endif

void require(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}

int main() {
  std::ifstream in(CORVCAD_FIXTURE);
  std::string source((std::istreambuf_iterator<char>(in)),{});
  auto b=corvcad::parse_kicad_board(source);
  require(b.outline.size()==4,"outline");
  {
    auto z=corvcad::parse_kicad_board(
        "(kicad_pcb (net 1 \"GND\") (gr_rect (start 0 0) (end 10 10) (layer \"Edge.Cuts\"))"
        " (zone (net 1) (net_name \"GND\") (layer \"F.Cu\") (polygon (pts (xy 0 0) (xy 10 0) (xy 10 10)))"
        "  (filled_polygon (layer \"F.Cu\") (pts (xy 1 1) (xy 9 1) (xy 9 9) (xy 1 9)))))");
    require(z.zones.size()==2 && !z.zones[0].filled && z.zones[0].points.size()==3 && z.zones[0].net==1 &&
            z.zones[1].filled && z.zones[1].layer=="F.Cu" && z.zones[1].points.size()==4,"zone outline and fill");
  }
  char native_error[128]="";
  require(!cc_board_native_backend() && !cc_board_native_drc(CORVCAD_FIXTURE,0,native_error,sizeof native_error) && *native_error,"compatible edition has no native board code");
  require(!cc_board_native_fab_outputs(CORVCAD_FIXTURE,".",0,native_error,sizeof native_error),"compatible edition has no fabrication outputs");
  char *project_text = reinterpret_cast<char*>(1);
  require(!cc_board_native_plot(CORVCAD_FIXTURE,".",0,native_error,sizeof native_error) &&
          !cc_board_native_board_setup(CORVCAD_FIXTURE,native_error,sizeof native_error) &&
          !cc_board_native_apply_board_setup(CORVCAD_FIXTURE,"layers\t2",&project_text,native_error,sizeof native_error) &&
          !project_text,"compatible edition has no plot or Board Setup");
  require(!cc_board_native_route_start(CORVCAD_FIXTURE,0,0,"F.Cu",2,0,0,native_error,sizeof native_error) &&
          !cc_board_native_drag_start(CORVCAD_FIXTURE,0,0,2,native_error,sizeof native_error) &&
          cc_board_native_route_fix(0,0,native_error,sizeof native_error)==-1 && !cc_board_native_route_toggle_via(),
          "compatible edition has no router");
  require(b.max_x-b.min_x==50 && b.max_y-b.min_y==30,"bounds");
  require(b.mounting_holes.size()==1 && b.mounting_holes[0].diameter==3.2,"mounting hole");
  require(b.placements.size()==3 &&
          std::any_of(b.placements.begin(),b.placements.end(),
                      [](const auto &p){return p.reference=="R1";}),"placements");
  {
    const auto r1=std::find_if(b.placements.begin(),b.placements.end(),[](const auto &p){return p.reference=="R1";});
    require(r1->model=="${KICAD10_3DMODEL_DIR}/Resistor_SMD.3dshapes/R_0603_1608Metric.step" && !r1->back &&
            r1->scale[0]==1 && r1->offset[2]==0 && r1->extent[0]<-1.2 && r1->extent[2]>1.2,"footprint 3D model and pad extent");
  }
  require(b.nets.size()==1 && b.nets[0].name=="GND","KiCad 10 named nets");
  require(b.tracks.size()==2 && b.tracks[0].net==1 && b.tracks[1].net==1,"tracks");
  require(b.vias.size()==1 && b.vias[0].drill==0.4 && b.vias[0].net==1,"vias");
  require(b.pads.size()==4,"pads");
  const auto r1pad=std::find_if(b.pads.begin(),b.pads.end(),
                  [](const auto &pad){return pad.reference=="R1"&&pad.number=="1";});
  require(r1pad!=b.pads.end() && r1pad->net==1,"KiCad 10 pad net");
  require(r1pad->shape=="roundrect" && r1pad->layers=="F.Cu,F.Mask,F.Paste",
          "pad shape and layers");
  require(std::hypot(r1pad->center.x-b.tracks[0].start.x,
                     r1pad->center.y-b.tracks[0].start.y)<1e-6,
          "KiCad footprint rotation and track attachment");
  const auto changed=corvcad::move_footprint(source,"R1",24.5,16,90);
  require(changed.find("(at 24.5 16 90)")!=std::string::npos,"KiCad footprint position rewrite");
  auto expected=source;
  const auto old_position=expected.find("(at 32 30 90)");
  require(old_position!=std::string::npos,"original footprint position");
  expected.replace(old_position,std::string("(at 32 30 90)").size(),"(at 24.5 16 90)");
  require(changed==expected,"preserve unrelated KiCad source exactly");
  auto moved=corvcad::parse_kicad_board(changed);
  const auto moved_r1=std::find_if(moved.placements.begin(),moved.placements.end(),
                      [](const auto &p){return p.reference=="R1";});
  require(moved_r1!=moved.placements.end() &&
          moved_r1->center.x==24.5 && moved_r1->rotation==90,
          "moved footprint parse");
  require(moved.tracks.size()==b.tracks.size() && moved.vias.size()==b.vias.size(),
          "preserve other KiCad data");
  bool missing_reference=false;
  try {corvcad::move_footprint(source,"R99",1,2,0);}catch(...) {missing_reference=true;}
  require(missing_reference,"reject unknown footprint");
  auto m=corvcad::make_model(b,{});
  require(m.enclosure.size()==28 && !m.board.empty() && m.standoffs.size()==128,"meshes");
  require(m.floor.size()==4 && m.walls.size()==16 && m.rim.size()==8 && m.lid.size()==12,"parts");
  auto output=std::filesystem::temp_directory_path()/"corvcad-test.stl";
  corvcad::write_stl(m,output.string());
  require(std::filesystem::file_size(output)>1000,"STL export");
  std::filesystem::remove(output);
  corvcad::write_lid_stl(m,output.string());
  require(std::filesystem::file_size(output)>1000,"lid STL export");
  std::filesystem::remove(output);
  char error[256]{};
  auto *handle=cc_board_parse(source.c_str(),error,sizeof(error));
  require(handle && cc_hole_count(handle)==1 && cc_placement_count(handle)==3,"C API import");
  require(std::abs(cc_board_thickness(handle)-1.6)<1e-9,"C API actual board thickness");
  require(cc_net_count(handle)==1 && cc_track_count(handle)==2 &&
          cc_via_count(handle)==1 && cc_pad_count(handle)==4,"C API electronics");
  auto edited_path=std::filesystem::temp_directory_path()/"corvcad-edited.kicad_pcb";
  require(cc_move_footprint_file(source.c_str(),"R1",24.5,16,90,
                                 edited_path.string().c_str(),error,sizeof(error))==1,
          "C API KiCad board edit");
  require(std::filesystem::file_size(edited_path)>100,"KiCad board output");
  if(const char *verify_path=std::getenv("CORVCAD_TEST_EDITED_OUTPUT"))
    std::filesystem::copy_file(edited_path,verify_path,
                               std::filesystem::copy_options::overwrite_existing);
  std::filesystem::remove(edited_path);
  auto settings=cc_default_settings();
  require(cc_model_triangles(handle,settings,0,nullptr,0,error,sizeof(error))==28,"C API mesh");
  require(cc_model_triangles(handle,settings,6,nullptr,0,error,sizeof(error))==12,"C API lid");
  settings.wall=0;
  require(cc_model_triangles(handle,settings,0,nullptr,0,error,sizeof(error))==0 &&
          std::string(error).find("positive")!=std::string::npos,"invalid dimensions");
  cc_board_free(handle);
  auto rotated=corvcad::parse_kicad_board(
    "(kicad_pcb "
    "(gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\")) "
    "(footprint \"MountingHole:M3\" (at 10 5 90) "
    "(property \"Reference\" \"H2\") "
    "(pad \"\" np_thru_hole circle (at 1 0) (drill 3.2))))");
  require(rotated.outline.size()==4,"rectangle outline");
  require(std::abs(rotated.mounting_holes[0].center.x-10)<1e-8 &&
          std::abs(rotated.mounting_holes[0].center.y-4)<1e-8,"rotated hole");
  auto dsub=corvcad::parse_kicad_board(
    "(kicad_pcb (gr_rect (start 0 0) (end 40 20) (layer \"Edge.Cuts\")) "
    "(footprint \"kit:DSUB-9_Female_Horizontal_MountingHolesOffset9.12mm\" (at 10 10) "
    "(property \"Reference\" \"P1\") (pad \"1\" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 1))) "
    "(footprint \"Local:MountingHole_3.2mm_M3\" (at 30 10) "
    "(property \"Reference\" \"H1\") (pad \"\" np_thru_hole circle (at 0 0) (drill 3.2))))");
  require(dsub.mounting_holes.size()==1 && dsub.mounting_holes[0].reference=="H1",
          "a connector named ...MountingHolesOffset... is not a mounting hole");
  auto legacy=corvcad::parse_kicad_board(
    "(kicad_pcb (net 0 \"\") (net 1 \"GND\") "
    "(gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\")) "
    "(segment (start 1 1) (end 2 2) (width 0.2) (layer \"F.Cu\") (net 1)))");
  require(legacy.nets.size()==2 && legacy.tracks[0].net==1,"legacy numeric nets");
  auto arced=corvcad::parse_kicad_board(
    "(kicad_pcb (net 0 \"\") (net 1 \"GND\") "
    "(gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\")) "
    "(arc (start 2 5) (mid 5 2) (end 8 5) (width 0.25) (layer \"F.Cu\") (net 1) (uuid \"a1\")))");
  require(arced.tracks.size()==16 && arced.tracks[0].start.x==2 && arced.tracks[15].end.x==8 &&
          arced.tracks[15].end.y==5 && std::abs(arced.tracks[7].end.y-2)<1e-9 && arced.tracks[3].uuid=="a1" &&
          arced.tracks[0].net==1,"track arcs as chords through mid, sharing the UUID");
  require(corvcad::delete_board_copper(
            "(kicad_pcb (gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\")) (arc (start 2 5) (mid 5 2) (end 8 5) (width 0.25) (layer \"F.Cu\") (uuid \"a1\")))", "a1")
            .find("(arc")==std::string::npos,"a track arc deletes by UUID");
  auto curved=corvcad::parse_kicad_board(
    "(kicad_pcb "
    "(gr_arc (start 0 0) (mid 5 -5) (end 10 0) (layer \"Edge.Cuts\")) "
    "(gr_line (start 10 0) (end 10 10) (layer \"Edge.Cuts\")) "
    "(gr_line (start 10 10) (end 0 10) (layer \"Edge.Cuts\")) "
    "(gr_line (start 0 10) (end 0 0) (layer \"Edge.Cuts\")))");
  require(curved.outline.size()>10 && curved.min_y < -4.9,"arc outline");
  auto slotted=corvcad::parse_kicad_board(
    "(kicad_pcb "
    "(gr_line (start 10 2) (end 12 2) (layer \"Edge.Cuts\")) "  // an open milled slot, before the outline
    "(gr_line (start 12 2) (end 12 4) (layer \"Edge.Cuts\")) "
    "(gr_rect (start 3 3) (end 5 5) (layer \"Edge.Cuts\")) "    // an inner cutout
    "(gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\")))");
  require(slotted.outline.size()==4 && slotted.max_x==20 && slotted.max_y==10,
          "the largest closed Edge.Cuts loop is the outline; slots and cutouts are not");
  {
    // Current in copper: a line A-B-C, parallel widths, a T off a track's middle, a zone, a via.
    using corvcad::Board; using corvcad::Pad; using corvcad::Point; using corvcad::Track;
    const auto near=[](double a,double b){return std::abs(a-b)<1e-6;};
    const auto pad=[](double x,double y,const char *reference){
      Pad p;p.center={x,y};p.size_x=p.size_y=1;p.net=1;p.reference=reference;p.layers="F.Cu";return p;};
    const auto track=[](Point a,Point b,double width,const char *layer="F.Cu"){
      Track t;t.start=a;t.end=b;t.width=width;t.net=1;t.layer=layer;return t;};
    Board line;
    line.pads={pad(0,0,"A"),pad(10,0,"B"),pad(20,0,"C")};
    line.tracks={track({0,0},{10,0},1),track({10,0},{20,0},1)};
    auto flow=corvcad::net_flow(line,1,{0,1,2});
    require(flow.edges.size()==2 && flow.unit.size()==6,"flow size");
    // 1 A in at B leaves at A, against the first edge's direction; C's passes both edges.
    require(near(flow.unit[0*3+0],0) && near(flow.unit[0*3+1],-1) && near(flow.unit[0*3+2],-1) &&
            near(flow.unit[1*3+1],0) && near(flow.unit[1*3+2],-1),"flow along a line");
    Board parallel;
    parallel.pads={pad(0,0,"A"),pad(10,0,"B")};
    parallel.tracks={track({0,0},{10,0},1),track({0,0},{0,5},3),track({0,5},{10,5},3),track({10,5},{10,0},3)};
    flow=corvcad::net_flow(parallel,1,{0,1});
    // Direct: conductance 1/10. Around: 20 mm at width 3, conductance 3/20. Split 40 : 60.
    require(near(flow.unit[0*2+1],-0.4) && near(flow.unit[2*2+1],-0.6),"current splits by conductance");
    Board tee;
    tee.pads={pad(0,0,"A"),pad(10,5,"B")};
    tee.tracks={track({0,0},{20,0},1),track({10,0},{10,5},1)};
    flow=corvcad::net_flow(tee,1,{0,1});
    require(flow.edges.size()==3 && near(flow.unit[0*2+1],-1) && near(flow.unit[1*2+1],0),"a T off a track's middle");
    Board poured;
    poured.pads={pad(0,0,"A"),pad(30,0,"B")};
    poured.tracks={track({0,0},{10,0},1),track({20,0},{30,0},1)};
    corvcad::ZonePolygon zone;zone.net=1;zone.layer="F.Cu";zone.filled=true;
    zone.points={{9,-2},{21,-2},{21,2},{9,2}};
    poured.zones={zone};
    flow=corvcad::net_flow(poured,1,{0,1});
    require(near(flow.unit[0*2+1],-1) && near(flow.unit[1*2+1],-1),"a filled zone joins tracks");
    bool apart=false;
    poured.zones.clear();
    try {corvcad::net_flow(poured,1,{0,1});}catch(const std::runtime_error &){apart=true;}
    require(apart,"pins not joined by copper are refused");
    Board layers;
    layers.pads={pad(0,0,"A"),pad(20,0,"B")};
    layers.pads[1].layers="B.Cu";
    layers.tracks={track({0,0},{10,0},1),track({10,0},{20,0},1,"B.Cu")};
    corvcad::Via via;via.center={10,0};via.size=0.8;via.net=1;
    layers.vias={via};
    flow=corvcad::net_flow(layers,1,{0,1});
    require(near(flow.unit[0*2+1],-1) && near(flow.unit[1*2+1],-1),"a via joins layers");
  }
  bool failed=false;
  try {corvcad::parse_kicad_board("(kicad_pcb (gr_line (layer Edge.Cuts) (start 0 0) (end 1 1)))");}
  catch(...) {failed=true;}
  require(failed,"reject open outline");
}

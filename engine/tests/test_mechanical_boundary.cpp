#include "corvcad/mechanical_session_c_api.h"
#include "corvcad/board_input.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
static void check(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
int main() {
  try {
    check(!cc_mechanical_available(), "default edition must not claim native geometry");
    char error[128]{};
    check(!cc_mechanical_initialize(nullptr,nullptr,error,sizeof error) && error[0], "unavailable runtime diagnostic");
    auto settings = cc_default_settings();
    check(!cc_mechanical_open("enclosure.FCStd","{}",settings,error,sizeof error), "no fake session");
    cc_triangle triangle{1,2,3,4,5,6,7,8,9};
    check(!cc_mechanical_triangles(nullptr,0,&triangle,1,error,sizeof error) && triangle.ax==1 && triangle.cz==9,
          "unavailable adapter cannot return a substitute mesh");
    const auto target = std::filesystem::temp_directory_path()/"corvcad-unavailable-export.step";
    check(!std::filesystem::exists(target), "choose unused export fixture");
    check(!cc_mechanical_export_step(nullptr,0,target.string().c_str(),error,sizeof error) && !std::filesystem::exists(target),
          "unavailable exact export must not write a mesh or file");
    char tiny[1]{'x'};
    cc_mechanical_save(nullptr,tiny,1); check(tiny[0]==0,"bounded error buffer");
    cc_mechanical_close(nullptr);
    check(corvcad::board_thickness("(kicad_pcb (general (thickness 2.4)))")==2.4,"actual board thickness");
    check(corvcad::board_thickness("(kicad_pcb (footprint (property \"thickness\" \"99\")))")==0,"missing metadata is explicit");
    for (const auto *bad : {"nan", "inf", "0", "-1", "2.4junk"}) {
      bool rejected=false;
      try { corvcad::board_thickness(std::string("(kicad_pcb (general (thickness ")+bad+")))"); }
      catch(const std::exception &) { rejected=true; }
      check(rejected,"invalid thickness rejected");
    }
    std::cout << "Optional mechanical boundary and board metadata passed\n";
    return 0;
  } catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}

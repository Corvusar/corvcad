#include "corvcad/engine.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc,char **argv) {
  if(argc<3||argc>4) {
    std::cerr<<"Usage: corvcad_cli board.kicad_pcb output.stl [--step]\n";
    return 2;
  }
  try {
    std::ifstream in(argv[1],std::ios::binary);
    if(!in)throw std::runtime_error("Cannot open input board");
    std::string source((std::istreambuf_iterator<char>(in)),{});
    auto board=corvcad::parse_kicad_board(source);
    corvcad::Settings settings;
    if(argc==4&&std::string(argv[3])=="--step")corvcad::write_step(board,settings,argv[2]);
    else if(argc==3)corvcad::write_stl(corvcad::make_model(board,settings),argv[2]);
    else throw std::runtime_error("Unknown option");
    std::cout<<"Board "<<board.max_x-board.min_x<<" x "<<board.max_y-board.min_y
             <<" mm; "<<board.mounting_holes.size()<<" mounting holes; "
             <<board.placements.size()<<" footprints\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

#include "corvcad/engine.hpp"
#include "corvcad/board_input.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

#ifdef CORVCAD_WITH_OCCT
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <STEPControl_Writer.hxx>
#endif

namespace corvcad {
namespace {
constexpr double pi = 3.14159265358979323846;
struct Node { std::string atom; std::vector<Node> children; size_t begin = 0, end = 0; };
struct Reader {
  const std::string &s; size_t p = 0;
  void skip() {
    while (p < s.size()) {
      if (std::isspace(static_cast<unsigned char>(s[p]))) { ++p; continue; }
      if (s[p] == ';') { while (p < s.size() && s[p] != '\n') ++p; continue; }
      break;
    }
  }
  Node read() {
    skip();
    if (p >= s.size()) throw std::runtime_error("Unexpected end of KiCad file");
    const size_t begin=p;
    if (s[p] == '(') {
      ++p; Node n; n.begin=begin; skip();
      while (p < s.size() && s[p] != ')') { n.children.push_back(read()); skip(); }
      if (p == s.size()) throw std::runtime_error("Unclosed KiCad expression");
      ++p; n.end=p; return n;
    }
    Node n; n.begin=begin;
    if (s[p] == '"') {
      ++p;
      while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) ++p;
        n.atom += s[p++];
      }
      if (p == s.size()) throw std::runtime_error("Unclosed KiCad string");
      ++p;
    } else {
      while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '(' && s[p] != ')') n.atom += s[p++];
    }
    n.end=p; return n;
  }
};
std::string head(const Node &n) { return n.children.empty() ? "" : n.children.front().atom; }
const Node *child(const Node &n, const std::string &name) {
  for (const auto &c : n.children) if (head(c) == name) return &c;
  return nullptr;
}
std::string val(const Node *n, size_t i = 1) { return n && n->children.size() > i ? n->children[i].atom : ""; }
double num(const Node *n, size_t i = 1) {
  try { return std::stod(val(n, i)); } catch (...) { throw std::runtime_error("Invalid KiCad coordinate"); }
}
Point point(const Node *n) { return {num(n), num(n, 2)}; }
bool near(Point a, Point b) { return std::hypot(a.x-b.x, a.y-b.y) < 0.001; }
Point rotate(Point p, double degrees) {
  // KiCad board coordinates increase downward on screen, so a positive
  // footprint angle rotates local pad coordinates clockwise in this plane.
  const double a = -degrees*pi/180;
  return {p.x*std::cos(a)-p.y*std::sin(a), p.x*std::sin(a)+p.y*std::cos(a)};
}
std::vector<Point> arc_points(Point a, Point m, Point b) {
  const double d = 2*(a.x*(m.y-b.y)+m.x*(b.y-a.y)+b.x*(a.y-m.y));
  if (std::abs(d) < 1e-9) return {a,b};
  const double aa = a.x*a.x+a.y*a.y, mm=m.x*m.x+m.y*m.y, bb=b.x*b.x+b.y*b.y;
  Point c{(aa*(m.y-b.y)+mm*(b.y-a.y)+bb*(a.y-m.y))/d,
          (aa*(b.x-m.x)+mm*(a.x-b.x)+bb*(m.x-a.x))/d};
  double start=std::atan2(a.y-c.y,a.x-c.x), middle=std::atan2(m.y-c.y,m.x-c.x), end=std::atan2(b.y-c.y,b.x-c.x);
  auto positive=[](double x) { while(x<0)x+=2*pi; while(x>=2*pi)x-=2*pi; return x; };
  double sweep=positive(end-start);
  if (positive(middle-start)>sweep) sweep-=2*pi;
  int steps=std::max(2,static_cast<int>(std::ceil(std::abs(sweep)/(pi/18))));
  std::vector<Point> out; out.reserve(steps+1);
  double radius=std::hypot(a.x-c.x,a.y-c.y);
  for(int i=0;i<=steps;++i) { double t=start+sweep*i/steps; out.push_back({c.x+radius*std::cos(t),c.y+radius*std::sin(t)}); }
  return out;
}
using V = std::array<float,3>;
V v(double x,double y,double z) { return {static_cast<float>(x),static_cast<float>(y),static_cast<float>(z)}; }
void tri(std::vector<Triangle>& out,V a,V b,V c) { out.push_back({a,b,c}); }
void quad(std::vector<Triangle>& out,V a,V b,V c,V d) { tri(out,a,b,c); tri(out,a,c,d); }
void cylinder(std::vector<Triangle>& out,double x,double y,double z0,double z1,double radius) {
  constexpr int n=32;
  for(int i=0;i<n;++i) {
    double a=2*pi*i/n,b=2*pi*(i+1)/n;
    V l=v(x+radius*std::cos(a),y+radius*std::sin(a),z0),r=v(x+radius*std::cos(b),y+radius*std::sin(b),z0);
    V u=v(l[0],l[1],z1),w=v(r[0],r[1],z1);
    quad(out,l,r,w,u); tri(out,v(x,y,z0),r,l); tri(out,v(x,y,z1),u,w);
  }
}
void box(std::vector<Triangle>& out,double x0,double y0,double z0,double x1,double y1,double z1) {
  quad(out,v(x0,y0,z0),v(x0,y1,z0),v(x1,y1,z0),v(x1,y0,z0));
  quad(out,v(x0,y0,z1),v(x1,y0,z1),v(x1,y1,z1),v(x0,y1,z1));
  quad(out,v(x0,y0,z0),v(x1,y0,z0),v(x1,y0,z1),v(x0,y0,z1));
  quad(out,v(x1,y0,z0),v(x1,y1,z0),v(x1,y1,z1),v(x1,y0,z1));
  quad(out,v(x1,y1,z0),v(x0,y1,z0),v(x0,y1,z1),v(x1,y1,z1));
  quad(out,v(x0,y1,z0),v(x0,y0,z0),v(x0,y0,z1),v(x0,y1,z1));
}
double cross(Point a,Point b,Point c) { return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x); }
bool inside(Point p,Point a,Point b,Point c) {
  double d1=cross(a,b,p),d2=cross(b,c,p),d3=cross(c,a,p);
  return (d1>=-1e-9&&d2>=-1e-9&&d3>=-1e-9)||(d1<=1e-9&&d2<=1e-9&&d3<=1e-9);
}
void board_mesh(std::vector<Triangle>& out,const std::vector<Point>& poly,double z0,double z1) {
  if(poly.size()<3) return;
  std::vector<size_t> ids(poly.size()); for(size_t i=0;i<ids.size();++i)ids[i]=i;
  double area=0; for(size_t i=0;i<poly.size();++i) { auto a=poly[i],b=poly[(i+1)%poly.size()]; area+=a.x*b.y-b.x*a.y; }
  bool ccw=area>0;
  size_t guard=0;
  while(ids.size()>2 && guard++<poly.size()*poly.size()) {
    bool clipped=false;
    for(size_t j=0;j<ids.size();++j) {
      size_t ia=ids[(j+ids.size()-1)%ids.size()],ib=ids[j],ic=ids[(j+1)%ids.size()];
      auto a=poly[ia],b=poly[ib],c=poly[ic];
      if((cross(a,b,c)>1e-9)!=ccw) continue;
      bool occupied=false;
      for(auto id:ids) if(id!=ia&&id!=ib&&id!=ic&&inside(poly[id],a,b,c)) {occupied=true;break;}
      if(occupied)continue;
      tri(out,v(a.x,a.y,z1),v(b.x,b.y,z1),v(c.x,c.y,z1));
      tri(out,v(c.x,c.y,z0),v(b.x,b.y,z0),v(a.x,a.y,z0));
      ids.erase(ids.begin()+static_cast<long>(j)); clipped=true; break;
    }
    if(!clipped) throw std::runtime_error("Board outline cannot be triangulated");
  }
  for(size_t i=0;i<poly.size();++i) {
    auto a=poly[i],b=poly[(i+1)%poly.size()];
    quad(out,v(a.x,a.y,z0),v(b.x,b.y,z0),v(b.x,b.y,z1),v(a.x,a.y,z1));
  }
}
void emit_stl(std::ofstream &f,const std::vector<Triangle>& ts) {
  for(const auto &t:ts) {
    double ux=t.b[0]-t.a[0],uy=t.b[1]-t.a[1],uz=t.b[2]-t.a[2];
    double vx=t.c[0]-t.a[0],vy=t.c[1]-t.a[1],vz=t.c[2]-t.a[2];
    double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
    double length=std::hypot(std::hypot(nx,ny),nz); if(length>0) {nx/=length;ny/=length;nz/=length;}
    f<<"facet normal "<<nx<<' '<<ny<<' '<<nz<<"\n outer loop\n";
    for(auto p:{t.a,t.b,t.c}) f<<"  vertex "<<p[0]<<' '<<p[1]<<' '<<p[2]<<'\n';
    f<<" endloop\nendfacet\n";
  }
}
}

double board_thickness(const std::string &source) {
  Reader reader{source};
  const Node root = reader.read();
  const Node *general = child(root, "general");
  const Node *thickness = general ? child(*general, "thickness") : nullptr;
  if (!thickness) return 0;
  const std::string text = val(thickness);
  size_t consumed = 0;
  const double value = std::stod(text, &consumed);
  if (consumed != text.size() || !std::isfinite(value) || value <= 0)
    throw std::runtime_error("Invalid board thickness");
  return value;
}

Board parse_kicad_board(const std::string &source) {
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb") throw std::runtime_error("Expected a .kicad_pcb document");
  std::vector<std::vector<Point>> paths;
  Board board;
  auto net_code=[&](const Node *n) {
    if(!n)return 0;
    const std::string first=val(n);
    if(first.empty())return 0;
    size_t consumed=0;
    try {
      int code=std::stoi(first,&consumed);
      if(consumed==first.size()) {
        if(std::none_of(board.nets.begin(),board.nets.end(),
                        [&](const Net &net){return net.code==code;}))
          board.nets.push_back({code,val(n,2)});
        return code;
      }
    }catch(...){}
    for(const auto &net:board.nets)if(net.name==first)return net.code;
    int code=1;
    for(const auto &net:board.nets)code=std::max(code,net.code+1);
    board.nets.push_back({code,first});
    return code;
  };
  for(const auto &n:root.children) {
    const auto kind=head(n);
    if(kind=="net") {
      net_code(&n);
    }
    if(kind=="segment") {
      const Node *start=child(n,"start"),*end=child(n,"end"),*width=child(n,"width");
      if(start&&end&&width) {
        int net=net_code(child(n,"net"));
        board.tracks.push_back({point(start),point(end),num(width),net,val(child(n,"layer")),val(child(n,"uuid"))});
      }
    }
    // A track arc (KiCad's router writes them for length-tuning meanders and arc corners) as 16
    // chords sharing its UUID: the circle through start, mid and end, swept from start through mid.
    if(kind=="arc") {
      const Node *start=child(n,"start"),*mid=child(n,"mid"),*end=child(n,"end"),*width=child(n,"width");
      if(start&&mid&&end&&width) {
        const int net=net_code(child(n,"net"));
        const Point a=point(start),b=point(mid),c=point(end);
        std::vector<Point> path{a,b,c};
        const double d=2*(a.x*(b.y-c.y)+b.x*(c.y-a.y)+c.x*(a.y-b.y));
        if(std::abs(d)>1e-12) {
          const double ux=((a.x*a.x+a.y*a.y)*(b.y-c.y)+(b.x*b.x+b.y*b.y)*(c.y-a.y)+(c.x*c.x+c.y*c.y)*(a.y-b.y))/d;
          const double uy=((a.x*a.x+a.y*a.y)*(c.x-b.x)+(b.x*b.x+b.y*b.y)*(a.x-c.x)+(c.x*c.x+c.y*c.y)*(b.x-a.x))/d;
          const double two_pi=2*3.14159265358979323846,radius=std::hypot(a.x-ux,a.y-uy),from=std::atan2(a.y-uy,a.x-ux);
          const auto turn=[&](double to){return std::fmod(std::fmod(to-from,two_pi)+two_pi,two_pi);};
          const double to_mid=turn(std::atan2(b.y-uy,b.x-ux)),to_end=turn(std::atan2(c.y-uy,c.x-ux));
          const double sweep=to_mid<=to_end?to_end:to_end-two_pi;
          path={a};
          for(int i=1;i<16;++i) path.push_back({ux+radius*std::cos(from+sweep*i/16),uy+radius*std::sin(from+sweep*i/16)});
          path.push_back(c);
        }
        for(size_t i=1;i<path.size();++i)
          board.tracks.push_back({path[i-1],path[i],num(width),net,val(child(n,"layer")),val(child(n,"uuid"))});
      }
    }
    if(kind=="via") {
      const Node *at=child(n,"at"),*size=child(n,"size"),*drill=child(n,"drill");
      if(at&&size&&drill) {
        int net=net_code(child(n,"net"));
        board.vias.push_back({point(at),num(size),num(drill),net,val(child(n,"uuid"))});
      }
    }
    if(kind=="zone") {
      int net=net_code(child(n,"net"));
      std::string layer=val(child(n,"layer"));
      if(layer.empty()) layer=val(child(n,"layers"));
      auto points=[&](const Node &shape) {
        std::vector<Point> path; const Node *pts=child(shape,"pts");
        if(pts) for(const auto &xy:pts->children) if(head(xy)=="xy") path.push_back(point(&xy));
        return path;
      };
      const std::string uuid=val(child(n,"uuid"));
      // (priority N), (connect_pads [yes|no|thru_hole_only] (clearance C)), (min_thickness T),
      // (fill [yes] (mode hatch) (thermal_gap G) (thermal_bridge_width W)).
      ZoneSettings settings;
      if(const Node *priority=child(n,"priority")) settings.priority=static_cast<int>(num(priority));
      if(const Node *pads=child(n,"connect_pads")) {
        for(const auto &part:pads->children) {
          if(part.children.empty()&&part.atom=="yes") settings.connection=0;
          if(part.children.empty()&&part.atom=="no") settings.connection=2;
          if(part.children.empty()&&part.atom=="thru_hole_only") settings.connection=3;
        }
        if(const Node *clearance=child(*pads,"clearance")) settings.clearance=num(clearance);
      }
      if(const Node *thickness=child(n,"min_thickness")) settings.min_thickness=num(thickness);
      if(const Node *attr=child(n,"attr")) settings.teardrop=child(*attr,"teardrop")!=nullptr;
      if(const Node *fill=child(n,"fill")) {
        if(const Node *gap=child(*fill,"thermal_gap")) settings.thermal_gap=num(gap);
        if(const Node *spoke=child(*fill,"thermal_bridge_width")) settings.spoke_width=num(spoke);
        settings.hatched=val(child(*fill,"mode"))=="hatch";
      }
      for(const auto &shape:n.children) {
        if(head(shape)=="polygon") board.zones.push_back({net,layer,false,points(shape),uuid,settings});
        if(head(shape)=="filled_polygon") board.zones.push_back({net,val(child(shape,"layer")),true,points(shape),uuid,settings});
      }
    }
    if(kind=="gr_text") {
      const Node *at=child(n,"at"),*effects=child(n,"effects");
      const Node *font=effects?child(*effects,"font"):nullptr;
      const Node *size=font?child(*font,"size"):nullptr;
      if(at) board.texts.push_back({point(at),size?num(size):1,at->children.size()>3?num(at,3):0,
                                    val(&n),val(child(n,"layer")),val(child(n,"uuid"))});
    }
    if(kind=="dimension") {
      // A dimension as its extension lines and crossbar (PCB_DIM_ALIGNED::updateGeometry without
      // the extension offsets: the crossbar is height along (-dy, dx) from the measured points)
      // and its text, all with the dimension's UUID.
      const Node *pts=child(n,"pts"),*height=child(n,"height"),*style=child(n,"style");
      const Node *thickness=style?child(*style,"thickness"):nullptr;
      const std::string layer=val(child(n,"layer")),uuid=val(child(n,"uuid"));
      std::vector<Point> ends;
      if(pts)for(const auto &xy:pts->children)if(head(xy)=="xy")ends.push_back(point(&xy));
      const double width=thickness?num(thickness):0.1,h=height?num(height):0;
      if(ends.size()>=2) {
        const double dx=ends[1].x-ends[0].x,dy=ends[1].y-ends[0].y,length=std::hypot(dx,dy);
        if(length>0) {
          const Point a{ends[0].x-dy/length*h,ends[0].y+dx/length*h},b{ends[1].x-dy/length*h,ends[1].y+dx/length*h};
          for(const auto &segment:{std::pair<Point,Point>{ends[0],a},{ends[1],b},{a,b}})
            board.graphics.push_back({segment.first,segment.second,width,0,layer,uuid});
        }
      }
      if(const Node *text=child(n,"gr_text")) {
        const Node *at=child(*text,"at"),*effects=child(*text,"effects");
        const Node *font=effects?child(*effects,"font"):nullptr;
        const Node *size=font?child(*font,"size"):nullptr;
        if(at) board.texts.push_back({point(at),size?num(size):1,at->children.size()>3?num(at,3):0,val(text),layer,uuid});
      }
    }
    if(kind=="gr_line"||kind=="gr_arc"||kind=="gr_rect"||kind=="gr_poly") {
      if(val(child(n,"layer"))!="Edge.Cuts") {
        if(kind=="gr_line") {
          const Node *stroke=child(n,"stroke");
          const Node *width=stroke?child(*stroke,"width"):child(n,"width");
          board.graphics.push_back({point(child(n,"start")),point(child(n,"end")),width?num(width):0,0,
                                    val(child(n,"layer")),val(child(n,"uuid"))});
        }
        continue;
      }
      if(kind=="gr_line") paths.push_back({point(child(n,"start")),point(child(n,"end"))});
      if(kind=="gr_arc") paths.push_back(arc_points(point(child(n,"start")),point(child(n,"mid")),point(child(n,"end"))));
      if(kind=="gr_rect") {
        Point a=point(child(n,"start")),b=point(child(n,"end"));
        paths.push_back({a,{b.x,a.y},b,{a.x,b.y},a});
      }
      if(kind=="gr_poly") {
        const Node *pts=child(n,"pts"); std::vector<Point> path;
        if(pts) for(const auto &xy:pts->children) if(head(xy)=="xy") path.push_back(point(&xy));
        if(path.size()>2) {path.push_back(path.front());paths.push_back(path);}
      }
    }
    if(kind=="footprint") {
      std::string library=val(&n),ref,value;
      Point origin=point(child(n,"at")); double rotation=0;
      const Node *at=child(n,"at"); if(at&&at->children.size()>3) rotation=num(at,3);
      for(const auto &f:n.children) {
        if(head(f)=="property" && val(&f)=="Reference") ref=val(&f,2);
        if(head(f)=="property" && val(&f)=="Value") value=val(&f,2);
        if(head(f)=="fp_text" && val(&f)=="reference" && ref.empty()) ref=val(&f,2);
        if(head(f)=="fp_text" && val(&f)=="value" && value.empty()) value=val(&f,2);
      }
      Placement placed{origin,rotation,ref,value,library};
      placed.back=val(child(n,"layer"))=="B.Cu";
      if(const Node *model=child(n,"model")) {
        placed.model=val(model);
        auto xyz=[&](const char *name,std::array<double,3> &out) {
          const Node *group=child(*model,name),*v=group?child(*group,"xyz"):nullptr;
          if(v&&v->children.size()>3) out={num(v,1),num(v,2),num(v,3)};
        };
        xyz("offset",placed.offset);xyz("rotate",placed.rotate);xyz("scale",placed.scale);
      }
      bool first_pad=true;
      for(const auto &pad:n.children) if(head(pad)=="pad") {
        const Node *pad_at=child(pad,"at"),*size=child(pad,"size");
        if(!pad_at||!size)continue;
        const Point c=point(pad_at);const double hx=num(size,1)/2,hy=size->children.size()>2?num(size,2)/2:hx;
        const double r=std::max(hx,hy); // ponytail: rotated pads use their larger half-size, not the exact box
        auto &e=placed.extent;
        if(first_pad) e={c.x-r,c.y-r,c.x+r,c.y+r};
        else e={std::min(e[0],c.x-r),std::min(e[1],c.y-r),std::max(e[2],c.x+r),std::max(e[3],c.y+r)};
        first_pad=false;
      }
      board.placements.push_back(placed);
      for(const auto &pad:n.children) if(head(pad)=="pad") {
        const Node *pad_at=child(pad,"at"),*size=child(pad,"size");
        if(!pad_at||!size)continue;
        Point local=rotate(point(pad_at),rotation);
        double pad_rotation=rotation;
        if(pad_at->children.size()>3)pad_rotation=num(pad_at,3);
        int net=net_code(child(pad,"net"));
        std::string layers;
        if(const Node *listed=child(pad,"layers"))
          for(size_t i=1;i<listed->children.size();++i) {
            if(!layers.empty())layers+=',';
            layers+=listed->children[i].atom;
          }
        board.pads.push_back({{origin.x+local.x,origin.y+local.y},num(size),num(size,2),
                              pad_rotation,net,ref,val(&pad),val(&pad,2),val(&pad,3),layers});
      }
      // A mounting hole is a footprint from KiCad's MountingHole library or one named after it;
      // a D-sub "..._MountingHolesOffset9.12mm" is a connector, not a hole.
      const size_t colon=library.find(':');
      const std::string nickname=library.substr(0,colon),name=colon==std::string::npos?library:library.substr(colon+1);
      if(nickname=="MountingHole"||name.rfind("MountingHole",0)==0) {
        for(const auto &pad:n.children) if(head(pad)=="pad") {
          const Node *drill=child(pad,"drill"),*pad_at=child(pad,"at");
          if(!drill||!pad_at)continue;
          double diameter=0;
          for(size_t i=1;i<drill->children.size();++i) {
            try {diameter=std::stod(drill->children[i].atom);break;}catch(...){}
          }
          if(diameter<=0)continue;
          Point local=rotate(point(pad_at),rotation);
          board.mounting_holes.push_back({{origin.x+local.x,origin.y+local.y},diameter,ref});
        }
      }
    }
  }
  if(paths.empty())throw std::runtime_error("No Edge.Cuts outline found");
  // Chain the paths into loops; the board is the largest closed one. Other loops (inner
  // cutouts) and open chains (milled slots, as imported Eagle boards have) are not part of it.
  double best_area=0;
  while(!paths.empty()) {
    std::vector<Point> chain=paths.front(); paths.erase(paths.begin());
    for(bool found=true;found&&!near(chain.front(),chain.back());) {
      found=false;
      for(size_t i=0;i<paths.size()&&!found;++i) {
        auto &path=paths[i];
        if(near(chain.back(),path.front())) {chain.insert(chain.end(),path.begin()+1,path.end());found=true;}
        else if(near(chain.back(),path.back())) {chain.insert(chain.end(),path.rbegin()+1,path.rend());found=true;}
        if(found)paths.erase(paths.begin()+static_cast<long>(i));
      }
    }
    if(chain.size()<4||!near(chain.front(),chain.back()))continue;
    double area=0;
    for(size_t i=0;i+1<chain.size();++i) area+=chain[i].x*chain[i+1].y-chain[i+1].x*chain[i].y;
    if(std::abs(area)>best_area) {best_area=std::abs(area);board.outline=chain;}
  }
  if(board.outline.empty()) throw std::runtime_error("Edge.Cuts outline is open");
  board.outline.pop_back();
  board.min_x=board.min_y=std::numeric_limits<double>::infinity();
  board.max_x=board.max_y=-std::numeric_limits<double>::infinity();
  for(auto p:board.outline) {
    board.min_x=std::min(board.min_x,p.x);board.min_y=std::min(board.min_y,p.y);
    board.max_x=std::max(board.max_x,p.x);board.max_y=std::max(board.max_y,p.y);
  }
  return board;
}
std::string move_footprint(const std::string &source,const std::string &reference,
                           double x,double y,double rotation) {
  if(reference.empty()||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(rotation))
    throw std::runtime_error("A reference and finite coordinates are required");
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  const Node *target=nullptr;
  for(const auto &n:root.children) if(head(n)=="footprint") {
    std::string ref;
    for(const auto &f:n.children) {
      if(head(f)=="property"&&val(&f)=="Reference")ref=val(&f,2);
      if(ref.empty()&&head(f)=="fp_text"&&val(&f)=="reference")ref=val(&f,2);
    }
    if(ref==reference) {
      if(target)throw std::runtime_error("Duplicate footprint reference");
      target=child(n,"at");
      if(!target)throw std::runtime_error("Footprint has no position");
    }
  }
  if(!target)throw std::runtime_error("Footprint reference not found");
  auto number=[](double value) {
    std::ostringstream out;out.imbue(std::locale::classic());
    out<<std::fixed<<std::setprecision(6)<<value;
    std::string result=out.str();
    while(result.find('.')!=std::string::npos&&result.back()=='0')result.pop_back();
    if(result.back()=='.')result.pop_back();
    if(result=="-0")result="0";
    return result;
  };
  std::string edited=source;
  edited.replace(target->begin,target->end-target->begin,
                 "(at "+number(x)+" "+number(y)+" "+number(rotation)+")");
  parse_kicad_board(edited);
  return edited;
}
std::string delete_footprint(const std::string &source,const std::string &reference) {
  if(reference.empty())throw std::runtime_error("Missing footprint reference");
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  const Node *target=nullptr;
  for(const auto &node:root.children)if(head(node)=="footprint") {
    std::string ref;
    for(const auto &field:node.children) {
      if(head(field)=="property"&&val(&field)=="Reference")ref=val(&field,2);
      if(ref.empty()&&head(field)=="fp_text"&&val(&field)=="reference")ref=val(&field,2);
    }
    if(ref==reference) {
      if(target)throw std::runtime_error("Duplicate footprint reference");
      target=&node;
    }
  }
  if(!target)throw std::runtime_error("Footprint reference not found");
  std::string edited=source;
  edited.erase(target->begin,target->end-target->begin);
  parse_kicad_board(edited);
  return edited;
}
namespace {
// Erases these top-level nodes with their indentation and line break.
std::string erase_nodes(const std::string &source,std::vector<const Node*> nodes) {
  std::sort(nodes.begin(),nodes.end(),[](const Node *a,const Node *b){return a->begin>b->begin;});
  std::string edited=source;
  for(const Node *node:nodes) {
    size_t begin=node->begin;
    while(begin>0&&(edited[begin-1]==' '||edited[begin-1]=='\t'))--begin;
    size_t end=node->end;
    if(end<edited.size()&&edited[end]=='\r')++end;
    if(end<edited.size()&&edited[end]=='\n')++end;
    edited.erase(begin,end-begin);
  }
  return edited;
}
}
std::string delete_board_copper(const std::string &source,const std::string &uuid) {
  if(uuid.empty())throw std::runtime_error("Missing item UUID");
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  const Node *target=nullptr;
  for(const auto &node:root.children) {
    const auto kind=head(node);
    if((kind=="segment"||kind=="arc"||kind=="via"||kind=="zone"||kind=="gr_line"||kind=="gr_text"||kind=="dimension")&&
       val(child(node,"uuid"))==uuid) {
      if(target)throw std::runtime_error("Duplicate item UUID");
      target=&node;
    }
  }
  if(!target)throw std::runtime_error("Item not found");
  const auto edited=erase_nodes(source,{target});
  parse_kicad_board(edited);
  return edited;
}
namespace {
std::string millimetres(double value) {
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<std::fixed<<std::setprecision(6)<<value;
  std::string result=out.str();
  while(result.find('.')!=std::string::npos&&result.back()=='0')result.pop_back();
  if(result.back()=='.')result.pop_back();
  return result=="-0"?"0":result;
}
// The top-level gr_line, gr_text, zone or dimension with this UUID: what the Draw tool adds.
const Node &drawn_item(const Node &root,const std::string &uuid) {
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  const Node *target=nullptr;
  for(const auto &node:root.children) {
    const auto kind=head(node);
    if((kind=="gr_line"||kind=="gr_text"||kind=="zone"||kind=="dimension")&&val(child(node,"uuid"))==uuid) {
      if(target)throw std::runtime_error("Duplicate item UUID");
      target=&node;
    }
  }
  if(!target)throw std::runtime_error("Item not found");
  return *target;
}
struct Splice { size_t begin,end; std::string text; };
std::string splice(std::string text,std::vector<Splice> edits) {
  std::sort(edits.begin(),edits.end(),[](const Splice &a,const Splice &b){return a.begin>b.begin;});
  for(const auto &e:edits)text.replace(e.begin,e.end-e.begin,e.text);
  return text;
}
}
std::string move_board_drawing(const std::string &source,const std::string &uuid,double dx,double dy) {
  if(!std::isfinite(dx)||!std::isfinite(dy))throw std::runtime_error("Offset must be finite");
  Reader reader{source}; Node root=reader.read();
  const Node &item=drawn_item(root,uuid);
  std::vector<Splice> edits;
  auto shift=[&](const Node &xy) {
    if(xy.children.size()<3)throw std::runtime_error("Invalid KiCad coordinate");
    edits.push_back({xy.children[1].begin,xy.children[1].end,millimetres(num(&xy)+dx)});
    edits.push_back({xy.children[2].begin,xy.children[2].end,millimetres(num(&xy,2)+dy)});
  };
  const auto kind=head(item);
  if(kind=="gr_line") {
    for(const char *end:{"start","end"}) {
      const Node *point=child(item,end);
      if(!point)throw std::runtime_error("Line has no ends");
      shift(*point);
    }
  } else if(kind=="gr_text") {
    const Node *at=child(item,"at");
    if(!at)throw std::runtime_error("Text has no position");
    shift(*at);
  } else if(kind=="dimension") {
    // Its measured points and its text; KiCad recomputes the rest from them on load.
    const Node *points=child(item,"pts"),*text=child(item,"gr_text");
    if(!points)throw std::runtime_error("Dimension has no points");
    for(const auto &xy:points->children)if(head(xy)=="xy")shift(xy);
    if(const Node *at=text?child(*text,"at"):nullptr)shift(*at);
  } else {
    const Node *outline=child(item,"polygon");
    const Node *points=outline?child(*outline,"pts"):nullptr;
    if(!points)throw std::runtime_error("Zone has no outline");
    for(const auto &xy:points->children)if(head(xy)=="xy")shift(xy);
    // The old fill no longer matches the outline; the zone stays unfilled until KiCad's
    // filler runs again (the NATIVE_KICAD edition refills in the same step).
    for(const auto &fill:item.children)if(head(fill)=="filled_polygon") {
      size_t begin=fill.begin;
      while(begin>0&&(source[begin-1]==' '||source[begin-1]=='\t'||source[begin-1]=='\n'||source[begin-1]=='\r'))--begin;
      edits.push_back({begin,fill.end,""});
    }
  }
  const auto edited=splice(source,edits);
  parse_kicad_board(edited);
  return edited;
}
std::string set_board_text(const std::string &source,const std::string &uuid,const std::string &text,double size) {
  if(text.empty()||text.size()>128||std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32;}))
    throw std::runtime_error("Enter one line of 1-128 characters");
  if(!std::isfinite(size)||size<0.5||size>10)throw std::runtime_error("Use a text size of 0.5-10 mm");
  Reader reader{source}; Node root=reader.read();
  const Node &item=drawn_item(root,uuid);
  if(head(item)!="gr_text"||item.children.size()<2)throw std::runtime_error("Only text can be edited");
  std::string quoted="\"";
  for(char c:text){if(c=='\\'||c=='"')quoted+='\\';quoted+=c;}
  quoted+='"';
  std::vector<Splice> edits{{item.children[1].begin,item.children[1].end,quoted}};
  const Node *effects=child(item,"effects");
  const Node *font=effects?child(*effects,"font"):nullptr;
  const Node *height=font?child(*font,"size"):nullptr;
  if(!height||height->children.size()<3)throw std::runtime_error("Text has no font size");
  edits.push_back({height->children[1].begin,height->children[1].end,millimetres(size)});
  edits.push_back({height->children[2].begin,height->children[2].end,millimetres(size)});
  // KiCad's default stroke is 15% of the height, as add_board_text writes it.
  if(const Node *thickness=child(*font,"thickness");thickness&&thickness->children.size()>=2)
    edits.push_back({thickness->children[1].begin,thickness->children[1].end,millimetres(size*0.15)});
  const auto edited=splice(source,edits);
  parse_kicad_board(edited);
  return edited;
}
std::string remove_board_outline(const std::string &source) {
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  std::vector<const Node*> edges;
  for(const auto &node:root.children) {
    const auto kind=head(node);
    if((kind=="gr_line"||kind=="gr_arc"||kind=="gr_rect"||kind=="gr_poly"||kind=="gr_circle")&&
       val(child(node,"layer"))=="Edge.Cuts") edges.push_back(&node);
  }
  return erase_nodes(source,edges);
}
std::string rename_footprint(const std::string &source,const std::string &reference,
                             const std::string &new_reference) {
  if(new_reference.empty()||new_reference.size()>16||
     !std::isalpha(static_cast<unsigned char>(new_reference.front()))||
     !std::all_of(new_reference.begin()+1,new_reference.end(),[](unsigned char c){
       return std::isalnum(c)!=0;
     }))throw std::runtime_error("Use a reference such as R1 or H1");
  const auto board=parse_kicad_board(source);
  for(const auto &part:board.placements)
    if(part.reference==new_reference&&new_reference!=reference)
      throw std::runtime_error("Reference is already on the board");
  Reader reader{source}; Node root=reader.read();
  const Node *target=nullptr;
  for(const auto &node:root.children)if(head(node)=="footprint") {
    std::string ref;
    for(const auto &field:node.children)
      if(head(field)=="property"&&val(&field)=="Reference")ref=val(&field,2);
    if(ref==reference) {
      if(target)throw std::runtime_error("Duplicate footprint reference");
      target=&node;
    }
  }
  if(!target)throw std::runtime_error("Footprint reference not found");
  const Node *property=nullptr;
  for(const auto &field:target->children)
    if(head(field)=="property"&&val(&field)=="Reference")property=&field;
  if(!property||property->children.size()<3)throw std::runtime_error("Footprint reference property not found");
  const Node &text=property->children[2];
  std::string edited=source;
  edited.replace(text.begin,text.end-text.begin,"\""+new_reference+"\"");
  parse_kicad_board(edited);
  return edited;
}
std::string rotate_footprint(const std::string &source,const std::string &reference) {
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  const Node *target=nullptr;
  for(const auto &node:root.children)if(head(node)=="footprint") {
    std::string ref;
    for(const auto &field:node.children)
      if(head(field)=="property"&&val(&field)=="Reference")ref=val(&field,2);
    if(ref==reference) {
      if(target)throw std::runtime_error("Duplicate footprint reference");
      target=&node;
    }
  }
  if(!target)throw std::runtime_error("Footprint reference not found");
  std::vector<const Node*> angles{child(*target,"at")};
  if(!angles[0])throw std::runtime_error("Footprint has no position");
  for(const auto &item:target->children)
    if(head(item)=="property"||head(item)=="fp_text"||head(item)=="pad")
      if(const Node *at=child(item,"at"))angles.push_back(at);
  std::string edited=source;
  // Back to front, so earlier offsets stay valid.
  std::sort(angles.begin(),angles.end(),[](const Node *a,const Node *b){return a->begin>b->begin;});
  for(const Node *node:angles) {
    const Node &at=*node;
    if(at.children.size()<3)throw std::runtime_error("Footprint item has no position");
    const std::string &third=at.children.size()>3?at.children[3].atom:std::string();
    const bool has=!third.empty()&&std::isdigit(static_cast<unsigned char>(third.back()));
    double angle=std::fmod((has?num(&at,3):0)+90,360);
    std::ostringstream out;out.imbue(std::locale::classic());out<<angle;
    if(has)edited.replace(at.children[3].begin,at.children[3].end-at.children[3].begin,out.str());
    else edited.insert(at.children[2].end," "+out.str());
  }
  parse_kicad_board(edited);
  return edited;
}
std::string set_pad_nets(const std::string &source,
                         const std::map<std::string,std::map<std::string,std::string>> &nets) {
  Reader reader{source}; Node root=reader.read();
  if(head(root)!="kicad_pcb")throw std::runtime_error("Expected a .kicad_pcb document");
  auto quoted=[](const std::string &text) {
    std::string out="\"";
    for(char c:text){if(c=='"'||c=='\\')out+='\\';out+=c;}
    return out+"\"";
  };
  struct Edit { size_t begin,end; std::string text; };
  std::vector<Edit> edits;
  for(const auto &node:root.children)if(head(node)=="footprint") {
    std::string ref;
    for(const auto &field:node.children)
      if(head(field)=="property"&&val(&field)=="Reference")ref=val(&field,2);
    const auto wanted=nets.find(ref);
    if(wanted==nets.end())continue;
    for(const auto &pad:node.children) {
      if(head(pad)!="pad"||val(&pad).empty())continue;
      const auto found=wanted->second.find(val(&pad));
      const std::string name=found==wanted->second.end()?"":found->second;
      const Node *net=child(pad,"net");
      if(net&&!name.empty()) {
        // Older boards write (net <code> "NAME"); KiCad 10 reads and writes the name alone.
        if(val(net,net->children.size()-1)!=name||net->children.size()!=2)
          edits.push_back({net->begin,net->end,"(net "+quoted(name)+")"});
      } else if(net) {
        size_t begin=net->begin;
        while(begin>pad.begin&&std::isspace(static_cast<unsigned char>(source[begin-1])))--begin;
        edits.push_back({begin,net->end,""});
      } else if(!name.empty()) {
        const Node *uuid=child(pad,"uuid");
        const size_t at=uuid?uuid->begin:pad.end-1;
        edits.push_back({at,at,uuid?"(net "+quoted(name)+") ":" (net "+quoted(name)+")"});
      }
    }
  }
  std::string edited=source;
  std::sort(edits.begin(),edits.end(),[](const Edit &a,const Edit &b){return a.begin>b.begin;});
  for(const auto &edit:edits)edited.replace(edit.begin,edit.end-edit.begin,edit.text);
  parse_kicad_board(edited);
  return edited;
}
void validate(const Board &b,const Settings &s) {
  if(b.outline.size()<3||b.max_x<=b.min_x||b.max_y<=b.min_y)throw std::runtime_error("Invalid board outline");
  if(s.margin<0||s.wall<=0||s.floor<=0||s.board_clearance<0||s.standoff_height<0||
     s.standoff_diameter<=0||s.lid_thickness<=0||s.lid_gap<0)
    throw std::runtime_error("Enclosure dimensions must be positive; margin and clearances may be zero");
  if(s.standoff_height+s.board_clearance<=0)throw std::runtime_error("Enclosure height is zero");
}
Model make_model(const Board &b,const Settings &s) {
  validate(b,s); Model model;
  auto &o=model.enclosure;
  double x0=b.min_x-s.margin-s.wall,x1=b.max_x+s.margin+s.wall;
  double y0=b.min_y-s.margin-s.wall,y1=b.max_y+s.margin+s.wall;
  double u0=x0+s.wall,u1=x1-s.wall,v0=y0+s.wall,v1=y1-s.wall;
  double z0=0,zf=s.floor,zt=s.floor+s.standoff_height+s.board_clearance+1.6;
  // One connected tray shell: bottom, outside, rim, cavity sides and cavity floor.
  quad(o,v(x0,y0,z0),v(x0,y1,z0),v(x1,y1,z0),v(x1,y0,z0));
  quad(o,v(x0,y0,z0),v(x1,y0,z0),v(x1,y0,zt),v(x0,y0,zt));
  quad(o,v(x1,y0,z0),v(x1,y1,z0),v(x1,y1,zt),v(x1,y0,zt));
  quad(o,v(x1,y1,z0),v(x0,y1,z0),v(x0,y1,zt),v(x1,y1,zt));
  quad(o,v(x0,y1,z0),v(x0,y0,z0),v(x0,y0,zt),v(x0,y1,zt));
  quad(o,v(x0,y0,zt),v(x1,y0,zt),v(u1,v0,zt),v(u0,v0,zt));
  quad(o,v(x1,y0,zt),v(x1,y1,zt),v(u1,v1,zt),v(u1,v0,zt));
  quad(o,v(x1,y1,zt),v(x0,y1,zt),v(u0,v1,zt),v(u1,v1,zt));
  quad(o,v(x0,y1,zt),v(x0,y0,zt),v(u0,v0,zt),v(u0,v1,zt));
  quad(o,v(u0,v0,zt),v(u1,v0,zt),v(u1,v0,zf),v(u0,v0,zf));
  quad(o,v(u1,v0,zt),v(u1,v1,zt),v(u1,v1,zf),v(u1,v0,zf));
  quad(o,v(u1,v1,zt),v(u0,v1,zt),v(u0,v1,zf),v(u1,v1,zf));
  quad(o,v(u0,v1,zt),v(u0,v0,zt),v(u0,v0,zf),v(u0,v1,zf));
  quad(o,v(u0,v0,zf),v(u1,v0,zf),v(u1,v1,zf),v(u0,v1,zf));
  model.floor.insert(model.floor.end(),o.begin(),o.begin()+2);
  model.floor.insert(model.floor.end(),o.end()-2,o.end());
  model.walls.insert(model.walls.end(),o.begin()+2,o.begin()+10);
  model.walls.insert(model.walls.end(),o.begin()+18,o.begin()+26);
  model.rim.insert(model.rim.end(),o.begin()+10,o.begin()+18);
  box(model.lid,x0+s.lid_gap,y0+s.lid_gap,zt+s.lid_gap,
      x1-s.lid_gap,y1-s.lid_gap,zt+s.lid_gap+s.lid_thickness);
  board_mesh(model.board,b.outline,zf+s.standoff_height,zf+s.standoff_height+1.6);
  for(const auto &h:b.mounting_holes) cylinder(model.standoffs,h.center.x,h.center.y,zf,zf+s.standoff_height,
                                                std::max(s.standoff_diameter,h.diameter+2)/2);
  return model;
}
void write_stl(const Model &m,const std::string &path) {
  std::ofstream f(path,std::ios::binary); if(!f)throw std::runtime_error("Cannot open STL output");
  f<<std::setprecision(9)<<"solid corvcad_enclosure\n";
  emit_stl(f,m.enclosure); emit_stl(f,m.standoffs);
  f<<"endsolid corvcad_enclosure\n";
  if(!f)throw std::runtime_error("Failed to write STL");
}
void write_lid_stl(const Model &m,const std::string &path) {
  std::ofstream f(path,std::ios::binary); if(!f)throw std::runtime_error("Cannot open lid STL output");
  f<<std::setprecision(9)<<"solid corvcad_lid\n";
  emit_stl(f,m.lid);
  f<<"endsolid corvcad_lid\n";
  if(!f)throw std::runtime_error("Failed to write lid STL");
}
bool step_available() {
#ifdef CORVCAD_WITH_OCCT
  return true;
#else
  return false;
#endif
}
void write_step(const Board &b,const Settings &s,const std::string &path) {
#ifdef CORVCAD_WITH_OCCT
  validate(b,s);
  double x0=b.min_x-s.margin-s.wall,x1=b.max_x+s.margin+s.wall;
  double y0=b.min_y-s.margin-s.wall,y1=b.max_y+s.margin+s.wall;
  double height=s.floor+s.standoff_height+s.board_clearance+1.6;
  TopoDS_Shape outer=BRepPrimAPI_MakeBox(gp_Pnt(x0,y0,0),x1-x0,y1-y0,height).Shape();
  TopoDS_Shape cavity=BRepPrimAPI_MakeBox(gp_Pnt(x0+s.wall,y0+s.wall,s.floor),
      x1-x0-2*s.wall,y1-y0-2*s.wall,height).Shape();
  TopoDS_Shape shape=BRepAlgoAPI_Cut(outer,cavity).Shape();
  for(const auto &h:b.mounting_holes) {
    double r=std::max(s.standoff_diameter,h.diameter+2)/2;
    TopoDS_Shape boss=BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(h.center.x,h.center.y,s.floor),gp_Dir(0,0,1)),r,s.standoff_height).Shape();
    shape=BRepAlgoAPI_Fuse(shape,boss).Shape();
  }
  STEPControl_Writer writer; writer.Transfer(shape,STEPControl_AsIs);
  if(writer.Write(path.c_str())!=IFSelect_RetDone)throw std::runtime_error("STEP export failed");
#else
  (void)b;(void)s;(void)path;
  throw std::runtime_error("STEP export requires an Open CASCADE enabled engine build");
#endif
}
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop check of the generator (gate 2 in docs/3D_MODELS.md).
//   parts3d_check compare <reference-dir> <name>...   generated vs KiCad's STEP <dir>/<name>.step:
//                                                     bounding box and volume of the fused solids
//   parts3d_check all                                 every table row: builds, valid, deterministic
// Exit status 1 when anything is out of tolerance or fails.
#include "parts3d.hpp"
#include "parts3d_c_api.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Reader.hxx>
#include <TopTools_ListOfShape.hxx>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

using namespace corvcad::parts3d;

namespace {
struct Measure { double box[6]; double volume; };

Measure measure(const TopoDS_Shape &shape) {
  Measure m{};
  Bnd_Box bounds;
  BRepBndLib::AddOptimal(shape, bounds, false, false);
  bounds.Get(m.box[0], m.box[1], m.box[2], m.box[3], m.box[4], m.box[5]);
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  m.volume = props.Mass();
  return m;
}

TopoDS_Shape fused(const Part &part) {
  TopTools_ListOfShape arguments, tools;
  arguments.Append(part.solids[0].shape);
  for (size_t i = 1; i < part.solids.size(); ++i) tools.Append(part.solids[i].shape);
  BRepAlgoAPI_Fuse fuse;
  fuse.SetArguments(arguments);
  fuse.SetTools(tools);
  fuse.Build();
  return fuse.Shape();
}

std::string brep(const Part &part) {
  std::ostringstream out;
  for (const Solid &solid : part.solids) BRepTools::Write(solid.shape, out);
  return out.str();
}
} // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && std::strcmp(argv[1], "all") == 0) {
    int failed = 0, invalid = 0, varying = 0, count = 0;
    double slowest = 0, total = 0;
    std::string slowest_name;
    for (const std::string &name : names()) {
      ++count;
      std::string error;
      const auto start = std::chrono::steady_clock::now();
      const auto part = generate(name, &error);
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      total += seconds;
      if (seconds > slowest) { slowest = seconds; slowest_name = name; }
      if (!part) { ++failed; std::printf("FAIL %s: %s\n", name.c_str(), error.c_str()); continue; }
      for (const Solid &solid : part->solids)
        if (!BRepCheck_Analyzer(solid.shape).IsValid()) { ++invalid; std::printf("INVALID %s\n", name.c_str()); break; }
      if (brep(*part) != brep(*generate(name))) { ++varying; std::printf("NONDETERMINISTIC %s\n", name.c_str()); }
    }
    std::printf("models %d, failed %d, invalid %d, nondeterministic %d; %.1f s total, slowest %s %.2f s\n", count,
                failed, invalid, varying, total, slowest_name.c_str(), slowest);
    return failed || invalid || varying ? 1 : 0;
  }
  if (argc < 4 || std::strcmp(argv[1], "compare") != 0) {
    std::fprintf(stderr, "usage: parts3d_check compare <reference-dir> <name>... | all\n");
    return 2;
  }
  int bad = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string name = argv[i];
    std::string error;
    const auto part = generate(name, &error);
    if (!part) { std::printf("%s: not generated %s\n", name.c_str(), error.c_str()); ++bad; continue; }
    STEPControl_Reader reader;
    if (reader.ReadFile((std::string(argv[2]) + "/" + name + ".step").c_str()) != IFSelect_RetDone) {
      std::printf("%s: no reference STEP\n", name.c_str()); ++bad; continue;
    }
    reader.TransferRoots();
    const Measure ours = measure(fused(*part)), theirs = measure(reader.OneShape());
    double worst = 0;
    for (int k = 0; k < 6; ++k) worst = std::max(worst, std::abs(ours.box[k] - theirs.box[k]));
    const double volume = (ours.volume - theirs.volume) / theirs.volume;
    // Tolerances: 0.02 mm on each bounding-box side, 1% of the volume.
    // KiCad 10's LQFP-48 file predates the generator table: its lead shoulder sits at z 0.625..0.725, the
    // table's A1/A2/c put it at 0.75..0.85 (bounding box identical). Reported, not failed.
    const bool known = name == "LQFP-48_7x7mm_P0.5mm";
    const bool ok = worst <= 0.02 && std::abs(volume) <= 0.01;
    // The app's mesh (parts3d_c_api): same box, and outward winding (positive enclosed volume).
    std::vector<cc_part_triangle> mesh(cc_parts3d_triangles(name.c_str(), nullptr, 0, nullptr, 0));
    cc_parts3d_triangles(name.c_str(), mesh.data(), mesh.size(), nullptr, 0);
    double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9}, enclosed = 0;
    for (const auto &t : mesh) {
      const float p[3][3] = {{t.ax, t.ay, t.az}, {t.bx, t.by, t.bz}, {t.cx, t.cy, t.cz}};
      for (const auto &q : p)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], double(q[k])), hi[k] = std::max(hi[k], double(q[k]));
      enclosed += (p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) - p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                   p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0])) / 6.0;
    }
    double mesh_worst = 0;
    for (int k = 0; k < 3; ++k)
      mesh_worst = std::max({mesh_worst, std::abs(lo[k] - ours.box[k]), std::abs(hi[k] - ours.box[k + 3])});
    if (mesh.empty() || mesh_worst > 0.02 || enclosed <= 0) {
      std::printf("%s: mesh %zu triangles, box off by %.4f mm, enclosed %.4f mm3\n", name.c_str(), mesh.size(),
                  mesh_worst, enclosed);
      ++bad;
    }
    if (!ok && !known) ++bad;
    std::printf("%-42s %s  box %.3f..%.3f %.3f..%.3f %.3f..%.3f (worst side %.4f mm)  volume %.4f vs %.4f mm3 (%+.2f%%)\n",
                name.c_str(), ok ? "OK  " : known ? "KNWN" : "DIFF", ours.box[0], ours.box[3], ours.box[1], ours.box[4], ours.box[2],
                ours.box[5], worst, ours.volume, theirs.volume, volume * 100);
  }
  return bad ? 1 : 0;
}

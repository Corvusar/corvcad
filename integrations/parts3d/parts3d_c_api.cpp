// SPDX-License-Identifier: GPL-3.0-or-later
#include "parts3d_c_api.h"
#include "parts3d.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

std::vector<cc_part_triangle> tessellate(const corvcad::parts3d::Part &part) {
  std::vector<cc_part_triangle> out;
  for (const auto &solid : part.solids) {
    if (solid.shape.IsNull()) continue;
    BRepMesh_IncrementalMesh(solid.shape, 0.01, false, 0.3); // 0.01 mm chord, ~17 degrees
    for (TopExp_Explorer faces(solid.shape, TopAbs_FACE); faces.More(); faces.Next()) {
      const TopoDS_Face &face = TopoDS::Face(faces.Current());
      TopLoc_Location location;
      const Handle(Poly_Triangulation) mesh = BRep_Tool::Triangulation(face, location);
      if (mesh.IsNull()) continue;
      const gp_Trsf transform = location.Transformation();
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      for (int i = 1; i <= mesh->NbTriangles(); ++i) {
        int a, b, c;
        mesh->Triangle(i).Get(a, b, c);
        if (reversed) std::swap(b, c);
        const gp_Pnt p[3] = {mesh->Node(a).Transformed(transform), mesh->Node(b).Transformed(transform),
                             mesh->Node(c).Transformed(transform)};
        out.push_back({float(p[0].X()), float(p[0].Y()), float(p[0].Z()), float(p[1].X()), float(p[1].Y()),
                       float(p[1].Z()), float(p[2].X()), float(p[2].Y()), float(p[2].Z()), solid.color[0],
                       solid.color[1], solid.color[2]});
      }
    }
  }
  return out;
}

void set_error(char *error, size_t capacity, const std::string &text) {
  if (!error || !capacity) return;
  const size_t n = std::min(text.size(), capacity - 1);
  std::memcpy(error, text.data(), n);
  error[n] = 0;
}

// ponytail: one lock around generation; parts generate one at a time. Per-name locks if a board
// with many packages opens too slowly.
std::mutex lock;
std::map<std::string, std::pair<std::vector<cc_part_triangle>, std::string>> parts;

} // namespace

size_t cc_parts3d_key(const char *model_path, char *out, size_t capacity) {
  if (!model_path) return 0;
  const std::string key = corvcad::parts3d::cache_key(corvcad::parts3d::model_name(model_path));
  set_error(out, capacity, key);
  return key.size();
}

void cc_parts3d_clear(void) {
  std::lock_guard guard(lock);
  parts.clear();
}

size_t cc_parts3d_triangles(const char *model_path, cc_part_triangle *out, size_t capacity, char *error,
                            size_t error_capacity) {
  if (!model_path) return 0;
  const std::string name = corvcad::parts3d::model_name(model_path);
  std::lock_guard guard(lock);
  auto found = parts.find(name);
  if (found == parts.end()) {
    std::vector<cc_part_triangle> triangles;
    std::string why;
    try {
      if (const auto part = corvcad::parts3d::generate(name, &why)) triangles = tessellate(*part);
      else if (why.empty()) why = "no generator for " + name;
    } catch (const Standard_Failure &failure) {
      why = std::string("OCCT: ") + failure.GetMessageString();
    } catch (const std::exception &failure) {
      why = failure.what();
    }
    if (triangles.empty() && why.empty()) why = "empty part " + name;
    found = parts.emplace(name, std::make_pair(std::move(triangles), std::move(why))).first;
  }
  const auto &[triangles, why] = found->second;
  if (triangles.empty()) { set_error(error, error_capacity, why); return 0; }
  if (out) std::copy_n(triangles.begin(), std::min(capacity, triangles.size()), out);
  return triangles.size();
}

// Editor core test: every shape, JSON round trip, export through BuildPack.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "build.h"
#include "scene.h"
using namespace editor;
int main(int argc, char** argv) {
  Map m = NewMap();
  m.name = "Editor Test";
  int conc = AddBuiltinTexture(m, "concrete"), haz = AddBuiltinTexture(m, "hazard"), metal = AddBuiltinTexture(m, "metal"), wood = AddBuiltinTexture(m, "planks");
  Object o;
  o.shape = Shape::Box; o.name = "Tower"; o.pos = {0, 0, 0}; o.size = {6, 3, 6}; o.texture = conc; m.objects.push_back(o);
  o.shape = Shape::Ramp; o.name = "Ramp"; o.pos = {0, 0, -7}; o.size = {3, 3, 8}; o.rot = {0, 0, 0}; o.texture = haz; m.objects.push_back(o);
  o.shape = Shape::Stairs; o.name = "Stairs"; o.pos = {10, 0, 0}; o.size = {3, 3, 6}; o.rot = {0, 90, 0}; o.steps = 10; o.texture = wood; m.objects.push_back(o);
  o.shape = Shape::Cylinder; o.name = "Pillar"; o.pos = {-10, 0, 8}; o.size = {2, 5, 2}; o.rot = {0, 0, 0}; o.texture = metal; m.objects.push_back(o);
  o.shape = Shape::Wedge; o.name = "Roof"; o.pos = {-10, 0, -10}; o.size = {6, 2, 4}; o.rot = {0, 45, 0}; o.texture = conc; m.objects.push_back(o);
  MarkerObj w; w.kind = mapconv::MarkerKind::Weapon; w.type = "rpg_launcher"; w.pos = {0, 3.1f, 0}; m.markers.push_back(w);
  MarkerObj t; t.team = 1; t.pos = {-17, 0, -17}; t.yaw = 45; m.markers.push_back(t);
  // every triangle of a convex shape must face away from the shape's centre (front = cross(p1-p0, p2-p0))
  int bad = 0, total = 0;
  for (auto& ob : m.objects) {
    if (ob.shape == Shape::Stairs) continue;
    V3 c = Rotate(ob.rot, V3{0, ob.size[1] / 3, 0});
    for (int k = 0; k < 3; ++k) c[k] += ob.pos[k];
    for (auto& t : ObjectTriangles(ob)) {
      float e1[3], e2[3], cr[3], ctr[3];
      for (int k = 0; k < 3; ++k) { e1[k] = t.p[1][k] - t.p[0][k]; e2[k] = t.p[2][k] - t.p[0][k]; ctr[k] = (t.p[0][k] + t.p[1][k] + t.p[2][k]) / 3 - c[k]; }
      cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
      float d = cr[0] * ctr[0] + cr[1] * ctr[1] + cr[2] * ctr[2], dn = cr[0] * t.n[0][0] + cr[1] * t.n[0][1] + cr[2] * t.n[0][2];
      ++total;
      if (d <= 0 || dn <= 0) { ++bad; std::printf("  inward face on %s\n", ob.name.c_str()); }
    }
  }
  std::printf("face orientation: %d of %d triangles wrong\n", bad, total);
  std::string out = argc > 2 ? argv[2] : "editor_test_out";
  std::filesystem::create_directories(out);
  std::string js = SaveMap(m, out);
  Map m2 = LoadMap(js, out);
  bool same = SaveMap(m2, out) == js;
  std::printf("json round trip %s, %zu objects, %zu markers, %zu textures\n", same ? "identical" : "DIFFERENT", m2.objects.size(), m2.markers.size(), m2.textures.size());
  std::ofstream(out + "/editor_test.srmap") << js;
  if (argc < 2) { std::printf("(no packfiles folder given: export skipped)\n"); return same ? 0 : 1; }
  auto in = ToMapInput(m2);
  mapconv::MapSettings st; st.display_name = m2.name; st.map_id = "mpx_editor_test"; st.folder = "EditorTest";
  auto rep = mapconv::BuildPack(in, st, argv[1], out + "/maps", [](const std::string& s) { std::printf("%s\n", s.c_str()); });
  std::printf("pack: %zu tris, %zu spawns, %zu weapons\n", rep.triangles, rep.spawns, rep.weapons);
  for (auto& x : rep.warnings) std::printf("  note: %s\n", x.c_str());
  return same ? 0 : 1;
}

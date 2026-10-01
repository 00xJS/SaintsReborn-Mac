#include "scene.h"

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>

#include "json.hpp"
#include "textures.h"

namespace editor {
namespace fs = std::filesystem;
using json = nlohmann::json;
using mapconv::MarkerKind;

const char* ShapeName(Shape s) {
  switch (s) {
    case Shape::Box: return "Box";
    case Shape::Ramp: return "Ramp";
    case Shape::Stairs: return "Stairs";
    case Shape::Cylinder: return "Cylinder";
    case Shape::Wedge: return "Wedge";
    case Shape::Model: return "Model";
  }
  return "?";
}

const char* MarkerLabel(const MarkerObj& m) {
  switch (m.kind) {
    case MarkerKind::Spawn: return m.team == 1 ? "Red spawn" : m.team == 2 ? "Blue spawn" : "Spawn";
    case MarkerKind::PlayerStart: return "Player start";
    case MarkerKind::Weapon: return "Weapon";
    case MarkerKind::Vehicle: return "Vehicle";
    case MarkerKind::ChainsDropOff: return "Chains drop-off";
  }
  return "?";
}

namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;
V3 Add(V3 a, V3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 Sub(V3 a, V3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 Mul(V3 a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
V3 Cross(V3 a, V3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
float Dot(V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Norm(V3 v) {
  float l = std::sqrt(Dot(v, v));
  return l > 1e-12f ? Mul(v, 1 / l) : V3{0, 1, 0};
}

// Faces in object space (before scale); each polygon is convex, listed with its outward normal.
struct Poly {
  std::vector<V3> p;
  V3 inside;  // a point inside the solid the face belongs to: the face is turned away from it
};

void BoxPolys(std::vector<Poly>& out, V3 mn, V3 mx) {
  float x0 = mn[0], y0 = mn[1], z0 = mn[2], x1 = mx[0], y1 = mx[1], z1 = mx[2];
  V3 c{(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
  out.push_back({{{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}, c});  // top
  out.push_back({{{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}, c});  // bottom
  out.push_back({{{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}}, c});  // -z
  out.push_back({{{x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}}, c});  // +z
  out.push_back({{{x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}}, c});  // -x
  out.push_back({{{x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}}, c});  // +x
}

std::vector<Poly> ShapePolys(const Object& o) {
  std::vector<Poly> P;
  float hx = o.size[0] / 2, h = o.size[1], hz = o.size[2] / 2;
  switch (o.shape) {
    case Shape::Box:
      BoxPolys(P, {-hx, 0, -hz}, {hx, h, hz});
      break;
    case Shape::Ramp: {  // rises towards +Z
      V3 c{0, h / 3, hz / 3};
      P.push_back({{{-hx, 0, -hz}, {-hx, h, hz}, {hx, h, hz}, {hx, 0, -hz}}, c});           // slope
      P.push_back({{{-hx, 0, -hz}, {hx, 0, -hz}, {hx, 0, hz}, {-hx, 0, hz}}, c});           // bottom
      P.push_back({{{hx, 0, hz}, {hx, h, hz}, {-hx, h, hz}, {-hx, 0, hz}}, c});             // back (+z)
      P.push_back({{{-hx, 0, hz}, {-hx, h, hz}, {-hx, 0, -hz}}, c});                        // -x side
      P.push_back({{{hx, 0, -hz}, {hx, h, hz}, {hx, 0, hz}}, c});                           // +x side
      break;
    }
    case Shape::Wedge: {  // triangular prism: ridge along X at the top centre (a roof / divider)
      V3 c{0, h / 3, 0};
      P.push_back({{{-hx, 0, -hz}, {-hx, h, 0}, {hx, h, 0}, {hx, 0, -hz}}, c});
      P.push_back({{{hx, 0, hz}, {hx, h, 0}, {-hx, h, 0}, {-hx, 0, hz}}, c});
      P.push_back({{{-hx, 0, -hz}, {hx, 0, -hz}, {hx, 0, hz}, {-hx, 0, hz}}, c});
      P.push_back({{{-hx, 0, hz}, {-hx, h, 0}, {-hx, 0, -hz}}, c});
      P.push_back({{{hx, 0, -hz}, {hx, h, 0}, {hx, 0, hz}}, c});
      break;
    }
    case Shape::Stairs: {  // steps rising towards +Z
      int n = std::max(1, o.steps);
      for (int i = 0; i < n; ++i) {
        float za = -hz + o.size[2] * i / n, top = h * (i + 1) / n;
        BoxPolys(P, {-hx, 0, za}, {hx, top, hz});
      }
      break;
    }
    case Shape::Cylinder: {
      const int seg = 24;
      std::vector<V3> ring;
      for (int i = 0; i < seg; ++i) {
        float a = 2 * 3.14159265f * i / seg;
        ring.push_back({hx * std::sin(a), 0, hz * std::cos(a)});
      }
      Poly top, bottom;
      top.inside = bottom.inside = V3{0, h / 2, 0};
      for (int i = 0; i < seg; ++i) {
        top.p.push_back({ring[size_t(i)][0], h, ring[size_t(i)][2]});
        bottom.p.push_back(ring[size_t(seg - 1 - i)]);
      }
      P.push_back(top);
      P.push_back(bottom);
      for (int i = 0; i < seg; ++i) {
        V3 a = ring[size_t(i)], b = ring[size_t((i + 1) % seg)];
        P.push_back({{a, {a[0], h, a[2]}, {b[0], h, b[2]}, b}, V3{0, h / 2, 0}});
      }
      break;
    }
    case Shape::Model:
      break;
  }
  return P;
}

}  // namespace

V3 Rotate(const V3& r, const V3& v) {
  // yaw (Y) * pitch (X) * roll (Z), applied to v
  float cx = std::cos(r[0] * kDeg), sx = std::sin(r[0] * kDeg), cy = std::cos(r[1] * kDeg), sy = std::sin(r[1] * kDeg),
        cz = std::cos(r[2] * kDeg), sz = std::sin(r[2] * kDeg);
  V3 a{v[0] * cz - v[1] * sz, v[0] * sz + v[1] * cz, v[2]};          // roll
  V3 b{a[0], a[1] * cx - a[2] * sx, a[1] * sx + a[2] * cx};          // pitch
  return {b[0] * cy + b[2] * sy, b[1], -b[0] * sy + b[2] * cy};       // yaw
}

namespace {
ModelSource g_models;
}

void SetModelSource(ModelSource source) { g_models = std::move(source); }

const mapconv::MapInput* FindModel(const std::string& asset) { return g_models ? g_models(asset) : nullptr; }

std::string ModelLabel(const std::string& asset) {
  size_t dot = asset.find('.');
  return dot == std::string::npos ? asset : asset.substr(0, dot);
}

std::vector<std::pair<Tri, int>> ModelTriangles(const Object& o) {
  std::vector<std::pair<Tri, int>> out;
  if (o.shape != Shape::Model) return out;
  const mapconv::MapInput* in = FindModel(o.asset);
  if (!in) return out;
  V3 s = o.scale;
  for (auto& k : s) k = std::max(k, 0.001f);
  out.reserve(in->tris.size());
  for (auto& it : in->tris) {
    Tri t;
    for (int k = 0; k < 3; ++k) {
      t.p[k] = Add(Rotate(o.rot, V3{it.p[k][0] * s[0], it.p[k][1] * s[1], it.p[k][2] * s[2]}), o.pos);
      t.n[k] = Norm(Rotate(o.rot, V3{it.n[k][0] / s[0], it.n[k][1] / s[1], it.n[k][2] / s[2]}));
      t.uv[k][0] = it.uv[k][0];
      t.uv[k][1] = it.uv[k][1];
    }
    out.push_back({t, it.material});
  }
  return out;
}

std::vector<Tri> ObjectTriangles(const Object& o) {
  std::vector<Tri> out;
  if (o.shape == Shape::Model) {
    for (auto& t : ModelTriangles(o)) out.push_back(t.first);
    return out;
  }
  auto polys = ShapePolys(o);
  float s = o.uv_metres > 0.01f ? o.uv_metres : 1.0f;
  for (auto& poly : polys) {
    std::vector<V3> w;
    for (auto& p : poly.p) w.push_back(Add(Rotate(o.rot, p), o.pos));
    // outward normal of the polygon (Newell), orientation as listed
    V3 n{0, 0, 0};
    for (size_t i = 0; i < w.size(); ++i) {
      const V3 &a = w[i], &b = w[(i + 1) % w.size()];
      n[0] += (a[1] - b[1]) * (a[2] + b[2]);
      n[1] += (a[2] - b[2]) * (a[0] + b[0]);
      n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    n = Norm(n);
    if (Dot(n, n) < 0.5f) continue;
    V3 centroid{0, 0, 0};
    for (auto& q : w) centroid = Add(centroid, Mul(q, 1.0f / float(w.size())));
    V3 inside = Add(Rotate(o.rot, poly.inside), o.pos);
    if (Dot(n, Sub(centroid, inside)) < 0) n = Mul(n, -1.0f);   // outward
    // world-space box mapping: dominant normal axis picks the plane; textures line up across objects
    float ax = std::abs(n[0]), ay = std::abs(n[1]), az = std::abs(n[2]);
    auto uv = [&](const V3& p, float* t) {
      if (ay >= ax && ay >= az) { t[0] = p[0] / s; t[1] = p[2] / s; }
      else if (ax >= az) { t[0] = p[2] / s; t[1] = -p[1] / s; }
      else { t[0] = p[0] / s; t[1] = -p[1] / s; }
    };
    for (size_t i = 1; i + 1 < w.size(); ++i) {
      Tri t;
      V3 q[3] = {w[0], w[i], w[i + 1]};
      if (Dot(Cross(Sub(q[1], q[0]), Sub(q[2], q[0])), n) < 0) std::swap(q[1], q[2]);
      for (int k = 0; k < 3; ++k) {
        t.p[k] = q[k];
        t.n[k] = n;
        uv(q[k], t.uv[k]);
      }
      if (Dot(Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0])), Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0]))) < 1e-10f) continue;
      out.push_back(t);
    }
  }
  if (o.shape == Shape::Cylinder) {  // smooth sides
    for (auto& t : out)
      if (std::abs(t.n[0][1]) < 0.5f)
        for (int k = 0; k < 3; ++k) {
          V3 local = Sub(t.p[k], o.pos);
          V3 r = Rotate(o.rot, V3{0, 1, 0});
          V3 radial = Sub(local, Mul(r, Dot(local, r)));
          t.n[k] = Norm(radial);
        }
  }
  return out;
}

Map NewMap() {
  Map m;
  m.name = "My Map";
  int grid = AddBuiltinTexture(m, "grid"), brick = AddBuiltinTexture(m, "brick");
  Object floor;
  floor.name = "Floor";
  floor.pos = {0, -0.5f, 0};
  floor.size = {40, 0.5f, 40};
  floor.texture = grid;
  floor.uv_metres = 4;
  m.objects.push_back(floor);
  const float H = 20;
  const V3 walls[4][2] = {{{0, 0, -H}, {41, 5, 1}}, {{0, 0, H}, {41, 5, 1}}, {{-H, 0, 0}, {1, 5, 39}}, {{H, 0, 0}, {1, 5, 39}}};
  for (int i = 0; i < 4; ++i) {
    Object w;
    w.name = "Wall " + std::to_string(i + 1);
    w.pos = walls[i][0];
    w.size = walls[i][1];
    w.texture = brick;
    w.uv_metres = 4;
    m.objects.push_back(w);
  }
  const float S = 15;
  const V3 sp[4] = {{-S, 0, -S}, {S, 0, S}, {-S, 0, S}, {S, 0, -S}};
  for (auto& p : sp) {
    MarkerObj mk;
    mk.pos = p;
    mk.yaw = std::atan2(-p[0], -p[2]) / kDeg;
    m.markers.push_back(mk);
  }
  return m;
}

int AddGameTexture(Map& m, const std::string& name) {
  for (size_t i = 0; i < m.textures.size(); ++i)
    if (m.textures[i].game == name) return int(i);
  TextureRef t;
  t.game = name;
  size_t dot = name.rfind('.');
  t.name = dot == std::string::npos ? name : name.substr(0, dot);
  t.image = MakeBuiltinTexture("grey");
  t.pending = true;
  m.textures.push_back(std::move(t));
  return int(m.textures.size() - 1);
}

int AddBuiltinTexture(Map& m, const std::string& id) {
  for (size_t i = 0; i < m.textures.size(); ++i)
    if (m.textures[i].builtin == id) return int(i);
  TextureRef t;
  t.builtin = id;
  for (auto& b : BuiltinTextures())
    if (id == b.id) t.name = b.label;
  t.image = MakeBuiltinTexture(id);
  m.textures.push_back(std::move(t));
  return int(m.textures.size() - 1);
}

static const char* KindId(MarkerKind k) {
  switch (k) {
    case MarkerKind::Spawn: return "spawn";
    case MarkerKind::PlayerStart: return "player_start";
    case MarkerKind::Weapon: return "weapon";
    case MarkerKind::Vehicle: return "vehicle";
    case MarkerKind::ChainsDropOff: return "chains_dropoff";
  }
  return "spawn";
}

std::string SaveMap(const Map& m, const std::string& base_dir) {
  json j;
  j["format"] = "saints-reborn-map";
  j["version"] = 1;
  j["name"] = m.name;
  for (auto& t : m.textures) {
    json x{{"name", t.name}};
    if (!t.builtin.empty()) x["builtin"] = t.builtin;
    if (!t.game.empty()) x["game"] = t.game;
    if (!t.file.empty()) {
      std::error_code ec;
      auto rel = fs::relative(fs::u8path(t.file), fs::u8path(base_dir), ec);
      x["file"] = (!ec && !rel.empty() && rel.native()[0] != '.') ? rel.generic_u8string() : t.file;
    }
    j["textures"].push_back(x);
  }
  for (auto& o : m.objects) {
    json x{{"shape", ShapeName(o.shape)}, {"name", o.name}, {"pos", o.pos}, {"rot", o.rot}, {"size", o.size},
           {"texture", o.texture}, {"uv_metres", o.uv_metres}, {"steps", o.steps}, {"collision", o.collision}};
    if (o.shape == Shape::Model) {
      x["asset"] = o.asset;  // a name only: the model itself stays in the player's game files
      x["scale"] = o.scale;
    }
    j["objects"].push_back(x);
  }
  for (auto& k : m.markers)
    j["markers"].push_back({{"kind", KindId(k.kind)}, {"pos", k.pos}, {"yaw", k.yaw}, {"team", k.team}, {"type", k.type}});
  return j.dump(1, '\t');
}

Map LoadMap(const std::string& text, const std::string& base_dir) {
  json j = json::parse(text);
  if (j.value("format", "") != "saints-reborn-map") throw std::runtime_error("not a Saints Reborn map file");
  Map m;
  m.name = j.value("name", "My Map");
  for (auto& x : j.value("textures", json::array())) {
    TextureRef t;
    t.name = x.value("name", "");
    t.builtin = x.value("builtin", "");
    t.file = x.value("file", "");
    t.game = x.value("game", "");
    if (!t.file.empty() && fs::u8path(t.file).is_relative()) t.file = (fs::u8path(base_dir) / fs::u8path(t.file)).u8string();
    m.textures.push_back(t);
  }
  for (auto& x : j.value("objects", json::array())) {
    Object o;
    std::string s = x.value("shape", "Box");
    for (Shape sh : {Shape::Box, Shape::Ramp, Shape::Stairs, Shape::Cylinder, Shape::Wedge, Shape::Model})
      if (s == ShapeName(sh)) o.shape = sh;
    o.name = x.value("name", "");
    o.pos = x.value("pos", o.pos);
    o.rot = x.value("rot", o.rot);
    o.size = x.value("size", o.size);
    o.texture = x.value("texture", 0);
    o.uv_metres = x.value("uv_metres", 2.0f);
    o.steps = x.value("steps", 8);
    o.collision = x.value("collision", true);
    o.asset = x.value("asset", "");
    o.scale = x.value("scale", o.scale);
    m.objects.push_back(o);
  }
  for (auto& x : j.value("markers", json::array())) {
    MarkerObj k;
    std::string kind = x.value("kind", "spawn");
    for (MarkerKind mk : {MarkerKind::Spawn, MarkerKind::PlayerStart, MarkerKind::Weapon, MarkerKind::Vehicle, MarkerKind::ChainsDropOff})
      if (kind == KindId(mk)) k.kind = mk;
    k.pos = x.value("pos", k.pos);
    k.yaw = x.value("yaw", 0.0f);
    k.team = x.value("team", 0);
    k.type = x.value("type", "");
    m.markers.push_back(k);
  }
  LoadTextures(m, base_dir);
  return m;
}

void LoadTextures(Map& m, const std::string&) {
  for (auto& t : m.textures) {
    if (t.image.w) continue;
    if (!t.game.empty()) {  // filled in by the editor from the game files
      t.image = MakeBuiltinTexture("grey");
      t.pending = true;
      continue;
    }
    if (!t.file.empty()) t.image = LoadImageFile(t.file);
    if (!t.image.w && !t.builtin.empty()) t.image = MakeBuiltinTexture(t.builtin);
    if (!t.image.w) t.image = MakeBuiltinTexture("grey");  // missing file: keep the map usable
  }
}

mapconv::MapInput ToMapInput(const Map& m) {
  mapconv::MapInput in;
  std::vector<int> used(m.textures.size(), -1);
  std::map<std::pair<std::string, int>, int> model_mats;  // (asset, model material) -> material
  for (auto& o : m.objects) {
    if (o.shape == Shape::Model) {
      const mapconv::MapInput* src = FindModel(o.asset);
      if (!src) {
        in.warnings.push_back("model " + o.asset + " could not be loaded from the game files and was left out");
        continue;
      }
      for (auto& [t, mat] : ModelTriangles(o)) {
        if (mat < 0 || size_t(mat) >= src->materials.size()) continue;
        auto key = std::make_pair(o.asset, mat);
        auto it = model_mats.find(key);
        if (it == model_mats.end()) {
          it = model_mats.emplace(key, int(in.materials.size())).first;
          in.materials.push_back({ModelLabel(o.asset) + "_" + src->materials[size_t(mat)].name, src->materials[size_t(mat)].image});
        }
        mapconv::InTriangle tri;
        for (int k = 0; k < 3; ++k) {
          tri.p[k] = t.p[k];
          tri.n[k] = t.n[k];
          tri.uv[k][0] = t.uv[k][0];
          tri.uv[k][1] = t.uv[k][1];
        }
        tri.material = it->second;
        in.tris.push_back(tri);
      }
      continue;
    }
    if (o.texture < 0 || size_t(o.texture) >= m.textures.size()) continue;
    if (used[size_t(o.texture)] < 0) {
      used[size_t(o.texture)] = int(in.materials.size());
      in.materials.push_back({m.textures[size_t(o.texture)].name, m.textures[size_t(o.texture)].image});
    }
    for (auto& t : ObjectTriangles(o)) {
      mapconv::InTriangle it;
      for (int k = 0; k < 3; ++k) {
        it.p[k] = t.p[k];
        it.n[k] = t.n[k];
        it.uv[k][0] = t.uv[k][0];
        it.uv[k][1] = t.uv[k][1];
      }
      it.material = used[size_t(o.texture)];
      in.tris.push_back(it);
    }
  }
  for (auto& k : m.markers) {
    mapconv::Marker mk;
    mk.kind = k.kind;
    mk.pos = k.pos;
    mk.yaw = k.yaw * kDeg;
    mk.team = k.team;
    mk.type = k.type;
    mk.source = MarkerLabel(k);
    in.markers.push_back(mk);
  }
  return in;
}

}  // namespace editor

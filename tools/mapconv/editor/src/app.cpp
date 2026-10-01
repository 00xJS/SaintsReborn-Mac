#include "app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "build.h"
#include "gltf_import.h"
#include "textures.h"

namespace editor {
namespace fs = std::filesystem;
using namespace DirectX;

App* g_app = nullptr;

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
constexpr int kThumbSize = 96;     // texture previews in the asset browser
constexpr int kFullSize = 512;     // textures on objects (the export scales them again)
constexpr size_t kMaxTextureThumbs = 700;

bool SameObject(const Object& a, const Object& b) {
  return a.shape == b.shape && a.pos == b.pos && a.rot == b.rot && a.size == b.size && a.texture == b.texture &&
         a.uv_metres == b.uv_metres && a.steps == b.steps && a.scale == b.scale && a.asset == b.asset;
}

bool RayTri(const float o[3], const float d[3], const V3& a, const V3& b, const V3& c, float& t) {
  float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
  float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::abs(det) < 1e-9f) return false;
  float inv = 1 / det, s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
  float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return false;
  float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
  float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return false;
  t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  return t > 0;
}

bool RayBox(const float o[3], const float d[3], const float mn[3], const float mx[3], float& t) {
  float t0 = 0, t1 = 1e30f;
  for (int a = 0; a < 3; ++a) {
    if (std::abs(d[a]) < 1e-12f) {
      if (o[a] < mn[a] || o[a] > mx[a]) return false;
      continue;
    }
    float i0 = (mn[a] - o[a]) / d[a], i1 = (mx[a] - o[a]) / d[a];
    if (i0 > i1) std::swap(i0, i1);
    t0 = std::max(t0, i0);
    t1 = std::min(t1, i1);
    if (t0 > t1) return false;
  }
  t = t0;
  return true;
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

XMMATRIX Projection(float aspect) { return XMMatrixPerspectiveFovLH(60 * kDeg, aspect, 0.1f, 3000.0f); }

}  // namespace

// Size of a marker in the viewport (a standing character, a weapon on the floor, a car)
void MarkerBox(const MarkerObj& m, float mn[3], float mx[3]) {
  float w = 0.6f, h = 1.8f, d = 0.6f;
  if (m.kind == mapconv::MarkerKind::Weapon) { w = 0.8f; h = 0.3f; d = 0.3f; }
  if (m.kind == mapconv::MarkerKind::Vehicle) { w = 2.0f; h = 1.4f; d = 4.6f; }
  if (m.kind == mapconv::MarkerKind::ChainsDropOff) { w = 1.4f; h = 0.2f; d = 1.4f; }
  mn[0] = m.pos[0] - w / 2; mn[1] = m.pos[1]; mn[2] = m.pos[2] - d / 2;
  mx[0] = m.pos[0] + w / 2; mx[1] = m.pos[1] + h; mx[2] = m.pos[2] + d / 2;
}

uint32_t MarkerColour(const MarkerObj& m) {
  using K = mapconv::MarkerKind;
  switch (m.kind) {
    case K::Spawn: return m.team == 1 ? 0xFF3040FFu : m.team == 2 ? 0xFFFF7040u : 0xFF50FF40u;  // ABGR
    case K::PlayerStart: return 0xFFFFFFFFu;
    case K::Weapon: return 0xFF20CCFFu;
    case K::Vehicle: return 0xFFFF66CCu;
    case K::ChainsDropOff: return 0xFF208CFFu;
  }
  return 0xFFFFFFFFu;
}

float Camera::Forward(int i) const {
  float f[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
  return f[i];
}

XMMATRIX Camera::View() const {
  return XMMatrixLookToLH(XMVectorSet(pos[0], pos[1], pos[2], 1), XMVectorSet(Forward(0), Forward(1), Forward(2), 0),
                          XMVectorSet(0, 1, 0, 0));
}

fs::path FindGameDir(const fs::path& start) {
  for (fs::path p = start; !p.empty(); p = p.parent_path()) {
    fs::path g = ResolveGameDir(p);
    if (!g.empty()) return g;
    if (p == p.parent_path()) break;
  }
  return {};
}

fs::path BrowseForFolder(HWND owner, const fs::path& initial) {
  fs::path out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  dlg->SetTitle(L"Choose your Saints Reborn folder");
  if (!initial.empty()) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
      dlg->SetFolder(item);
      item->Release();
    }
  }
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      wchar_t* p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = fs::path(p);
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// ---------------------------------------------------------------- undo

void App::PushUndo() {
  undo.push_back({map.name, map.objects, map.markers});
  if (undo.size() > 200) undo.pop_front();
  redo.clear();
  MarkDirty();
}

void App::Undo() {
  if (undo.empty()) return;
  redo.push_back({map.name, map.objects, map.markers});
  auto& s = undo.back();
  map.name = s.name;
  map.objects = s.objects;
  map.markers = s.markers;
  undo.pop_back();
  sel.Clear();
  MarkDirty();
}

void App::Redo() {
  if (redo.empty()) return;
  undo.push_back({map.name, map.objects, map.markers});
  auto& s = redo.back();
  map.name = s.name;
  map.objects = s.objects;
  map.markers = s.markers;
  redo.pop_back();
  sel.Clear();
  MarkDirty();
}

// ---------------------------------------------------------------- GPU state

void App::SyncGpu() {
  while (gpu_textures.size() < map.textures.size()) {
    gpu_textures.push_back(r.CreateTexture(map.textures[gpu_textures.size()].image));
    gpu_texture_src.push_back(map.textures[gpu_texture_src.size()].image.rgb.data());
  }
  for (size_t i = 0; i < map.textures.size(); ++i)
    if (gpu_texture_src[i] != map.textures[i].image.rgb.data()) {
      r.UpdateTexture(gpu_textures[i], map.textures[i].image);
      gpu_texture_src[i] = map.textures[i].image.rgb.data();
    }
  meshes.resize(map.objects.size());
  for (size_t i = 0; i < map.objects.size(); ++i) {
    auto& c = meshes[i];
    const Object& o = map.objects[i];
    const ModelData* md = o.shape == Shape::Model ? Model(o.asset) : nullptr;
    const void* model_key = md && md->loaded ? static_cast<const void*>(md) : nullptr;
    if (c.valid && SameObject(c.key, o) && c.model == model_key) continue;
    c.key = o;
    c.model = model_key;
    c.valid = true;
    c.parts.clear();
    c.tris.clear();
    auto add_vertex = [](std::vector<MeshVertex>& v, const Tri& t) {
      for (int k = 0; k < 3; ++k)
        v.push_back({{t.p[k][0], t.p[k][1], t.p[k][2]}, {t.n[k][0], t.n[k][1], t.n[k][2]}, {t.uv[k][0], t.uv[k][1]}});
    };
    if (o.shape == Shape::Model) {
      if (model_key) {
        std::vector<std::vector<MeshVertex>> per(md->in.materials.size());
        for (auto& [t, mat] : ModelTriangles(o)) {
          c.tris.push_back(t);
          if (mat >= 0 && size_t(mat) < per.size()) add_vertex(per[size_t(mat)], t);
        }
        for (size_t m = 0; m < per.size(); ++m)
          if (!per[m].empty()) c.parts.push_back({r.CreateMesh(per[m]), -1, md->gpu_textures[m]});
      }
    } else {
      c.tris = ObjectTriangles(o);
      std::vector<MeshVertex> v;
      v.reserve(c.tris.size() * 3);
      for (auto& t : c.tris) add_vertex(v, t);
      c.parts.push_back({r.CreateMesh(v), o.texture, -1});
    }
    for (int a = 0; a < 3; ++a) { c.mn[a] = 1e30f; c.mx[a] = -1e30f; }
    for (auto& t : c.tris)
      for (auto& p : t.p)
        for (int a = 0; a < 3; ++a) { c.mn[a] = std::min(c.mn[a], p[a]); c.mx[a] = std::max(c.mx[a], p[a]); }
    if (c.tris.empty())  // model still loading: a 1 m box
      for (int a = 0; a < 3; ++a) { c.mn[a] = o.pos[a] - 0.5f; c.mx[a] = o.pos[a] + 0.5f; }
  }
}

// ---------------------------------------------------------------- editing

void App::DeleteSelection() {
  if (sel.Empty()) return;
  PushUndo();
  std::vector<int> objs, marks;
  for (auto& s : sel.items) (s.kind == SelItem::Obj ? objs : marks).push_back(s.index);
  std::sort(objs.rbegin(), objs.rend());
  std::sort(marks.rbegin(), marks.rend());
  for (int i : objs)
    if (i >= 0 && size_t(i) < map.objects.size()) {
      map.objects.erase(map.objects.begin() + i);
      if (size_t(i) < meshes.size()) meshes.erase(meshes.begin() + i);
    }
  for (int i : marks)
    if (i >= 0 && size_t(i) < map.markers.size()) map.markers.erase(map.markers.begin() + i);
  status = "Deleted " + std::to_string(objs.size() + marks.size()) + (objs.size() + marks.size() == 1 ? " item" : " items");
  sel.Clear();
}

void App::CopySelection() {
  clipboard = {};
  for (auto& s : sel.items) {
    if (s.kind == SelItem::Obj) clipboard.objects.push_back(Obj(s.index));
    else clipboard.markers.push_back(Mark(s.index));
  }
  if (!sel.Empty()) status = "Copied " + std::to_string(sel.Count()) + (sel.Count() == 1 ? " item" : " items");
}

void App::Paste() {
  if (clipboard.objects.empty() && clipboard.markers.empty()) return;
  PushUndo();
  // pasted things keep their layout and land under the screen centre
  float c[3] = {0, 0, 0}, n = 0, target[3];
  for (auto& o : clipboard.objects) { for (int a = 0; a < 3; ++a) c[a] += o.pos[a]; ++n; }
  for (auto& m : clipboard.markers) { for (int a = 0; a < 3; ++a) c[a] += m.pos[a]; ++n; }
  for (float& x : c) x /= n;
  float lowest = 1e30f;
  for (auto& o : clipboard.objects) lowest = std::min(lowest, o.pos[1]);
  for (auto& m : clipboard.markers) lowest = std::min(lowest, m.pos[1]);
  PlacementPoint(target);
  float off[3] = {target[0] - c[0], target[1] - lowest, target[2] - c[2]};
  sel.Clear();
  for (auto o : clipboard.objects) {
    for (int a = 0; a < 3; ++a) o.pos[size_t(a)] += off[a];
    map.objects.push_back(o);
    sel.Add({SelItem::Obj, int(map.objects.size() - 1)});
  }
  for (auto m : clipboard.markers) {
    for (int a = 0; a < 3; ++a) m.pos[size_t(a)] += off[a];
    map.markers.push_back(m);
    sel.Add({SelItem::Mark, int(map.markers.size() - 1)});
  }
  status = "Pasted " + std::to_string(sel.Count()) + (sel.Count() == 1 ? " item" : " items");
}

void App::DuplicateSelection() {
  if (sel.Empty()) return;
  PushUndo();
  // like Unity: the copies sit on top of the originals and become the selection, ready to move
  Selection copies;
  for (auto& s : sel.items) {
    if (s.kind == SelItem::Obj) {
      Object o = Obj(s.index);
      map.objects.push_back(o);
      copies.Add({SelItem::Obj, int(map.objects.size() - 1)});
    } else {
      map.markers.push_back(Mark(s.index));
      copies.Add({SelItem::Mark, int(map.markers.size() - 1)});
    }
  }
  sel = copies;
  status = "Duplicated " + std::to_string(sel.Count()) + (sel.Count() == 1 ? " item" : " items") + " (in place: move them with the gizmo)";
}

void App::SelectAll() {
  sel.Clear();
  for (size_t i = 0; i < map.objects.size(); ++i) sel.Add({SelItem::Obj, int(i)});
  for (size_t i = 0; i < map.markers.size(); ++i) sel.Add({SelItem::Mark, int(i)});
}

bool App::MouseRay(float mx, float my, float o[3], float d[3]) const {
  XMMATRIX vp = cam.View() * Projection(float(width) / float(std::max(height, 1)));
  XMMATRIX inv = XMMatrixInverse(nullptr, vp);
  float nx = 2 * mx / width - 1, ny = 1 - 2 * my / height;
  XMVECTOR a = XMVector3TransformCoord(XMVectorSet(nx, ny, 0, 1), inv), b = XMVector3TransformCoord(XMVectorSet(nx, ny, 1, 1), inv);
  XMFLOAT3 A, B;
  XMStoreFloat3(&A, a);
  XMStoreFloat3(&B, b);
  o[0] = A.x; o[1] = A.y; o[2] = A.z;
  d[0] = B.x - A.x; d[1] = B.y - A.y; d[2] = B.z - A.z;
  float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (l < 1e-12f) return false;
  for (int k = 0; k < 3; ++k) d[k] /= l;
  return true;
}

bool App::RayHit(const float o[3], const float d[3], float& best, SelItem* hit, const Selection* skip) const {
  best = 1e30f;
  bool any = false;
  for (size_t i = 0; i < map.markers.size(); ++i) {
    if (skip && skip->Has(SelItem::Mark, int(i))) continue;
    float mn[3], mx[3], t;
    MarkerBox(map.markers[i], mn, mx);
    if (RayBox(o, d, mn, mx, t) && t < best) { best = t; any = true; if (hit) *hit = {SelItem::Mark, int(i)}; }
  }
  for (size_t i = 0; i < meshes.size() && i < map.objects.size(); ++i) {
    if (skip && skip->Has(SelItem::Obj, int(i))) continue;
    const CachedMesh& c = meshes[i];
    float t;
    if (!RayBox(o, d, c.mn, c.mx, t) || t > best) continue;
    if (c.tris.empty()) {  // unloaded model: its box
      if (t < best - 0.05f) { best = t; any = true; if (hit) *hit = {SelItem::Obj, int(i)}; }
      continue;
    }
    for (auto& tr : c.tris) {
      float h;
      if (RayTri(o, d, tr.p[0], tr.p[1], tr.p[2], h) && h < best - 0.05f) { best = h; any = true; if (hit) *hit = {SelItem::Obj, int(i)}; }
    }
  }
  return any;
}

bool App::SurfacePoint(float mx, float my, float out[3]) {
  float o[3], d[3], t;
  if (!MouseRay(mx, my, o, d)) return false;
  bool hit = RayHit(o, d, t, nullptr);
  if (d[1] < -1e-4f && (!hit || -o[1] / d[1] < t)) { t = -o[1] / d[1]; hit = true; }  // the ground plane
  if (!hit || t > 400) t = 15;
  for (int a = 0; a < 3; ++a) out[a] = o[a] + d[a] * t;
  if (snap) {
    out[0] = std::round(out[0] / snap_move) * snap_move;
    out[2] = std::round(out[2] / snap_move) * snap_move;
  }
  return true;
}

bool App::PlacementPoint(float out[3]) { return SurfacePoint(width / 2.0f, height / 2.0f, out); }

void App::AddObject(Shape s, const float* at) {
  PushUndo();
  Object o;
  o.shape = s;
  o.texture = std::clamp(current_texture, 0, int(map.textures.size()) - 1);
  switch (s) {
    case Shape::Box: o.size = {4, 2, 4}; break;
    case Shape::Ramp: o.size = {3, 2, 6}; break;
    case Shape::Stairs: o.size = {3, 3, 5}; o.steps = 12; break;
    case Shape::Cylinder: o.size = {2, 4, 2}; break;
    case Shape::Wedge: o.size = {4, 1.5f, 3}; break;
    case Shape::Model: break;
  }
  o.name = std::string(ShapeName(s)) + " " + std::to_string(map.objects.size() + 1);
  float p[3];
  if (at) std::copy(at, at + 3, p);
  else PlacementPoint(p);
  o.pos = {p[0], p[1], p[2]};
  map.objects.push_back(o);
  sel.Set({SelItem::Obj, int(map.objects.size() - 1)});
  if (tool == ToolView) tool = ToolMove;
}

void App::AddModel(const std::string& asset, const float* at) {
  PushUndo();
  Object o;
  o.shape = Shape::Model;
  o.asset = asset;
  o.name = ModelLabel(asset);
  float p[3];
  if (at) std::copy(at, at + 3, p);
  else PlacementPoint(p);
  o.pos = {p[0], p[1], p[2]};
  // stand it on the surface (models have their origin anywhere)
  if (ModelData* md = Model(asset); md && md->loaded) o.pos[1] -= md->mn[1];
  map.objects.push_back(o);
  sel.Set({SelItem::Obj, int(map.objects.size() - 1)});
  if (tool == ToolView) tool = ToolMove;
  status = "Added " + o.name;
}

void App::AddMarker(mapconv::MarkerKind k, int team, const char* type, const float* at) {
  PushUndo();
  MarkerObj m;
  m.kind = k;
  m.team = team;
  m.type = type;
  float p[3];
  if (at) std::copy(at, at + 3, p);
  else PlacementPoint(p);
  m.pos = {p[0], p[1] + (k == mapconv::MarkerKind::Weapon ? 0.1f : 0.05f), p[2]};
  m.yaw = std::round(std::atan2(-cam.Forward(0), -cam.Forward(2)) / kDeg / 45) * 45;  // face the camera
  map.markers.push_back(m);
  sel.Set({SelItem::Mark, int(map.markers.size() - 1)});
  if (tool == ToolView) tool = ToolMove;
}

void App::Pick(float mx, float my, int mode) {
  float o[3], d[3], t;
  SelItem hit;
  bool any = MouseRay(mx, my, o, d) && RayHit(o, d, t, &hit);
  if (mode == 0) {
    if (any) sel.Set(hit);
    else sel.Clear();
  } else if (any) {
    if (mode == 1) sel.Toggle(hit);
    else sel.Add(hit);
  }
}

bool App::ItemBounds(const SelItem& s, float mn[3], float mx[3]) const {
  if (s.kind == SelItem::Obj) {
    if (s.index < 0 || size_t(s.index) >= meshes.size()) return false;
    std::copy(meshes[size_t(s.index)].mn, meshes[size_t(s.index)].mn + 3, mn);
    std::copy(meshes[size_t(s.index)].mx, meshes[size_t(s.index)].mx + 3, mx);
    return true;
  }
  if (s.index < 0 || size_t(s.index) >= map.markers.size()) return false;
  MarkerBox(map.markers[size_t(s.index)], mn, mx);
  return true;
}

bool App::SelectionBounds(float mn[3], float mx[3]) const {
  bool any = false;
  for (int a = 0; a < 3; ++a) { mn[a] = 1e30f; mx[a] = -1e30f; }
  for (auto& s : sel.items) {
    float a0[3], a1[3];
    if (!ItemBounds(s, a0, a1)) continue;
    any = true;
    for (int a = 0; a < 3; ++a) { mn[a] = std::min(mn[a], a0[a]); mx[a] = std::max(mx[a], a1[a]); }
  }
  return any;
}

void App::MarqueeSelect(const float a[2], const float b[2], bool additive) {
  float x0 = std::min(a[0], b[0]), x1 = std::max(a[0], b[0]), y0 = std::min(a[1], b[1]), y1 = std::max(a[1], b[1]);
  XMMATRIX vp = cam.View() * Projection(float(width) / float(std::max(height, 1)));
  auto inside = [&](const float mn[3], const float mx[3]) {
    XMVECTOR c = XMVectorSet((mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2, 1);
    XMVECTOR h = XMVector4Transform(c, vp);
    float w = XMVectorGetW(h);
    if (w <= 0.1f) return false;
    float sx = (XMVectorGetX(h) / w * 0.5f + 0.5f) * width, sy = (0.5f - XMVectorGetY(h) / w * 0.5f) * height;
    return sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1;
  };
  if (!additive) sel.Clear();
  for (size_t i = 0; i < meshes.size() && i < map.objects.size(); ++i)
    if (inside(meshes[i].mn, meshes[i].mx)) sel.Add({SelItem::Obj, int(i)});
  for (size_t i = 0; i < map.markers.size(); ++i) {
    float mn[3], mx[3];
    MarkerBox(map.markers[i], mn, mx);
    if (inside(mn, mx)) sel.Add({SelItem::Mark, int(i)});
  }
}

void App::FocusSelection() {
  float mn[3], mx[3];
  if (!SelectionBounds(mn, mx)) return;
  float c[3], radius = 1;
  for (int a = 0; a < 3; ++a) {
    c[a] = (mn[a] + mx[a]) / 2;
    radius = std::max(radius, (mx[a] - mn[a]) / 2);
  }
  orbit_dist = radius * 2.4f + 2;
  for (int a = 0; a < 3; ++a) cam.pos[a] = c[a] - cam.Forward(a) * orbit_dist;
}

void App::DropToGround() {
  if (sel.Empty()) return;
  PushUndo();
  for (auto& s : sel.items) {
    float mn[3], mx[3];
    if (!ItemBounds(s, mn, mx)) continue;
    // straight down from just above the item's bottom: the first surface of something else, or the ground
    float o[3] = {(mn[0] + mx[0]) / 2, mn[1] + 0.05f, (mn[2] + mx[2]) / 2}, d[3] = {0, -1, 0}, t;
    Selection self;
    self.Set(s);
    float drop = mn[1];
    if (RayHit(o, d, t, nullptr, &self)) drop = t - 0.05f;
    else if (mn[1] > 0) drop = mn[1];
    else continue;
    if (s.kind == SelItem::Obj) Obj(s.index).pos[1] -= drop;
    else Mark(s.index).pos[1] -= drop;
  }
  status = "Dropped to the ground";
}

void App::ApplyTextureToObject(int object, int map_texture) {
  if (object < 0 || size_t(object) >= map.objects.size() || Obj(object).shape == Shape::Model) return;
  Obj(object).texture = map_texture;
}

void App::ApplyTextureToSelection(int map_texture) {
  current_texture = map_texture;
  bool any = false;
  for (auto& s : sel.items)
    if (s.kind == SelItem::Obj && Obj(s.index).shape != Shape::Model && Obj(s.index).texture != map_texture) any = true;
  if (!any) return;
  PushUndo();
  for (auto& s : sel.items)
    if (s.kind == SelItem::Obj) ApplyTextureToObject(s.index, map_texture);
}

// ---------------------------------------------------------------- files

void App::NewDocument() {
  map = NewMap();
  path.clear();
  dirty = false;
  undo.clear();
  redo.clear();
  sel.Clear();
  meshes.clear();
  current_texture = 0;
  cam = Camera{};
}

bool App::ConfirmDiscard() {
  if (!dirty) return true;
  int r2 = MessageBoxW(hwnd, L"Save the changes to this map first?", L"Saints Reborn Map Editor", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (r2 == IDCANCEL) return false;
  if (r2 == IDYES) return Save(false);
  return true;
}

static fs::path FileDialog(HWND owner, bool save, const wchar_t* filter, const wchar_t* ext, const fs::path& initial) {
  wchar_t buf[MAX_PATH * 4] = {};
  if (!initial.empty()) wcsncpy_s(buf, initial.wstring().c_str(), _TRUNCATE);
  OPENFILENAMEW of{};
  of.lStructSize = sizeof of;
  of.hwndOwner = owner;
  of.lpstrFilter = filter;
  of.lpstrFile = buf;
  of.nMaxFile = DWORD(std::size(buf));
  of.lpstrDefExt = ext;
  of.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of))) return {};
  return fs::path(buf);
}

fs::path OpenImageDialog(HWND owner) {
  return FileDialog(owner, false, L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0", L"png", {});
}

bool App::Save(bool save_as) {
  fs::path p = path;
  if (p.empty() || save_as) {
    fs::path suggestion = path.empty() ? fs::u8path(map.name + ".srmap") : path;
    p = FileDialog(hwnd, true, L"Saints Reborn map (*.srmap)\0*.srmap\0", L"srmap", suggestion);
    if (p.empty()) return false;
  }
  std::string text = SaveMap(map, p.parent_path().u8string());
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
  if (!f) {
    MessageBoxW(hwnd, (L"Could not save " + p.wstring()).c_str(), L"Saints Reborn Map Editor", MB_ICONERROR);
    return false;
  }
  path = p;
  dirty = false;
  status = "Saved " + p.filename().u8string();
  return true;
}

bool App::Open(const fs::path& p0) {
  fs::path p = p0;
  if (p.empty()) p = FileDialog(hwnd, false, L"Saints Reborn map (*.srmap)\0*.srmap\0", L"srmap", {});
  if (p.empty()) return false;
  try {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Map m = LoadMap(ss.str(), p.parent_path().u8string());
    map = std::move(m);
    path = p;
    dirty = false;
    undo.clear();
    redo.clear();
    sel.Clear();
    meshes.clear();
    RequestMapAssets();
    status = "Opened " + p.filename().u8string();
    return true;
  } catch (const std::exception& e) {
    MessageBoxA(hwnd, (std::string("Could not open the map: ") + e.what()).c_str(), "Saints Reborn Map Editor", MB_ICONERROR);
    return false;
  }
}

std::string App::Export() {
  std::ostringstream log;
  try {
    if (game_dir.empty() || !fs::exists(game_dir / "packfiles" / "mp_city_stream.vpp_xbox2"))
      throw mapconv::Error("the game folder is not set: choose your Saints Reborn folder in File > Settings");
    RequestMapAssets();
    bool uses_game = false;
    for (auto& o : map.objects) uses_game |= o.shape == Shape::Model;
    for (auto& t : map.textures) uses_game |= !t.game.empty();
    if (uses_game) {
      log << "Reading the game models and textures this map uses...\n";
      if (!WaitForGameAssets(120000)) log << "  (some could not be read: " << (link.error().empty() ? "timed out" : link.error()) << ")\n";
    }
    for (auto& t : map.textures)
      if (!t.game.empty() && t.pending) throw mapconv::Error("the game texture " + t.game + " could not be read");
    mapconv::MapSettings st;
    st.display_name = map.name.empty() ? "My Map" : map.name;
    st.texture_size = export_texture_size;
    std::string id = "mpx_";
    for (unsigned char ch : st.display_name) {
      if (std::isalnum(ch)) id += char(std::tolower(ch));
      else if ((ch == ' ' || ch == '_' || ch == '-') && id.back() != '_') id += '_';
      if (id.size() >= 24) break;
    }
    while (id.back() == '_') id.pop_back();
    st.map_id = id == "mpx" ? "mpx_map" : id;
    for (unsigned char ch : st.display_name)
      if (std::isalnum(ch)) st.folder += char(ch);
    if (st.folder.empty()) st.folder = "CustomMap";
    fs::path out = maps_dir.empty() ? game_dir.parent_path() / "maps" : maps_dir;
    auto rep = mapconv::BuildPack(ToMapInput(map), st, game_dir / "packfiles", out, [&](const std::string& s) { log << s << "\n"; });
    log << "\nDone: " << rep.triangles << " triangles, " << rep.textures << " textures, " << rep.spawns << " spawns, " << rep.weapons
        << " weapons, " << rep.vehicles << " vehicles, " << rep.dropoffs << " chain drop-offs\nModes:";
    for (auto& m : rep.modes) log << " " << m << ";";
    log << "\nMap pack written to " << rep.folder.u8string() << "\n";
    for (auto& w : rep.warnings) log << "  - " << w << "\n";
    log << "\nStart the game: \"" << st.display_name << "\" is in the System Link lobby's Level list.\n";
    if (uses_game)
      log << "\nThis pack holds models/textures from your copy of the game. To share the map, share the .srmap file:\n"
             "it only names the game files, and the editor rebuilds the pack from the other player's own game.\n";
    status = "Exported " + st.display_name;
  } catch (const std::exception& e) {
    log << "\nExport failed: " << e.what() << "\n";
    status = "Export failed";
  }
  return log.str();
}

// ---------------------------------------------------------------- settings

void App::LoadSettings() {
  std::ifstream f(exe_dir / "SaintsRebornMapEditor.ini");
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "game") game_dir = fs::u8path(v);
    else if (k == "game_confirmed") game_dir_confirmed = v == "1";
    else if (k == "maps") maps_dir = fs::u8path(v);
    else if (k == "viewer") viewer_exe = fs::u8path(v);
    else if (k == "snap") snap = v == "1";
    else if (k == "texture_size") export_texture_size = std::clamp(std::atoi(v.c_str()), 128, 1024);
  }
  if (!game_dir.empty()) game_dir = ResolveGameDir(game_dir);
  if (game_dir.empty()) {
    game_dir_confirmed = false;
    game_dir = FindGameDir(exe_dir);  // offered in the folder dialog; the user confirms it
  }
  if (viewer_exe.empty() || !fs::exists(viewer_exe)) viewer_exe = exe_dir / "SaintsRowAssetViewer.exe";
}

void App::SaveSettings() {
  std::ofstream f(exe_dir / "SaintsRebornMapEditor.ini", std::ios::trunc);
  f << "game=" << game_dir.u8string() << "\ngame_confirmed=" << (game_dir_confirmed ? 1 : 0) << "\nmaps=" << maps_dir.u8string()
    << "\nviewer=" << (viewer_exe == exe_dir / "SaintsRowAssetViewer.exe" ? std::string() : viewer_exe.u8string())
    << "\nsnap=" << (snap ? 1 : 0) << "\ntexture_size=" << export_texture_size << "\n";
}

void App::UpdateTitle() {
  std::string t = (dirty ? "* " : "") + map.name + (path.empty() ? "" : " (" + path.filename().u8string() + ")") + " - Saints Reborn Map Editor";
  int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
  std::wstring w(size_t(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), n);
  SetWindowTextW(hwnd, w.c_str());
}

// ---------------------------------------------------------------- game assets

bool App::SetGameDir(const fs::path& picked) {
  fs::path g = ResolveGameDir(picked);
  if (g.empty()) return false;
  game_dir = g;
  game_dir_confirmed = true;
  SaveSettings();
  StartAssets();
  return true;
}

void App::StartAssets() {
  for (auto& [name, md] : models) {
    for (int t : md.gpu_textures) r.ReleaseTexture(t);
    r.ReleaseTexture(md.thumb);
  }
  models.clear();
  for (auto& [name, id] : texture_thumbs) r.ReleaseTexture(id);
  texture_thumbs.clear();
  texture_thumb_lru.clear();
  for (auto& c : meshes) c.valid = false;
  if (game_dir.empty()) return;
  link.Start(viewer_exe, game_dir);
  RequestMapAssets();
}

void App::RequestMapAssets() {
  for (auto& o : map.objects)
    if (o.shape == Shape::Model) Model(o.asset);
  for (auto& t : map.textures)
    if (!t.game.empty() && t.pending) link.RequestTexture(t.game, kFullSize, true);
}

ModelData* App::Model(const std::string& asset, bool request) {
  std::string key = Lower(asset);
  auto it = models.find(key);
  if (it != models.end()) return &it->second;
  if (!request) return nullptr;
  ModelData& md = models[key];
  link.RequestModel(asset);
  return &md;
}

int App::TextureThumb(const std::string& name) {
  auto it = texture_thumbs.find(name);
  if (it != texture_thumbs.end()) {
    // most recently used last
    auto l = std::find(texture_thumb_lru.begin(), texture_thumb_lru.end(), name);
    if (l != texture_thumb_lru.end()) texture_thumb_lru.splice(texture_thumb_lru.end(), texture_thumb_lru, l);
    return it->second;
  }
  if (link.state() == AssetLink::State::Ready && !link.Requested(name, kThumbSize)) link.RequestTexture(name, kThumbSize, false);
  return -1;
}

void App::PumpAssets() {
  for (auto& res : link.Poll()) {
    if (res.model) {
      ModelData& md = models[Lower(res.name)];
      md.loaded = md.failed = false;
      if (res.path.empty()) {
        md.failed = true;
        md.error = res.error;
        continue;
      }
      try {
        md.in = mapconv::ImportGltf(res.path);
        for (int a = 0; a < 3; ++a) { md.mn[a] = 1e30f; md.mx[a] = -1e30f; }
        for (auto& t : md.in.tris)
          for (auto& p : t.p)
            for (int a = 0; a < 3; ++a) { md.mn[a] = std::min(md.mn[a], p[a]); md.mx[a] = std::max(md.mx[a], p[a]); }
        md.in.markers.clear();  // a prop named like a marker is still just a prop
        for (auto& m : md.in.materials) {
          if (!m.image.w) m.image = MakeBuiltinTexture("white");
          md.gpu_textures.push_back(r.CreateTexture(m.image));
        }
        md.loaded = !md.in.tris.empty();
        if (!md.loaded) { md.failed = true; md.error = "the model has no triangles"; }
      } catch (const std::exception& e) {
        md.failed = true;
        md.error = e.what();
      }
      continue;
    }
    if (res.size == kThumbSize) {
      mapconv::Image img = res.path.empty() ? mapconv::Image{} : LoadImageFile(res.path.u8string());
      if (!img.w) img = MakeBuiltinTexture("grey");
      texture_thumbs[res.name] = r.CreateTexture(img);
      texture_thumb_lru.push_back(res.name);
      while (texture_thumb_lru.size() > kMaxTextureThumbs) {
        // forget the oldest preview; it is read again (from the disk cache) when it scrolls back into view
        std::string old = texture_thumb_lru.front();
        texture_thumb_lru.pop_front();
        r.ReleaseTexture(texture_thumbs[old]);
        texture_thumbs.erase(old);
      }
      continue;
    }
    for (auto& t : map.textures) {
      if (t.game != res.name || !t.pending) continue;
      mapconv::Image img = res.path.empty() ? mapconv::Image{} : LoadImageFile(res.path.u8string());
      if (img.w) {
        t.image = std::move(img);
        t.pending = false;
      } else {
        status = "Could not read the game texture " + res.name + (res.error.empty() ? "" : ": " + res.error);
      }
    }
  }
}

bool App::WaitForGameAssets(int timeout_ms) {
  RequestMapAssets();
  DWORD start = GetTickCount();
  for (;;) {
    PumpAssets();
    bool done = true;
    for (auto& o : map.objects)
      if (o.shape == Shape::Model) {
        ModelData* md = Model(o.asset);
        if (!md->loaded && !md->failed) done = false;
      }
    for (auto& t : map.textures)
      if (!t.game.empty() && t.pending) done = false;
    if (done) return true;
    if (link.state() == AssetLink::State::Failed || link.state() == AssetLink::State::Off) return false;
    if (GetTickCount() - start > DWORD(timeout_ms)) return false;
    Sleep(15);
  }
}

void App::ProcessThumbnails() {
  int budget = 2;
  for (auto& [name, md] : models) {
    if (budget <= 0) break;
    if (!md.loaded || md.thumb >= 0 || !md.thumb_wanted) continue;
    --budget;
    std::vector<std::pair<std::vector<MeshVertex>, int>> parts(md.in.materials.size());
    for (size_t m = 0; m < parts.size(); ++m) parts[m].second = md.gpu_textures[m];
    for (auto& t : md.in.tris)
      if (t.material >= 0 && size_t(t.material) < parts.size())
        for (int k = 0; k < 3; ++k)
          parts[size_t(t.material)].first.push_back({{t.p[k][0], t.p[k][1], t.p[k][2]}, {t.n[k][0], t.n[k][1], t.n[k][2]}, {t.uv[k][0], t.uv[k][1]}});
    const float white[4] = {1, 1, 1, 1};
    md.thumb = RenderPreview(parts, md.mn, md.mx, white);
  }
}

int App::RenderPreview(const std::vector<std::pair<std::vector<MeshVertex>, int>>& parts, const float mn[3], const float mx[3],
                       const float tint[4]) {
  const int S = 128;
  ComPtr<ID3D11RenderTargetView> target;
  int id = r.CreateTargetTexture(S, S, target);
  if (id < 0) return -1;
  if (!thumb_dsv) {
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = dd.Height = S;
    dd.MipLevels = dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    if (SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &depth))) dev->CreateDepthStencilView(depth.Get(), nullptr, &thumb_dsv);
  }
  const float bg[4] = {0.13f, 0.12f, 0.16f, 1};
  ctx->ClearRenderTargetView(target.Get(), bg);
  ctx->ClearDepthStencilView(thumb_dsv.Get(), D3D11_CLEAR_DEPTH, 1, 0);
  D3D11_VIEWPORT vp{0, 0, float(S), float(S), 0, 1};
  ctx->RSSetViewports(1, &vp);
  ctx->OMSetRenderTargets(1, target.GetAddressOf(), thumb_dsv.Get());
  // fit the bounding sphere into the 60 degree view
  float c[3], radius = 0;
  for (int a = 0; a < 3; ++a) {
    c[a] = (mn[a] + mx[a]) / 2;
    radius += (mx[a] - mn[a]) * (mx[a] - mn[a]) / 4;
  }
  radius = std::max(0.02f, std::sqrt(radius));
  Camera tc;
  tc.yaw = -0.8f;  // three-quarter view from the lit side
  tc.pitch = -0.4f;
  float dist = radius / std::sin(30 * kDeg) * 1.02f;
  for (int a = 0; a < 3; ++a) tc.pos[a] = c[a] - tc.Forward(a) * dist;
  XMMATRIX view_proj = tc.View() * XMMatrixPerspectiveFovLH(60 * kDeg, 1.0f, std::max(0.01f, dist - radius * 2), dist + radius * 2);
  r.Begin(view_proj, tc.pos);
  for (auto& [verts, tex] : parts)
    if (!verts.empty()) r.DrawMesh(r.CreateMesh(verts), tex, tint, tex >= 0);
  r.End();
  return id;
}

// ---------------------------------------------------------------- viewport

void App::RenderScene() {
  SyncGpu();
  XMMATRIX vp = cam.View() * Projection(float(width) / float(std::max(height, 1)));
  r.Begin(vp, cam.pos);
  for (size_t i = 0; i < map.objects.size(); ++i) {
    const CachedMesh& c = meshes[i];
    bool selected = sel.Has(SelItem::Obj, int(i));
    bool active = selected && sel.Active() && *sel.Active() == SelItem{SelItem::Obj, int(i)};
    float tint[4] = {1, 1, 1, 1};
    if (selected) { tint[0] = 1.12f; tint[1] = 1.05f; tint[2] = 1.25f; }
    for (auto& p : c.parts) {
      int tex = p.gpu_texture;
      if (p.map_texture >= 0 && size_t(p.map_texture) < gpu_textures.size()) tex = gpu_textures[size_t(p.map_texture)];
      r.DrawMesh(p.mesh, tex, tint);
    }
    if (c.tris.empty()) {  // a model that is still loading (or missing)
      const ModelData* md = models.count(Lower(map.objects[i].asset)) ? &models.at(Lower(map.objects[i].asset)) : nullptr;
      r.Box(c.mn, c.mx, md && md->failed ? 0xFF3030FFu : 0xFFC0C0C0u, true);
    }
    if (selected) r.Box(c.mn, c.mx, active ? 0xFF40D0FFu : 0xFFFF60E0u, true);
  }
  for (size_t i = 0; i < map.markers.size(); ++i) {
    const MarkerObj& m = map.markers[i];
    float mn[3], mx[3];
    MarkerBox(m, mn, mx);
    uint32_t col = MarkerColour(m);
    bool selected = sel.Has(SelItem::Mark, int(i));
    float tint[4] = {float(col & 255) / 255, float((col >> 8) & 255) / 255, float((col >> 16) & 255) / 255, selected ? 0.9f : 0.6f};
    Object box;
    box.pos = {(mn[0] + mx[0]) / 2, mn[1], (mn[2] + mx[2]) / 2};
    box.size = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
    box.rot = {0, m.yaw, 0};
    std::vector<MeshVertex> v;
    for (auto& t : ObjectTriangles(box))
      for (int k = 0; k < 3; ++k) v.push_back({{t.p[k][0], t.p[k][1], t.p[k][2]}, {t.n[k][0], t.n[k][1], t.n[k][2]}, {0, 0}});
    auto mesh = r.CreateMesh(v);
    r.DrawMesh(mesh, -1, tint, false);
    r.Box(mn, mx, selected ? 0xFFFFFFFFu : col, selected);
    // facing arrow on the floor
    float y = m.pos[1] + 0.05f, fx = std::sin(m.yaw * kDeg), fz = std::cos(m.yaw * kDeg);
    float a[3] = {m.pos[0], y, m.pos[2]}, b[3] = {m.pos[0] + fx * 1.6f, y, m.pos[2] + fz * 1.6f};
    float l[3] = {b[0] - fx * 0.5f - fz * 0.35f, y, b[2] - fz * 0.5f + fx * 0.35f};
    float rr[3] = {b[0] - fx * 0.5f + fz * 0.35f, y, b[2] - fz * 0.5f - fx * 0.35f};
    r.Line(a, b, col, true);
    r.Line(b, l, col, true);
    r.Line(b, rr, col, true);
  }
  if (show_grid) {
    const int R = 100;
    for (int i = -R; i <= R; ++i) {
      uint32_t col = i % 5 == 0 ? 0x80B0B0B0u : 0x40909090u;
      float a[3] = {float(i), 0.01f, float(-R)}, b[3] = {float(i), 0.01f, float(R)};
      r.Line(a, b, i == 0 ? 0xFFFF7070u : col);  // x = 0 line (blue: Z axis)
      float c2[3] = {float(-R), 0.01f, float(i)}, d2[3] = {float(R), 0.01f, float(i)};
      r.Line(c2, d2, i == 0 ? 0xFF6060FFu : col);  // z = 0 line (red: X axis)
    }
  }
  r.End();
}

}  // namespace editor

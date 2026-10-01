// Editor UI: menus, toolbar, hierarchy, inspector, asset browser, gizmo and viewport input
// (Dear ImGui + ImGuizmo). Camera and tools follow Unity's scene view.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.h"
#include <shellapi.h>
#include "imgui.h"
#include "ImGuizmo.h"
#include "textures.h"

namespace editor {
namespace fs = std::filesystem;
using namespace DirectX;
using mapconv::MarkerKind;

std::filesystem::path OpenImageDialog(HWND owner);

namespace {
constexpr float kDeg = 3.14159265358979f / 180.0f;
const char* kWeapons[] = {"ak47", "desert eagle", "m16", "mac10", "molotov", "pipe_bomb", "pump_action_shotgun",
                          "rpg_launcher", "sniper_rifle", "spas12", "tec9"};
const char* kWeaponLabels[] = {"AK47", "Desert Eagle", "M16", "Mac-10", "Molotov", "Pipe bomb", "Pump shotgun",
                               "RPG", "Sniper rifle", "SPAS-12", "Tec-9"};
const char* kVehicles[] = {"car_2dr_sports03", "car_2dr_muscle01", "car_4dr_standard08", "car_2dr_compact01", "sp_metermaid01"};
const ImVec4 kRed(0.78f, 0.22f, 0.22f, 1), kRedHi(0.9f, 0.3f, 0.3f, 1);

ImTextureID TexId(App& a, int renderer_texture) {
  return ImTextureID(reinterpret_cast<intptr_t>(a.r.TextureView(renderer_texture)));
}
ImTextureID MapTex(App& a, int map_texture) {
  if (map_texture < 0 || size_t(map_texture) >= a.gpu_textures.size()) return ImTextureID(0);
  return TexId(a, a.gpu_textures[size_t(map_texture)]);
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// our rotation (yaw * pitch * roll) <-> 4x4 (row-vector layout, as ImGuizmo expects)
void ObjectMatrix(const V3& pos, const V3& rot, float m[16]) {
  V3 ax = Rotate(rot, {1, 0, 0}), ay = Rotate(rot, {0, 1, 0}), az = Rotate(rot, {0, 0, 1});
  const float M[16] = {ax[0], ax[1], ax[2], 0, ay[0], ay[1], ay[2], 0, az[0], az[1], az[2], 0, pos[0], pos[1], pos[2], 1};
  std::copy(M, M + 16, m);
}
void MatrixToObject(const float m[16], V3& pos, V3& rot) {
  float s[3];
  for (int i = 0; i < 3; ++i) s[i] = std::sqrt(m[4 * i] * m[4 * i] + m[4 * i + 1] * m[4 * i + 1] + m[4 * i + 2] * m[4 * i + 2]);
  float c0[3] = {m[0] / s[0], m[1] / s[0], m[2] / s[0]}, c1[3] = {m[4] / s[1], m[5] / s[1], m[6] / s[1]};
  float c2[3] = {m[8] / s[2], m[9] / s[2], m[10] / s[2]};
  float pitch = std::asin(std::clamp(-c2[1], -1.0f, 1.0f));
  float yaw = std::atan2(c2[0], c2[2]);
  float roll = std::atan2(c0[1], c1[1]);
  rot = {pitch / kDeg, yaw / kDeg, roll / kDeg};
  for (auto& x : rot) x = std::round(x * 1000) / 1000;
  pos = {m[12], m[13], m[14]};
}

bool Key(ImGuiKey k) { return ImGui::IsKeyPressed(k, false); }
bool Chord(ImGuiKeyChord c) { return ImGui::IsKeyChordPressed(c); }

std::string ItemLabel(App& a, const SelItem& s) {
  if (s.kind == SelItem::Obj) {
    auto& o = a.Obj(s.index);
    return o.name.empty() ? (o.shape == Shape::Model ? ModelLabel(o.asset) : ShapeName(o.shape)) : o.name;
  }
  auto& m = a.Mark(s.index);
  return std::string(MarkerLabel(m)) + (m.type.empty() ? "" : " (" + m.type + ")");
}

// Screen-space axis widget (top right of the view), like Unity's scene gizmo: click an axis to look along it.
struct AxisWidget {
  ImVec2 centre;
  float radius = 42;
  int hovered = -1;  // 0..5 = +X, -X, +Y, -Y, +Z, -Z
  bool over = false;
};
AxisWidget g_axis;

void UpdateAxisWidget(App& a) {
  float top = ImGui::GetFrameHeight() + 44;
  g_axis.centre = ImVec2(float(a.width) - 70, top + 60);
  ImVec2 m = ImGui::GetIO().MousePos;
  float dx = m.x - g_axis.centre.x, dy = m.y - g_axis.centre.y;
  g_axis.over = dx * dx + dy * dy < (g_axis.radius + 14) * (g_axis.radius + 14);
  g_axis.hovered = -1;
  XMMATRIX v = a.cam.View();
  float best = 14 * 14;
  for (int i = 0; i < 6; ++i) {
    XMVECTOR dir = XMVectorSet(i / 2 == 0 ? (i % 2 ? -1.f : 1.f) : 0, i / 2 == 1 ? (i % 2 ? -1.f : 1.f) : 0, i / 2 == 2 ? (i % 2 ? -1.f : 1.f) : 0, 0);
    XMVECTOR s = XMVector3TransformNormal(dir, v);
    float px = g_axis.centre.x + XMVectorGetX(s) * g_axis.radius, py = g_axis.centre.y - XMVectorGetY(s) * g_axis.radius;
    float d2 = (m.x - px) * (m.x - px) + (m.y - py) * (m.y - py);
    if (d2 < best) { best = d2; g_axis.hovered = i; }
  }
}

void DrawAxisWidget(App& a) {
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->AddCircleFilled(g_axis.centre, g_axis.radius + 16, g_axis.over ? 0x60302030u : 0x40201818u);
  XMMATRIX v = a.cam.View();
  struct Tip { float z, x, y; int i; };
  Tip tips[6];
  for (int i = 0; i < 6; ++i) {
    XMVECTOR dir = XMVectorSet(i / 2 == 0 ? (i % 2 ? -1.f : 1.f) : 0, i / 2 == 1 ? (i % 2 ? -1.f : 1.f) : 0, i / 2 == 2 ? (i % 2 ? -1.f : 1.f) : 0, 0);
    XMVECTOR s = XMVector3TransformNormal(dir, v);
    tips[i] = {XMVectorGetZ(s), g_axis.centre.x + XMVectorGetX(s) * g_axis.radius, g_axis.centre.y - XMVectorGetY(s) * g_axis.radius, i};
  }
  std::sort(tips, tips + 6, [](const Tip& p, const Tip& q) { return p.z > q.z; });  // far ones first
  const ImU32 cols[3] = {0xFF4848E8u, 0xFF48D068u, 0xFFE89048u};
  const char* names[3] = {"X", "Y", "Z"};
  for (auto& t : tips) {
    ImU32 c = cols[t.i / 2];
    bool neg = t.i % 2;
    if (!neg) dl->AddLine(g_axis.centre, ImVec2(t.x, t.y), c, 3);
    float rad = neg ? 6.0f : 10.0f;
    if (t.i == g_axis.hovered) dl->AddCircleFilled(ImVec2(t.x, t.y), rad + 3, 0xFFFFFFFFu);
    dl->AddCircleFilled(ImVec2(t.x, t.y), rad, neg ? (c & 0x00FFFFFFu) | 0x90000000u : c);
    if (!neg) {
      ImVec2 ts = ImGui::CalcTextSize(names[t.i / 2]);
      dl->AddText(ImVec2(t.x - ts.x / 2, t.y - ts.y / 2), 0xFF000000u, names[t.i / 2]);
    }
  }
}

// Click on an axis end: look along that axis at what is in front of the camera.
void SnapViewToAxis(App& a, int i) {
  float pivot[3];
  for (int k = 0; k < 3; ++k) pivot[k] = a.cam.pos[k] + a.cam.Forward(k) * a.orbit_dist;
  // looking from the +X end means looking towards -X
  float d[3] = {0, 0, 0};
  d[i / 2] = i % 2 ? 1.f : -1.f;
  if (i / 2 == 1) {
    a.cam.pitch = d[1] < 0 ? -1.55f : 1.55f;
  } else {
    a.cam.pitch = 0;
    a.cam.yaw = std::atan2(d[0], d[2]);
  }
  for (int k = 0; k < 3; ++k) a.cam.pos[k] = pivot[k] - a.cam.Forward(k) * a.orbit_dist;
}

}  // namespace

// ============================================================================ input

void App::HandleInput() {
  ImGuiIO& io = ImGui::GetIO();
  static bool flying = false, dolly = false, orbiting = false, panning = false, rmb_moved = false, lmb_down = false;
  static float lmb_from[2];
  bool over_ui = io.WantCaptureMouse;
  UpdateAxisWidget(*this);
  bool gizmo_busy = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
  float dt = io.DeltaTime;

  // --- right mouse: look + WASD/QE (Unity flythrough); Alt + right: zoom
  if (!over_ui && !drag.active && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (io.KeyAlt) dolly = true;
    else flying = true;
    rmb_moved = false;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    if ((flying || dolly) && !rmb_moved && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      // a click, not a look: context menu for what is under the mouse
      SelItem hit;
      float o[3], d[3], t;
      if (MouseRay(io.MousePos.x, io.MousePos.y, o, d) && RayHit(o, d, t, &hit) && !sel.Has(hit.kind, hit.index)) sel.Set(hit);
      want_context_menu = true;
    }
    flying = dolly = false;
  }
  if ((flying || dolly) && (std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y)) > 0.5f) rmb_moved = true;
  if (flying) {
    cam.yaw += io.MouseDelta.x * 0.004f;
    cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.004f, -1.55f, 1.55f);
    if (io.MouseWheel != 0) fly_speed = std::clamp(fly_speed * (io.MouseWheel > 0 ? 1.25f : 0.8f), 1.0f, 200.0f);
    float speed = fly_speed * (io.KeyShift ? 3.0f : 1.0f) * dt;
    float f[3] = {cam.Forward(0), cam.Forward(1), cam.Forward(2)}, rt[3] = {std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
    auto mv = [&](const float* d, float k) { for (int a = 0; a < 3; ++a) cam.pos[a] += d[a] * k; };
    if (ImGui::IsKeyDown(ImGuiKey_W)) mv(f, speed);
    if (ImGui::IsKeyDown(ImGuiKey_S)) mv(f, -speed);
    if (ImGui::IsKeyDown(ImGuiKey_D)) mv(rt, speed);
    if (ImGui::IsKeyDown(ImGuiKey_A)) mv(rt, -speed);
    if (ImGui::IsKeyDown(ImGuiKey_E)) cam.pos[1] += speed;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) cam.pos[1] -= speed;
    for (ImGuiKey k : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E})
      if (ImGui::IsKeyDown(k)) rmb_moved = true;  // flying with the keys is not a click either
  }
  if (dolly) {
    float k = (io.MouseDelta.x - io.MouseDelta.y) * 0.02f * std::max(1.0f, orbit_dist * 0.1f);
    for (int a = 0; a < 3; ++a) cam.pos[a] += cam.Forward(a) * k;
    orbit_dist = std::max(0.5f, orbit_dist - k);
  }
  // --- wheel: zoom towards what is in front (Unity scrolls towards the pivot)
  if (!over_ui && !flying && io.MouseWheel != 0) {
    float k = io.MouseWheel * std::max(0.5f, orbit_dist * 0.15f) * (io.KeyShift ? 3.0f : 1.0f);
    k = std::min(k, orbit_dist - 0.5f);
    for (int a = 0; a < 3; ++a) cam.pos[a] += cam.Forward(a) * k;
    orbit_dist = std::max(0.5f, orbit_dist - k);
  }
  // --- middle mouse (or left with the hand tool): pan
  bool hand = tool == ToolView && !io.KeyAlt;
  if (!over_ui && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (hand && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_axis.over)))
    panning = true;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !(hand && ImGui::IsMouseDown(ImGuiMouseButton_Left))) panning = false;
  if (panning) {
    float rt[3] = {std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
    float up[3] = {-std::sin(cam.pitch) * std::sin(cam.yaw), std::cos(cam.pitch), -std::sin(cam.pitch) * std::cos(cam.yaw)};
    float k = 0.0012f * std::max(2.0f, orbit_dist);
    for (int a = 0; a < 3; ++a) cam.pos[a] += -rt[a] * io.MouseDelta.x * k + up[a] * io.MouseDelta.y * k;
  }
  // --- Alt + left: orbit around the point in front of the camera (F puts the selection there)
  if (!over_ui && io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) orbiting = true;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) orbiting = false;
  if (orbiting) {
    float pivot[3];
    for (int a = 0; a < 3; ++a) pivot[a] = cam.pos[a] + cam.Forward(a) * orbit_dist;
    cam.yaw += io.MouseDelta.x * 0.005f;
    cam.pitch = std::clamp(cam.pitch - io.MouseDelta.y * 0.005f, -1.55f, 1.55f);
    for (int a = 0; a < 3; ++a) cam.pos[a] = pivot[a] - cam.Forward(a) * orbit_dist;
  }
  // --- left: axis widget, click select (Ctrl toggles, Shift adds), drag = rectangle select
  if (!over_ui && !io.KeyAlt && !hand && !drag.active && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (g_axis.over) {
      if (g_axis.hovered >= 0) SnapViewToAxis(*this, g_axis.hovered);
    } else if (!gizmo_busy) {
      lmb_down = true;
      lmb_from[0] = io.MousePos.x;
      lmb_from[1] = io.MousePos.y;
    }
  }
  if (lmb_down) {
    float dx = io.MousePos.x - lmb_from[0], dy = io.MousePos.y - lmb_from[1];
    if (!marquee && dx * dx + dy * dy > 25) {
      marquee = true;
      std::copy(lmb_from, lmb_from + 2, marquee_from);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      float to[2] = {io.MousePos.x, io.MousePos.y};
      if (marquee) MarqueeSelect(marquee_from, to, io.KeyShift || io.KeyCtrl);
      else Pick(io.MousePos.x, io.MousePos.y, io.KeyCtrl ? 1 : io.KeyShift ? 2 : 0);
      lmb_down = marquee = false;
    }
  }
  // --- keys (all of them work unless a text field is being typed in)
  if (io.WantTextInput) return;
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_Z)) Undo();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_Y) || Chord(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) Redo();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_S)) Save(false);
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_O) && ConfirmDiscard()) Open({});
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_N) && ConfirmDiscard()) NewDocument();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_E)) { export_log = Export(); show_export_log = true; }
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_D)) DuplicateSelection();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_C)) CopySelection();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_V)) Paste();
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_X)) { CopySelection(); DeleteSelection(); }
  if (Chord(ImGuiMod_Ctrl | ImGuiKey_A)) SelectAll();
  if (flying || io.KeyCtrl || io.KeyAlt) return;
  if (Key(ImGuiKey_Delete) || Key(ImGuiKey_Backspace)) DeleteSelection();
  if (Key(ImGuiKey_Q)) tool = ToolView;
  if (Key(ImGuiKey_W)) tool = ToolMove;
  if (Key(ImGuiKey_E)) tool = ToolRotate;
  if (Key(ImGuiKey_R)) tool = ToolScale;
  if (Key(ImGuiKey_F)) FocusSelection();
  if (Key(ImGuiKey_G)) snap = !snap;
  if (Key(ImGuiKey_End)) DropToGround();
  if (Key(ImGuiKey_Escape)) sel.Clear();
}

// ============================================================================ gizmo

static void Gizmo(App& a) {
  ImGuiIO& io = ImGui::GetIO();
  if (a.sel.Empty() || a.tool == ToolView || a.drag.active) {
    a.gizmo_was_using = false;
    return;
  }
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
  ImGuizmo::SetRect(0, 0, float(a.width), float(a.height));
  XMFLOAT4X4 view, proj;
  XMStoreFloat4x4(&view, a.cam.View());
  XMStoreFloat4x4(&proj, XMMatrixPerspectiveFovLH(60 * kDeg, float(a.width) / float(std::max(a.height, 1)), 0.1f, 3000.0f));
  bool only_markers = true;
  for (auto& s : a.sel.items) only_markers &= s.kind == SelItem::Mark;
  int op = a.tool == ToolMove ? 0 : a.tool == ToolRotate ? 1 : 2;
  if (only_markers && op == 2) op = 0;
  ImGuizmo::OPERATION ops[3] = {ImGuizmo::TRANSLATE, only_markers ? ImGuizmo::ROTATE_Y : ImGuizmo::ROTATE, ImGuizmo::SCALE};

  auto item_matrix = [&](const SelItem& s, float m[16]) {
    if (s.kind == SelItem::Obj) ObjectMatrix(a.Obj(s.index).pos, a.Obj(s.index).rot, m);
    else ObjectMatrix(a.Mark(s.index).pos, V3{0, a.Mark(s.index).yaw, 0}, m);
  };
  // pivot: the active item, or the centre of everything selected (Unity's Pivot / Center)
  const SelItem& act = *a.sel.Active();
  float pivot[16];
  item_matrix(act, pivot);
  if (a.pivot_center && a.sel.Count() > 1) {
    float mn[3], mx[3];
    if (a.SelectionBounds(mn, mx))
      for (int k = 0; k < 3; ++k) pivot[12 + k] = (mn[k] + mx[k]) / 2;
  }
  bool local = a.gizmo_local || op == 2;
  if (!local) {  // world axes
    for (int i = 0; i < 12; ++i) pivot[i] = (i % 5 == 0) ? 1.0f : 0.0f;
  }
  float m[16], delta[16];
  std::copy(pivot, pivot + 16, m);
  bool snapping = a.snap != io.KeyCtrl;  // Ctrl flips snapping while dragging (Unity: hold Ctrl to snap)
  float snapv[3] = {a.snap_move, a.snap_move, a.snap_move};
  if (op == 1) snapv[0] = snapv[1] = snapv[2] = a.snap_angle;
  if (op == 2) snapv[0] = snapv[1] = snapv[2] = a.snap_scale;
  bool changed = ImGuizmo::Manipulate(&view.m[0][0], &proj.m[0][0], ops[op], local ? ImGuizmo::LOCAL : ImGuizmo::WORLD, m, delta,
                                      snapping ? snapv : nullptr);
  bool using_now = ImGuizmo::IsUsing();
  if (using_now && !a.gizmo_was_using) a.PushUndo();
  a.gizmo_was_using = using_now;
  if (!changed) return;
  a.MarkDirty();
  if (op == 2) {
    float s[3];
    for (int i = 0; i < 3; ++i) s[i] = std::sqrt(delta[4 * i] * delta[4 * i] + delta[4 * i + 1] * delta[4 * i + 1] + delta[4 * i + 2] * delta[4 * i + 2]);
    for (auto& it : a.sel.items) {
      if (it.kind == SelItem::Obj) {
        Object& o = a.Obj(it.index);
        for (int i = 0; i < 3; ++i) {
          if (o.shape == Shape::Model) o.scale[size_t(i)] = std::max(0.01f, o.scale[size_t(i)] * s[i]);
          else o.size[size_t(i)] = std::max(0.05f, o.size[size_t(i)] * s[i]);
        }
      }
      if (a.sel.Count() > 1) {  // spread the items with the scale
        V3& p = it.kind == SelItem::Obj ? a.Obj(it.index).pos : a.Mark(it.index).pos;
        for (int k = 0; k < 3; ++k) p[size_t(k)] = pivot[12 + k] + (p[size_t(k)] - pivot[12 + k]) * s[k];
      }
    }
    return;
  }
  // every selected item moves with the pivot: M' = M * inverse(pivot before) * pivot after
  XMFLOAT4X4 before, after;
  std::memcpy(&before, pivot, sizeof before);
  std::memcpy(&after, m, sizeof after);
  XMMATRIX w = XMMatrixInverse(nullptr, XMLoadFloat4x4(&before)) * XMLoadFloat4x4(&after);
  for (auto& it : a.sel.items) {
    float im[16];
    item_matrix(it, im);
    XMFLOAT4X4 cur;
    std::memcpy(&cur, im, sizeof cur);
    XMFLOAT4X4 res;
    XMStoreFloat4x4(&res, XMLoadFloat4x4(&cur) * w);
    V3 p, r;
    MatrixToObject(&res.m[0][0], p, r);
    if (it.kind == SelItem::Obj) {
      a.Obj(it.index).pos = p;
      if (op == 1) a.Obj(it.index).rot = r;
    } else {
      a.Mark(it.index).pos = p;
      if (op == 1) a.Mark(it.index).yaw = std::round(r[1] * 100) / 100;
    }
  }
}

// ============================================================================ widgets

// "Position  X [ 1.00 ] Y [ 2.00 ] Z [ 3.00 ]": click a number to type it, drag it (or its X/Y/Z label) to scrub.
static bool Vec3Row(App& a, const char* label, float* v, float speed, float lo, float hi, const char* fmt) {
  ImGui::PushID(label);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(ImGui::GetFontSize() * 5.2f);
  float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  float lw = ImGui::GetFrameHeight() * 0.8f;
  float w = (ImGui::GetContentRegionAvail().x - 2 * spacing) / 3 - lw;
  const char* axes[3] = {"X", "Y", "Z"};
  const ImVec4 cols[3] = {ImVec4(0.72f, 0.2f, 0.2f, 1), ImVec4(0.24f, 0.6f, 0.24f, 1), ImVec4(0.2f, 0.36f, 0.75f, 1)};
  bool changed = false;
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(i);
    if (i) ImGui::SameLine(0, spacing);
    ImGui::PushStyleColor(ImGuiCol_Button, cols[i]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, cols[i]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, cols[i]);
    ImGui::Button(axes[i], ImVec2(lw, 0));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActivated()) a.PushUndo();
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0) {
      v[i] = std::clamp(v[i] + ImGui::GetIO().MouseDelta.x * speed, lo, hi);
      changed = true;
    }
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##v", &v[i], speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::IsItemActivated()) a.PushUndo();
    ImGui::PopID();
  }
  ImGui::PopID();
  if (changed) a.MarkDirty();
  return changed;
}

static bool RedButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button, kRed);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kRedHi);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kRedHi);
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return r;
}

static void MapTexturePicker(App& a, int& target, float size) {
  float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  for (size_t i = 0; i < a.map.textures.size(); ++i) {
    ImGui::PushID(int(i));
    bool on = int(i) == target;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.35f, 1.0f, 1.0f));
    if (ImGui::ImageButton("t", MapTex(a, int(i)), ImVec2(size, size))) target = int(i);
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !a.drag.active)
      a.drag = {true, false, a.map.textures[i].name, int(i)};
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s%s", a.map.textures[i].name.c_str(), a.map.textures[i].game.empty() ? "" : "  (game texture)");
    ImGui::PopID();
    float next = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + size;
    if (i + 1 < a.map.textures.size() && next < right) ImGui::SameLine();
  }
}

// ============================================================================ asset browser

namespace {

// Things that can be added from the Shapes tab (clicked or dragged into the view).
struct AddItem {
  const char* id;
  const char* label;
};
const AddItem kAddItems[] = {
    {"shape:Box", "Box"},           {"shape:Ramp", "Ramp"},          {"shape:Stairs", "Stairs"},
    {"shape:Cylinder", "Cylinder"}, {"shape:Wedge", "Wedge"},        {"marker:spawn", "Spawn point"},
    {"marker:red", "Red spawn"},    {"marker:blue", "Blue spawn"},   {"marker:weapon", "Weapon"},
    {"marker:vehicle", "Vehicle (drivable)"}, {"marker:dropoff", "Chains drop-off"}, {"marker:start", "Player start"},
};

void AddById(App& a, const std::string& id, const float* at) {
  for (Shape s : {Shape::Box, Shape::Ramp, Shape::Stairs, Shape::Cylinder, Shape::Wedge})
    if (id == std::string("shape:") + ShapeName(s)) return a.AddObject(s, at);
  if (id == "marker:spawn") a.AddMarker(MarkerKind::Spawn, 0, "", at);
  if (id == "marker:red") a.AddMarker(MarkerKind::Spawn, 1, "", at);
  if (id == "marker:blue") a.AddMarker(MarkerKind::Spawn, 2, "", at);
  if (id == "marker:weapon") a.AddMarker(MarkerKind::Weapon, 0, "ak47", at);
  if (id == "marker:vehicle") a.AddMarker(MarkerKind::Vehicle, 0, kVehicles[0], at);
  if (id == "marker:dropoff") a.AddMarker(MarkerKind::ChainsDropOff, 0, "", at);
  if (id == "marker:start") a.AddMarker(MarkerKind::PlayerStart, 0, "", at);
}

bool Matches(const std::string& name, const char* filter) {
  if (!filter[0]) return true;
  return Lower(name).find(Lower(filter)) != std::string::npos;
}

// A square tile with a picture (or text while it loads) and a caption. Returns true when clicked.
bool Tile(App& a, const char* id, ImTextureID tex, const std::string& caption, bool selected, float size, const char* placeholder) {
  ImGui::PushID(id);
  ImGui::BeginGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton("tile", ImVec2(size, size + ImGui::GetTextLineHeight() + 4));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 frame = selected ? 0xFFE060B0u : hovered ? 0xFF805070u : 0xFF3A3040u;
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), 0xFF281F2Cu, 4);
  if (tex) dl->AddImage(tex, ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + size - 2, p.y + size - 2));
  else {
    ImVec2 ts = ImGui::CalcTextSize(placeholder);
    dl->AddText(ImVec2(p.x + (size - ts.x) / 2, p.y + (size - ts.y) / 2), 0xFF9A90A0u, placeholder);
  }
  dl->AddRect(p, ImVec2(p.x + size, p.y + size), frame, 4, 0, selected ? 2.5f : 1.0f);
  std::string cap = caption;
  while (cap.size() > 3 && ImGui::CalcTextSize(cap.c_str()).x > size) cap = cap.substr(0, cap.size() - 4) + "..";
  ImVec2 cs = ImGui::CalcTextSize(cap.c_str());
  dl->AddText(ImVec2(p.x + (size - cs.x) / 2, p.y + size + 2), hovered || selected ? 0xFFFFFFFFu : 0xFFC8C0CCu, cap.c_str());
  ImGui::EndGroup();
  if (hovered) ImGui::SetTooltip("%s", caption.c_str());
  ImGui::PopID();
  return clicked;
}

// Lays tiles out in rows; call Next() before each tile.
struct Grid {
  float size, right;
  int col = 0, per_row;
  Grid(float s) : size(s) {
    float avail = ImGui::GetContentRegionAvail().x;
    per_row = std::max(1, int((avail + ImGui::GetStyle().ItemSpacing.x) / (s + ImGui::GetStyle().ItemSpacing.x)));
    right = avail;
  }
  void Next() {
    if (col > 0 && col < per_row) ImGui::SameLine();
    if (col >= per_row) col = 0;
    ++col;
  }
};

std::string g_selected_asset;

void ModelTab(App& a, const char* kind) {
  if (a.link.state() != AssetLink::State::Ready) return;
  std::vector<const GameMeshInfo*> list;
  for (auto& m : a.link.Meshes())
    if (m.kind == kind && Matches(m.name, a.asset_search)) list.push_back(&m);
  if (!std::strcmp(kind, "vehicle"))
    ImGui::TextDisabled("Car bodies as scenery (no wheels, can't be driven). For drivable cars use Shapes > Vehicle.");
  ImGui::BeginChild("grid");
  Grid g(a.asset_tile);
  for (auto* m : list) {
    g.Next();
    ModelData* md = a.Model(m->name);
    // request a preview only for tiles that are on screen
    bool visible = ImGui::IsRectVisible(ImVec2(a.asset_tile, a.asset_tile));
    if (visible) md->thumb_wanted = true;
    ImTextureID tex = md->thumb >= 0 ? TexId(a, md->thumb) : ImTextureID(0);
    if (Tile(a, m->name.c_str(), tex, ModelLabel(m->name), g_selected_asset == m->name, a.asset_tile, md->failed ? "(error)" : "loading"))
      g_selected_asset = m->name;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.AddModel(m->name);
    if (ImGui::IsItemHovered() && md->failed) ImGui::SetTooltip("%s\n%s", m->name.c_str(), md->error.c_str());
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !a.drag.active) a.drag = {true, true, m->name, -1};
  }
  if (list.empty()) ImGui::TextDisabled("Nothing matches the search.");
  ImGui::EndChild();
}

void GameTextureTab(App& a) {
  if (a.link.state() != AssetLink::State::Ready) return;
  static std::vector<int> list;
  static std::string last_filter = "\x01";
  static size_t last_count = 0;
  const auto& all = a.link.Textures();
  if (last_filter != a.asset_search || last_count != all.size()) {
    list.clear();
    for (size_t i = 0; i < all.size(); ++i)
      if (Matches(all[i].name, a.asset_search) || Matches(all[i].peg, a.asset_search)) list.push_back(int(i));
    last_filter = a.asset_search;
    last_count = all.size();
  }
  ImGui::TextDisabled("%zu textures. Click: paint the selection. Drag onto an object to paint it.", list.size());
  ImGui::BeginChild("grid");
  Grid g(a.asset_tile);
  int rows = (int(list.size()) + g.per_row - 1) / g.per_row;
  ImGuiListClipper clip;
  clip.Begin(rows, a.asset_tile + ImGui::GetTextLineHeight() + 4 + ImGui::GetStyle().ItemSpacing.y);
  while (clip.Step())
    for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
      for (int c = 0; c < g.per_row; ++c) {
        size_t k = size_t(row) * size_t(g.per_row) + size_t(c);
        if (k >= list.size()) break;
        const GameTextureInfo& t = all[size_t(list[k])];
        if (c) ImGui::SameLine();
        int thumb = a.TextureThumb(t.name);
        std::string cap = t.name.substr(0, t.name.rfind('.'));
        if (Tile(a, t.name.c_str(), thumb >= 0 ? TexId(a, thumb) : ImTextureID(0), cap, false, a.asset_tile, "..."))
          a.ApplyTextureToSelection(AddGameTexture(a.map, t.name)), a.RequestMapAssets();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%d x %d, %s", t.name.c_str(), t.w, t.h, t.peg.c_str());
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !a.drag.active) a.drag = {true, false, t.name, -1};
      }
    }
  ImGui::EndChild();
}

void MapTextureTab(App& a) {
  if (ImGui::Button("+ Built-in")) ImGui::OpenPopup("builtin");
  ImGui::SameLine();
  if (ImGui::Button("+ Import image...")) {
    fs::path p = OpenImageDialog(a.hwnd);
    if (!p.empty()) {
      TextureRef t;
      t.file = p.u8string();
      t.name = p.stem().u8string();
      t.image = LoadImageFile(t.file);
      if (t.image.w) {
        a.map.textures.push_back(std::move(t));
        a.ApplyTextureToSelection(int(a.map.textures.size() - 1));
        a.MarkDirty();
      } else {
        a.status = "Could not read " + p.filename().u8string();
      }
    }
  }
  if (ImGui::BeginPopup("builtin")) {
    auto& list = BuiltinTextures();
    for (size_t i = 0; i < list.size(); ++i) {
      ImGui::PushID(int(i));
      ImTextureID id = i < a.builtin_thumbs.size() ? TexId(a, a.builtin_thumbs[i]) : ImTextureID(0);
      if (ImGui::ImageButton("b", id, ImVec2(48, 48))) {
        a.ApplyTextureToSelection(AddBuiltinTexture(a.map, list[i].id));
        a.MarkDirty();
        ImGui::CloseCurrentPopup();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", list[i].label);
      if ((i + 1) % 4) ImGui::SameLine();
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Click: paint the selection / new objects. Drag onto an object.");
  int t = a.current_texture;
  MapTexturePicker(a, t, a.asset_tile * 0.7f);
  if (t != a.current_texture) a.ApplyTextureToSelection(t);
}

// Pictures for the Shapes tab: the shapes in concrete, the markers as their coloured boxes.
void BuildShapeIcons(App& a) {
  for (auto& it : kAddItems) {
    std::string id = it.id;
    Object o;
    float tint[4] = {1, 1, 1, 1};
    int tex = a.builtin_thumbs.empty() ? -1 : a.builtin_thumbs[0];
    bool found = false;
    for (Shape s : {Shape::Box, Shape::Ramp, Shape::Stairs, Shape::Cylinder, Shape::Wedge})
      if (id == std::string("shape:") + ShapeName(s)) {
        o.shape = s;
        o.size = s == Shape::Stairs ? V3{3, 3, 5} : s == Shape::Cylinder ? V3{2, 4, 2} : s == Shape::Ramp ? V3{3, 2, 6} : V3{4, 2, 4};
        o.steps = 8;
        o.uv_metres = 2;
        found = true;
      }
    if (!found) {
      MarkerObj m;
      if (id == "marker:red") m.team = 1;
      if (id == "marker:blue") m.team = 2;
      if (id == "marker:weapon") m.kind = MarkerKind::Weapon;
      if (id == "marker:vehicle") m.kind = MarkerKind::Vehicle;
      if (id == "marker:dropoff") m.kind = MarkerKind::ChainsDropOff;
      if (id == "marker:start") m.kind = MarkerKind::PlayerStart;
      float mn[3], mx[3];
      MarkerBox(m, mn, mx);
      o.size = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
      uint32_t c = MarkerColour(m);
      tint[0] = float(c & 255) / 255;
      tint[1] = float((c >> 8) & 255) / 255;
      tint[2] = float((c >> 16) & 255) / 255;
      tex = -1;
    }
    std::vector<std::pair<std::vector<MeshVertex>, int>> parts(1);
    parts[0].second = tex;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& t : ObjectTriangles(o))
      for (int k = 0; k < 3; ++k) {
        parts[0].first.push_back({{t.p[k][0], t.p[k][1], t.p[k][2]}, {t.n[k][0], t.n[k][1], t.n[k][2]}, {t.uv[k][0], t.uv[k][1]}});
        for (int ax = 0; ax < 3; ++ax) { mn[ax] = std::min(mn[ax], t.p[k][ax]); mx[ax] = std::max(mx[ax], t.p[k][ax]); }
      }
    a.shape_icons.push_back(a.RenderPreview(parts, mn, mx, tint));
  }
}

void ShapesTab(App& a) {
  if (a.shape_icons.empty()) BuildShapeIcons(a);
  ImGui::TextDisabled("Click to add under the screen centre, or drag into the view.");
  Grid g(a.asset_tile);
  size_t n = 0;
  for (auto& it : kAddItems) {
    g.Next();
    int icon = n < a.shape_icons.size() ? a.shape_icons[n] : -1;
    ++n;
    if (Tile(a, it.id, icon >= 0 ? TexId(a, icon) : ImTextureID(0), it.label, false, a.asset_tile, it.label)) AddById(a, it.id, nullptr);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !a.drag.active) a.drag = {true, false, it.id, -2};
  }
}

}  // namespace

static void AssetsWindow(App& a, float top) {
  ImGui::SetNextWindowPos(ImVec2(316, float(a.height) - 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(float(a.width) - 316 - 336, 260), ImGuiCond_FirstUseEver);
  (void)top;
  if (!ImGui::Begin("Assets")) {
    ImGui::End();
    return;
  }
  const char* tabs[] = {"Shapes", "Props", "Vehicles", "Game textures", "Map textures"};
  if (ImGui::BeginTabBar("assettabs")) {
    for (int i = 0; i < 5; ++i)
      if (ImGui::BeginTabItem(tabs[i], nullptr, i == a.asset_tab_request ? ImGuiTabItemFlags_SetSelected : 0)) {
        a.asset_tab = i;
        ImGui::EndTabItem();
      }
    a.asset_tab_request = -1;
    ImGui::EndTabBar();
  }
  bool game_tab = a.asset_tab >= 1 && a.asset_tab <= 3;
  if (a.asset_tab != 0 && a.asset_tab != 4) {
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::InputTextWithHint("##search", "Search...", a.asset_search, sizeof a.asset_search);
    ImGui::SameLine();
  }
  ImGui::SetNextItemWidth(120);
  ImGui::SliderFloat("##tile", &a.asset_tile, 48, 160, "Size %.0f");
  if (game_tab) {
    auto st = a.link.state();
    if (a.game_dir.empty()) {
      ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Choose your Saints Reborn folder first (File > Settings).");
    } else if (st == AssetLink::State::Starting) {
      ImGui::TextDisabled("Reading the game files...");
    } else if (st == AssetLink::State::Failed) {
      ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "Game assets unavailable: %s", a.link.error().c_str());
      if (ImGui::Button("Try again")) a.StartAssets();
    } else if (st == AssetLink::State::Off) {
      if (ImGui::Button("Load game assets")) a.StartAssets();
    }
  }
  switch (a.asset_tab) {
    case 0: ShapesTab(a); break;
    case 1: ModelTab(a, "prop"); break;
    case 2: ModelTab(a, "vehicle"); break;
    case 3: GameTextureTab(a); break;
    case 4: MapTextureTab(a); break;
  }
  ImGui::End();
}

// ============================================================================ panels

static void Toolbar(App& a, float top) {
  ImGui::SetNextWindowPos(ImVec2(0, top));
  ImGui::SetNextWindowSize(ImVec2(float(a.width), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::Begin("toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleVar();
  auto toggle = [&](const char* label, bool on, const char* tip) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.17f, 0.25f, 1));
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine();
    return r;
  };
  if (toggle("View (Q)", a.tool == ToolView, "Hand tool: left mouse pans the view")) a.tool = ToolView;
  if (toggle("Move (W)", a.tool == ToolMove, "Move: drag an arrow, or a square to move on a plane")) a.tool = ToolMove;
  if (toggle("Rotate (E)", a.tool == ToolRotate, "Rotate: drag a ring")) a.tool = ToolRotate;
  if (toggle("Scale (R)", a.tool == ToolScale, "Scale / size: drag a handle")) a.tool = ToolScale;
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (toggle(a.pivot_center ? "Center" : "Pivot", false, "Pivot: the gizmo sits on the active object.\nCenter: in the middle of everything selected."))
    a.pivot_center = !a.pivot_center;
  if (toggle(a.gizmo_local ? "Local" : "Global", false, "Local: gizmo follows the object's rotation.\nGlobal: gizmo follows the world axes."))
    a.gizmo_local = !a.gizmo_local;
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::Checkbox("Snap (G)", &a.snap);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold Ctrl while dragging to do the opposite");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(78);
  ImGui::DragFloat("##sm", &a.snap_move, 0.05f, 0.05f, 50, "%.2f m", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move step (click to type)");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(66);
  ImGui::DragFloat("##sa", &a.snap_angle, 1, 1, 180, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate step (click to type)");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(60);
  ImGui::DragFloat("##ss", &a.snap_scale, 0.01f, 0.01f, 10, "x%.2f", ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scale step (click to type)");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  ImGui::BeginDisabled(a.sel.Empty());
  if (ImGui::Button("Duplicate")) a.DuplicateSelection();
  ImGui::SameLine();
  if (RedButton("Delete")) a.DeleteSelection();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.18f, 0.65f, 1));
  float ew = ImGui::CalcTextSize("Export map pack").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ew));
  if (ImGui::Button("Export map pack")) { a.export_log = a.Export(); a.show_export_log = true; }
  ImGui::PopStyleColor();
  ImGui::End();
}

static void ItemContextMenu(App& a) {
  bool any = !a.sel.Empty();
  if (ImGui::MenuItem("Focus", "F", false, any)) a.FocusSelection();
  if (ImGui::MenuItem("Drop to ground", "End", false, any)) a.DropToGround();
  ImGui::Separator();
  if (ImGui::MenuItem("Copy", "Ctrl+C", false, any)) a.CopySelection();
  if (ImGui::MenuItem("Paste", "Ctrl+V", false, !a.clipboard.objects.empty() || !a.clipboard.markers.empty())) a.Paste();
  if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, any)) a.DuplicateSelection();
  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.45f, 1));
  if (ImGui::MenuItem(a.sel.Count() > 1 ? "Delete selected" : "Delete", "Del", false, any)) a.DeleteSelection();
  ImGui::PopStyleColor();
}

static void Hierarchy(App& a, float top) {
  ImGui::SetNextWindowPos(ImVec2(8, top + 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 560), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Hierarchy")) {
    ImGui::End();
    return;
  }
  char name[128];
  strncpy_s(name, a.map.name.c_str(), _TRUNCATE);
  if (ImGui::InputText("Map name", name, sizeof name)) { a.map.name = name; a.MarkDirty(); }
  if (ImGui::IsItemActivated()) a.PushUndo();
  int spawns = 0, weapons = 0, teams = 0, drops = 0;
  for (auto& m : a.map.markers) {
    spawns += m.kind == MarkerKind::Spawn;
    teams += m.kind == MarkerKind::Spawn && m.team;
    weapons += m.kind == MarkerKind::Weapon;
    drops += m.kind == MarkerKind::ChainsDropOff;
  }
  ImGui::TextDisabled("%d spawns (%d team), %d weapons, %d drop-offs", spawns, teams, weapons, drops);
  if (spawns < 4) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "Add spawn points: 8 or more play best");
  static char filter[64] = "";
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##filter", "Filter...", filter, sizeof filter);
  ImGui::Separator();
  static int anchor_kind = -1, anchor_index = -1;
  auto row = [&](SelItem it, const std::string& label, const char* tag) {
    if (!Matches(label, filter)) return;
    ImGui::PushID(it.kind * 100000 + it.index);
    bool on = a.sel.Has(it.kind, it.index);
    ImGui::TextDisabled("%s", tag);
    ImGui::SameLine(46);
    if (ImGui::Selectable(label.c_str(), on)) {
      ImGuiIO& io = ImGui::GetIO();
      if (io.KeyCtrl) a.sel.Toggle(it);
      else if (io.KeyShift && anchor_kind == it.kind) {
        int lo = std::min(anchor_index, it.index), hi = std::max(anchor_index, it.index);
        for (int i = lo; i <= hi; ++i) a.sel.Add({it.kind, i});
        a.sel.Add(it);
      } else a.sel.Set(it);
      if (!io.KeyShift) { anchor_kind = it.kind; anchor_index = it.index; }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) a.FocusSelection();
    if (ImGui::BeginPopupContextItem("ctx")) {
      if (!a.sel.Has(it.kind, it.index)) a.sel.Set(it);
      ItemContextMenu(a);
      ImGui::EndPopup();
    }
    ImGui::PopID();
  };
  if (ImGui::BeginChild("list", ImVec2(0, 0))) {
    for (size_t i = 0; i < a.map.objects.size(); ++i) {
      auto& o = a.map.objects[i];
      const char* tag = o.shape == Shape::Model ? "[prop]" : "[shape]";
      row({SelItem::Obj, int(i)}, ItemLabel(a, {SelItem::Obj, int(i)}), tag);
    }
    for (size_t i = 0; i < a.map.markers.size(); ++i) row({SelItem::Mark, int(i)}, ItemLabel(a, {SelItem::Mark, int(i)}), "[mark]");
    // click in the empty part of the list: deselect
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) a.sel.Clear();
  }
  ImGui::EndChild();
  ImGui::End();
}

static void Inspector(App& a, float top) {
  ImGui::SetNextWindowPos(ImVec2(float(a.width) - 336, top + 8), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(328, 520), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Inspector")) {
    ImGui::End();
    return;
  }
  auto undoable = [&]() { if (ImGui::IsItemActivated()) a.PushUndo(); if (ImGui::IsItemEdited()) a.MarkDirty(); };
  auto buttons = [&]() {
    ImGui::Separator();
    if (ImGui::Button("Focus (F)")) a.FocusSelection();
    ImGui::SameLine();
    if (ImGui::Button("Drop to ground")) a.DropToGround();
    ImGui::SameLine();
    if (ImGui::Button("Duplicate")) a.DuplicateSelection();
    if (RedButton(a.sel.Count() > 1 ? "Delete selected (Del)" : "Delete (Del)", ImVec2(-1, 0))) a.DeleteSelection();
  };
  if (a.sel.Count() > 1) {
    ImGui::Text("%zu items selected", a.sel.Count());
    ImGui::TextDisabled("Move / rotate / scale them together with the gizmo.");
    ImGui::TextDisabled("Pivot / Center in the toolbar sets where it sits.");
    ImGui::Separator();
    ImGui::Text("Paint the shapes with:");
    int t = a.current_texture;
    MapTexturePicker(a, t, 40);
    if (t != a.current_texture) a.ApplyTextureToSelection(t);
    buttons();
  } else if (const SelItem* s = a.sel.Active(); s && s->kind == SelItem::Obj) {
    Object& o = a.Obj(s->index);
    char name[128];
    strncpy_s(name, o.name.c_str(), _TRUNCATE);
    if (ImGui::InputText("Name", name, sizeof name)) o.name = name;
    undoable();
    if (o.shape == Shape::Model) {
      ImGui::Text("Game model: %s", o.asset.c_str());
      ModelData* md = a.Model(o.asset);
      if (md->failed) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "Could not load: %s", md->error.c_str());
      else if (!md->loaded) ImGui::TextDisabled("Loading from the game files...");
      else ImGui::TextDisabled("%zu triangles, %zu materials", md->in.tris.size(), md->in.materials.size());
    } else {
      int shape = int(o.shape);
      const char* shapes[] = {"Box", "Ramp", "Stairs", "Cylinder", "Wedge"};
      if (ImGui::Combo("Shape", &shape, shapes, 5)) { a.PushUndo(); o.shape = Shape(shape); }
    }
    ImGui::SeparatorText("Transform");
    Vec3Row(a, "Position", o.pos.data(), 0.05f, -4000, 4000, "%.2f");
    Vec3Row(a, "Rotation", o.rot.data(), 0.5f, -720, 720, "%.1f");
    if (o.shape == Shape::Model) Vec3Row(a, "Scale", o.scale.data(), 0.01f, 0.01f, 100, "%.2f");
    else Vec3Row(a, "Size", o.size.data(), 0.05f, 0.05f, 2000, "%.2f");
    ImGui::TextDisabled("Click a number to type it; drag it (or X/Y/Z) to change it.");
    if (o.shape != Shape::Model) {
      ImGui::SeparatorText("Surface");
      if (o.shape == Shape::Stairs) { ImGui::DragInt("Steps", &o.steps, 0.2f, 2, 60, "%d", ImGuiSliderFlags_AlwaysClamp); undoable(); }
      ImGui::DragFloat("Texture scale", &o.uv_metres, 0.05f, 0.1f, 64, "%.2f m / repeat", ImGuiSliderFlags_AlwaysClamp);
      undoable();
      ImGui::Text("Texture: %s", o.texture >= 0 && size_t(o.texture) < a.map.textures.size() ? a.map.textures[size_t(o.texture)].name.c_str() : "?");
      int t = o.texture;
      MapTexturePicker(a, t, 40);
      if (t != o.texture) a.ApplyTextureToSelection(t);
      ImGui::TextDisabled("More in Assets > Game textures.");
    }
    buttons();
  } else if (s && s->kind == SelItem::Mark) {
    MarkerObj& m = a.Mark(s->index);
    int kind = int(m.kind);
    const char* kinds[] = {"Spawn point", "Player start", "Weapon", "Vehicle", "Chains drop-off"};
    if (ImGui::Combo("Kind", &kind, kinds, 5)) { a.PushUndo(); m.kind = MarkerKind(kind); if (m.kind == MarkerKind::Weapon && m.type.empty()) m.type = "ak47"; }
    if (m.kind == MarkerKind::Spawn) {
      const char* teams[] = {"Any", "Red (team 1)", "Blue (team 2)"};
      if (ImGui::Combo("Team", &m.team, teams, 3)) a.PushUndo();
    }
    if (m.kind == MarkerKind::Weapon) {
      int w = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kWeapons); ++i) if (m.type == kWeapons[i]) w = i;
      if (ImGui::Combo("Weapon", &w, kWeaponLabels, IM_ARRAYSIZE(kWeaponLabels))) { a.PushUndo(); m.type = kWeapons[w]; }
    }
    if (m.kind == MarkerKind::Vehicle) {
      char v[64];
      strncpy_s(v, m.type.c_str(), _TRUNCATE);
      if (ImGui::InputText("Vehicle", v, sizeof v)) m.type = v;
      undoable();
      if (ImGui::BeginCombo("Presets", nullptr, ImGuiComboFlags_NoPreview)) {
        for (auto* x : kVehicles) if (ImGui::Selectable(x)) { a.PushUndo(); m.type = x; }
        if (a.link.state() == AssetLink::State::Ready)
          for (auto& gm : a.link.Meshes())
            if (gm.kind == "vehicle" && ImGui::Selectable(ModelLabel(gm.name).c_str())) { a.PushUndo(); m.type = ModelLabel(gm.name); }
        ImGui::EndCombo();
      }
    }
    ImGui::SeparatorText("Transform");
    Vec3Row(a, "Position", m.pos.data(), 0.05f, -4000, 4000, "%.2f");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Facing");
    ImGui::SameLine(ImGui::GetFontSize() * 5.2f);
    ImGui::SetNextItemWidth(-1);
    ImGui::DragFloat("##facing", &m.yaw, 1, -360, 360, "%.0f deg");
    undoable();
    ImGui::TextDisabled("The arrow on the floor shows where it faces.");
    buttons();
  } else {
    ImGui::TextDisabled("Click something in the view to edit it.");
    ImGui::TextDisabled("Drag in empty space to select several.");
    ImGui::Separator();
    ImGui::Text("Texture for new shapes:");
    int t = a.current_texture;
    MapTexturePicker(a, t, 40);
    a.current_texture = t;
  }
  ImGui::End();
}

// ============================================================================ dialogs

static void FolderDialog(App& a) {
  if (a.show_folder_dialog && !ImGui::IsPopupOpen("Choose your Saints Reborn folder")) {
    ImGui::OpenPopup("Choose your Saints Reborn folder");
    a.folder_dialog_text = a.game_dir.u8string();
  }
  ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(a.width / 2.0f, a.height / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal("Choose your Saints Reborn folder", nullptr, ImGuiWindowFlags_NoResize)) return;
  ImGui::TextWrapped(
      "The editor builds maps from your own copy of the game: its props, textures and the multiplayer map template "
      "are read from your Saints Reborn folder. Nothing from the game comes with the editor.");
  ImGui::Spacing();
  ImGui::TextWrapped("Pick the folder Saints Reborn is installed in (the one with game\\packfiles, or packfiles itself).");
  ImGui::Spacing();
  char buf[1024];
  strncpy_s(buf, a.folder_dialog_text.c_str(), _TRUNCATE);
  ImGui::SetNextItemWidth(-110);
  if (ImGui::InputText("##folder", buf, sizeof buf)) a.folder_dialog_text = buf;
  ImGui::SameLine();
  if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
    fs::path p = BrowseForFolder(a.hwnd, fs::u8path(a.folder_dialog_text));
    if (!p.empty()) a.folder_dialog_text = p.u8string();
  }
  fs::path resolved = ResolveGameDir(fs::u8path(a.folder_dialog_text));
  if (resolved.empty())
    ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), a.folder_dialog_text.empty() ? "No folder chosen yet." : "No Saints Reborn game files in this folder.");
  else
    ImGui::TextColored(ImVec4(0.45f, 1, 0.45f, 1), "Game files found: %s", (resolved / "packfiles").u8string().c_str());
  ImGui::Spacing();
  ImGui::BeginDisabled(resolved.empty());
  if (ImGui::Button("Use this folder", ImVec2(200, 0))) {
    a.SetGameDir(resolved);
    a.show_folder_dialog = false;
    a.status = "Game folder set. Game props and textures are in the Assets panel.";
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (a.game_dir_confirmed) {
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      a.show_folder_dialog = false;
      ImGui::CloseCurrentPopup();
    }
  } else if (ImGui::Button("Exit the editor", ImVec2(160, 0))) {
    PostMessageW(a.hwnd, WM_CLOSE, 0, 0);
  }
  ImGui::EndPopup();
}

static void Dialogs(App& a) {
  if (a.show_export_log) {
    ImGui::SetNextWindowSize(ImVec2(660, 400), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(a.width / 2.0f, a.height / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Export", &a.show_export_log)) {
      ImGui::TextWrapped("%s", a.export_log.c_str());
      if (ImGui::Button("Open maps folder")) {
        fs::path out = a.maps_dir.empty() ? a.game_dir.parent_path() / "maps" : a.maps_dir;
        ShellExecuteW(nullptr, L"open", out.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) a.show_export_log = false;
    }
    ImGui::End();
  }
  if (a.show_settings) {
    ImGui::SetNextWindowSize(ImVec2(660, 0), ImGuiCond_Appearing);
    if (ImGui::Begin("Settings", &a.show_settings)) {
      ImGui::SeparatorText("Game");
      ImGui::TextWrapped("Saints Reborn folder: %s", a.game_dir.empty() ? "(not set)" : a.game_dir.u8string().c_str());
      if (ImGui::Button("Change folder...")) a.show_folder_dialog = true;
      ImGui::SameLine();
      const char* st[] = {"off", "reading the game files...", "ready", "failed"};
      ImGui::TextDisabled("Game assets: %s", st[int(a.link.state())]);
      if (a.link.state() == AssetLink::State::Failed) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", a.link.error().c_str());
      char v[512];
      strncpy_s(v, a.viewer_exe.u8string().c_str(), _TRUNCATE);
      if (ImGui::InputText("Asset viewer", v, sizeof v)) a.viewer_exe = fs::u8path(v);
      ImGui::TextDisabled("SaintsRowAssetViewer.exe reads the game's models and textures (normally next to the editor).");
      if (ImGui::Button("Reload game assets")) a.StartAssets();
      ImGui::SeparatorText("Export");
      char mdir[512];
      strncpy_s(mdir, a.maps_dir.u8string().c_str(), _TRUNCATE);
      if (ImGui::InputText("Maps folder", mdir, sizeof mdir)) a.maps_dir = fs::u8path(mdir);
      ImGui::TextDisabled("Empty = the game's maps folder");
      int sizes[] = {128, 256, 512, 1024}, cur = 1;
      for (int i = 0; i < 4; ++i) if (sizes[i] == a.export_texture_size) cur = i;
      const char* labels[] = {"128 (smallest)", "256 (default)", "512", "1024 (large packs)"};
      if (ImGui::Combo("Texture size", &cur, labels, 4)) a.export_texture_size = sizes[cur];
      ImGui::Spacing();
      if (ImGui::Button("Save settings")) { a.SaveSettings(); a.show_settings = false; }
    }
    ImGui::End();
  }
  if (a.show_help) {
    ImGui::SetNextWindowSize(ImVec2(600, 520), ImGuiCond_Appearing);
    if (ImGui::Begin("Controls", &a.show_help)) {
      ImGui::TextWrapped(
          "View (like Unity's scene view)\n"
          "  Right mouse held: look around; WASD move, Q/E down/up, Shift faster, wheel = fly speed\n"
          "  Mouse wheel: zoom   Middle mouse: pan   Alt + left: orbit   Alt + right: zoom\n"
          "  F: frame the selection   Axis widget (top right): click an axis to look along it\n\n"
          "Tools\n"
          "  Q view (hand)   W move   E rotate   R scale / size   G snap on/off (hold Ctrl to flip it while dragging)\n"
          "  Pivot / Center and Local / Global in the toolbar set where the gizmo sits and how it is turned\n\n"
          "Selection\n"
          "  Click: select   Ctrl+click: add/remove   Shift+click: add   Drag in empty space: rectangle   Ctrl+A: all\n"
          "  Esc: nothing   Right click: menu\n\n"
          "Editing\n"
          "  Del or Backspace: delete   Ctrl+D duplicate (in place)   Ctrl+C / Ctrl+V / Ctrl+X   End: drop to the ground\n"
          "  Ctrl+Z / Ctrl+Y undo / redo   Numbers in the Inspector: click to type, drag to change\n\n"
          "Assets\n"
          "  Shapes: blocks and markers.  Props / Vehicles: models from your game.  Game textures: paint shapes with the game's\n"
          "  textures.  Click to add or paint, or drag into the view onto the spot / the object.\n\n"
          "Map\n"
          "  1 unit = 1 metre. Every object gets collision. Put 8+ spawn points on the ground, weapons where you want pickups.\n"
          "  Red / blue spawns are used in team modes, chains drop-offs add Big Ass Chains.\n"
          "  Export map pack (Ctrl+E) writes it into the game's maps folder: start the game and pick it in the System Link lobby.");
    }
    ImGui::End();
  }
}

// ============================================================================ frame

void App::DrawUi() {
  ImGuiIO& io = ImGui::GetIO();
  ImGuizmo::BeginFrame();
  // ---------------- menu
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("New map", "Ctrl+N") && ConfirmDiscard()) NewDocument();
      if (ImGui::MenuItem("Open...", "Ctrl+O") && ConfirmDiscard()) Open({});
      if (ImGui::MenuItem("Save", "Ctrl+S")) Save(false);
      if (ImGui::MenuItem("Save as...")) Save(true);
      ImGui::Separator();
      if (ImGui::MenuItem("Export map pack", "Ctrl+E")) { export_log = Export(); show_export_log = true; }
      if (ImGui::MenuItem("Choose game folder...")) show_folder_dialog = true;
      if (ImGui::MenuItem("Settings...")) show_settings = true;
      ImGui::Separator();
      if (ImGui::MenuItem("Exit")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo.empty())) Undo();
      if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo.empty())) Redo();
      ImGui::Separator();
      if (ImGui::MenuItem("Select all", "Ctrl+A")) SelectAll();
      ItemContextMenu(*this);
      ImGui::Separator();
      ImGui::MenuItem("Snap to grid", "G", &snap);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
      for (auto& it : kAddItems) {
        if (std::strcmp(it.id, "marker:weapon") == 0) {
          if (ImGui::BeginMenu("Weapon")) {
            for (int i = 0; i < IM_ARRAYSIZE(kWeapons); ++i)
              if (ImGui::MenuItem(kWeaponLabels[i])) AddMarker(MarkerKind::Weapon, 0, kWeapons[i]);
            ImGui::EndMenu();
          }
          continue;
        }
        if (std::strcmp(it.id, "marker:spawn") == 0) ImGui::Separator();
        if (ImGui::MenuItem(it.label)) AddById(*this, it.id, nullptr);
      }
      ImGui::Separator();
      ImGui::TextDisabled("Game props: Assets panel");
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Grid", nullptr, &show_grid);
      if (ImGui::MenuItem("Frame selection", "F")) FocusSelection();
      if (ImGui::MenuItem("Reset camera")) { cam = Camera{}; orbit_dist = 20; }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
      if (ImGui::MenuItem("Controls")) show_help = true;
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  float top = ImGui::GetFrameHeight();
  Toolbar(*this, top);
  float panels_top = top + ImGui::GetFrameHeight() + 14;
  Hierarchy(*this, panels_top);
  Inspector(*this, panels_top);
  AssetsWindow(*this, panels_top);

  // ---------------- dragging from the asset browser into the view
  if (drag.active) {
    bool over_view = !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool painting = !drag.model && drag.map_texture != -2;
    SelItem target;
    bool on_object = false;
    if (over_view && painting) {
      float o[3], d[3], t;
      on_object = MouseRay(io.MousePos.x, io.MousePos.y, o, d) && RayHit(o, d, t, &target) && target.kind == SelItem::Obj &&
                  Obj(target.index).shape != Shape::Model;
    }
    std::string what = drag.model ? "Place " + ModelLabel(drag.name) : !painting ? "Add " + drag.name.substr(drag.name.find(':') + 1)
                                                                                  : "Paint with " + drag.name;
    ImGui::SetTooltip("%s%s", what.c_str(), painting && over_view && !on_object ? "\n(drop it on a shape)" : "");
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (over_view) {
        float p[3];
        if (drag.model) {
          SurfacePoint(io.MousePos.x, io.MousePos.y, p);
          AddModel(drag.name, p);
        } else if (!painting) {
          SurfacePoint(io.MousePos.x, io.MousePos.y, p);
          AddById(*this, drag.name, p);
        } else if (on_object) {
          int tex = drag.map_texture >= 0 ? drag.map_texture : AddGameTexture(map, drag.name);
          RequestMapAssets();
          PushUndo();
          ApplyTextureToObject(target.index, tex);
          current_texture = tex;
          sel.Set(target);
        }
      }
      drag = {};
    }
  }

  // ---------------- right-click menu in the view
  if (want_context_menu) {
    ImGui::OpenPopup("viewctx");
    want_context_menu = false;
  }
  if (ImGui::BeginPopup("viewctx")) {
    static float at[3];
    if (ImGui::IsWindowAppearing()) SurfacePoint(ImGui::GetMousePosOnOpeningCurrentPopup().x, ImGui::GetMousePosOnOpeningCurrentPopup().y, at);
    if (!sel.Empty()) {
      ImGui::TextDisabled("%s", sel.Count() > 1 ? (std::to_string(sel.Count()) + " items").c_str() : ItemLabel(*this, *sel.Active()).c_str());
      ItemContextMenu(*this);
      ImGui::Separator();
    }
    if (ImGui::BeginMenu("Add here")) {
      for (auto& it : kAddItems)
        if (ImGui::MenuItem(it.label)) AddById(*this, it.id, at);
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }

  // ---------------- rectangle selection
  if (marquee) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(marquee_from[0], marquee_from[1]), io.MousePos, 0x30E070C0u);
    dl->AddRect(ImVec2(marquee_from[0], marquee_from[1]), io.MousePos, 0xC0E070C0u);
  }

  // ---------------- status bar
  ImGui::SetNextWindowPos(ImVec2(0, float(height) - ImGui::GetFrameHeight() - 6));
  ImGui::SetNextWindowSize(ImVec2(float(width), 0));
  ImGui::Begin("status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
  size_t tris = 0;
  for (auto& c : meshes) tris += c.tris.size();
  std::string assets;
  switch (link.state()) {
    case AssetLink::State::Off: assets = game_dir.empty() ? "no game folder" : "game assets off"; break;
    case AssetLink::State::Starting: assets = "reading game files..."; break;
    case AssetLink::State::Ready: assets = link.Busy() ? "loading " + std::to_string(link.Busy()) + " game assets..." : "game assets ready"; break;
    case AssetLink::State::Failed: assets = "game assets unavailable"; break;
  }
  ImGui::TextDisabled("%zu selected | %zu triangles | snap %s | %s | %s", sel.Count(), tris, snap ? "on" : "off", assets.c_str(), status.c_str());
  ImGui::End();

  Dialogs(*this);
  FolderDialog(*this);
  DrawAxisWidget(*this);
  Gizmo(*this);
  (void)io;
}

}  // namespace editor

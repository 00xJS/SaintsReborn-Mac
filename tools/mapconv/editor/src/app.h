// Saints Reborn Map Editor: application state shared by the window code (main.cpp) and the UI (ui.cpp).
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <deque>
#include <filesystem>
#include <list>
#include <map>
#include <string>
#include <vector>

#include "assetlink.h"
#include "renderer.h"
#include "scene.h"

namespace editor {

struct Camera {
  float pos[3] = {0, 18, -30};
  float yaw = 0;       // radians, 0 = looking along +Z
  float pitch = -0.5f; // radians, negative = looking down
  float Forward(int i) const;
  DirectX::XMMATRIX View() const;
};

struct SelItem {
  enum Kind { Obj, Mark } kind = Obj;
  int index = -1;
  bool operator==(const SelItem& o) const { return kind == o.kind && index == o.index; }
};

// Several things can be selected (Ctrl/Shift click, rectangle); the last one is the active one.
struct Selection {
  std::vector<SelItem> items;
  bool Empty() const { return items.empty(); }
  size_t Count() const { return items.size(); }
  bool Has(SelItem::Kind k, int i) const {
    for (auto& s : items) if (s.kind == k && s.index == i) return true;
    return false;
  }
  const SelItem* Active() const { return items.empty() ? nullptr : &items.back(); }
  void Clear() { items.clear(); }
  void Set(SelItem s) { items = {s}; }
  void Add(SelItem s) {
    Remove(s);
    items.push_back(s);
  }
  void Remove(SelItem s) {
    for (size_t i = 0; i < items.size(); ++i) if (items[i] == s) { items.erase(items.begin() + long(i)); return; }
  }
  void Toggle(SelItem s) {
    if (Has(s.kind, s.index)) Remove(s);
    else items.push_back(s);
  }
};

struct Snapshot {
  std::string name;
  std::vector<Object> objects;
  std::vector<MarkerObj> markers;
};

struct CachedMesh {
  struct Part {
    Renderer::Mesh mesh;
    int map_texture = -1;  // primitives: map texture index
    int gpu_texture = -1;  // models: renderer texture
  };
  Object key;
  const void* model = nullptr;  // the model data the parts were built from
  bool valid = false;
  std::vector<Part> parts;
  std::vector<Tri> tris;        // world space, for picking
  float mn[3]{}, mx[3]{};
};

// A game model read through the asset viewer (model space, game coordinates).
struct ModelData {
  bool loaded = false, failed = false;
  std::string error;
  mapconv::MapInput in;
  float mn[3]{}, mx[3]{};
  std::vector<int> gpu_textures;  // per material
  int thumb = -1;                 // rendered preview (renderer texture)
  bool thumb_wanted = false;
};

struct Clipboard {
  std::vector<Object> objects;
  std::vector<MarkerObj> markers;
};

enum Tool { ToolView = 0, ToolMove = 1, ToolRotate = 2, ToolScale = 3 };

struct App {
  // window / device
  HWND hwnd = nullptr;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain> swap;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11DepthStencilView> dsv;
  int width = 1600, height = 900;
  Renderer r;

  // document
  Map map;
  std::filesystem::path path;  // empty = unsaved
  bool dirty = false;
  Selection sel;
  std::deque<Snapshot> undo, redo;
  Clipboard clipboard;

  // view state
  Camera cam;
  float fly_speed = 12.0f;
  float orbit_dist = 20.0f;
  std::vector<CachedMesh> meshes;
  std::vector<int> gpu_textures;          // per map texture
  std::vector<const void*> gpu_texture_src;
  std::vector<int> builtin_thumbs;        // per built-in texture
  int tool = ToolMove;
  bool gizmo_local = true;
  bool pivot_center = false;              // gizmo at the selection's centre (else at the active object)
  bool snap = true;
  float snap_move = 0.5f, snap_angle = 15.0f, snap_scale = 0.1f;
  bool show_grid = true;
  int current_texture = 0;
  bool gizmo_was_using = false;

  // game assets (through the asset viewer)
  AssetLink link;
  std::map<std::string, ModelData> models;             // by asset name (lower case)
  std::map<std::string, int> texture_thumbs;           // game texture name -> renderer texture
  std::list<std::string> texture_thumb_lru;
  ComPtr<ID3D11DepthStencilView> thumb_dsv;
  std::vector<std::pair<std::string, int>> recent_textures;

  // UI state
  int asset_tab = 0;
  int asset_tab_request = -1;              // switch the asset browser to this tab
  char asset_search[128] = "";
  float asset_tile = 84;
  struct Drag {
    bool active = false;
    bool model = false;       // model or texture
    std::string name;
    int map_texture = -1;     // dragging one of the map's textures
  } drag;
  bool marquee = false;
  float marquee_from[2]{};
  bool want_context_menu = false;
  bool show_folder_dialog = false;
  std::string folder_dialog_text;

  // settings / environment
  std::filesystem::path exe_dir, game_dir, maps_dir, viewer_exe;
  bool game_dir_confirmed = false;
  int export_texture_size = 256;
  std::string status;
  std::string export_log;
  bool show_export_log = false, show_help = false, show_settings = false;

  void PushUndo();
  void Undo();
  void Redo();
  void MarkDirty() { dirty = true; }
  void SyncGpu();                          // meshes + textures follow the map
  Object& Obj(int i) { return map.objects[size_t(i)]; }
  MarkerObj& Mark(int i) { return map.markers[size_t(i)]; }
  void DeleteSelection();
  void DuplicateSelection();
  void CopySelection();
  void Paste();
  void SelectAll();
  bool PlacementPoint(float out[3]);       // where new things go: under the screen centre
  bool MouseRay(float mx, float my, float o[3], float d[3]) const;
  // Nearest hit along a ray (objects and markers); skip = items to ignore (the thing being placed).
  bool RayHit(const float o[3], const float d[3], float& t, SelItem* hit, const Selection* skip = nullptr) const;
  bool SurfacePoint(float mx, float my, float out[3]);
  void AddObject(Shape s, const float* at = nullptr);
  void AddModel(const std::string& asset, const float* at = nullptr);
  void AddMarker(mapconv::MarkerKind k, int team = 0, const char* type = "", const float* at = nullptr);
  void Pick(float mx, float my, int mode);  // 0 replace, 1 toggle (Ctrl), 2 add (Shift)
  void MarqueeSelect(const float a[2], const float b[2], bool additive);
  bool ItemBounds(const SelItem& s, float mn[3], float mx[3]) const;
  bool SelectionBounds(float mn[3], float mx[3]) const;
  void FocusSelection();
  void DropToGround();
  void ApplyTextureToSelection(int map_texture);
  void ApplyTextureToObject(int object, int map_texture);
  bool Save(bool save_as);
  bool Open(const std::filesystem::path& p);
  void NewDocument();
  bool ConfirmDiscard();
  std::string Export();                    // returns the report text
  void LoadSettings();
  void SaveSettings();
  void UpdateTitle();
  void RenderScene();
  void DrawUi();
  void HandleInput();

  // game assets
  bool SetGameDir(const std::filesystem::path& picked);   // validates, saves, restarts the asset link
  void StartAssets();
  void PumpAssets();                        // every frame: results from the asset viewer
  void ProcessThumbnails();                 // every frame, before the scene: renders model previews
  // Renders a 128 px preview (parts: vertices + renderer texture) into a new renderer texture.
  int RenderPreview(const std::vector<std::pair<std::vector<MeshVertex>, int>>& parts, const float mn[3], const float mx[3],
                    const float tint[4]);
  std::vector<int> shape_icons;             // Shapes tab
  ModelData* Model(const std::string& asset, bool request = true);
  int TextureThumb(const std::string& name);  // -1 while loading
  bool WaitForGameAssets(int timeout_ms);   // export / tests: everything the map uses is loaded
  void RequestMapAssets();
};

extern App* g_app;
std::filesystem::path FindGameDir(const std::filesystem::path& start);
std::filesystem::path BrowseForFolder(HWND owner, const std::filesystem::path& initial);
void MarkerBox(const MarkerObj& m, float mn[3], float mx[3]);
uint32_t MarkerColour(const MarkerObj& m);

}  // namespace editor

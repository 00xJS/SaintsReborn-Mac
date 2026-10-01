// The editor's map: primitives, game models, markers and textures, in game space (metres, Y up, left-handed like
// Direct3D: +X right, +Z forward). Saved as JSON (.srmap); exported through the converter's BuildPack.
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model.h"

namespace editor {

using V3 = std::array<float, 3>;

enum class Shape { Box, Ramp, Stairs, Cylinder, Wedge, Model };
const char* ShapeName(Shape s);

struct TextureRef {
  std::string name;
  std::string builtin;  // built-in generator id, or
  std::string file;     // image file (relative to the map file when possible), or
  std::string game;     // texture name in the player's own game files (read through the asset viewer)
  mapconv::Image image; // loaded pixels (not saved)
  bool pending = false; // game texture still loading
};

struct Object {
  Shape shape = Shape::Box;
  std::string name;
  V3 pos{0, 0, 0};      // bottom centre
  V3 rot{0, 0, 0};      // degrees: pitch (X), yaw (Y), roll (Z)
  V3 size{4, 1, 4};     // width (X), height (Y), depth (Z); models: unused
  V3 scale{1, 1, 1};    // models only
  std::string asset;    // models: file name in the game files, e.g. "box.smesh_xbox2"
  int texture = 0;
  float uv_metres = 2;  // metres per texture repeat
  int steps = 8;        // stairs
  bool collision = true;
};

struct MarkerObj {
  mapconv::MarkerKind kind = mapconv::MarkerKind::Spawn;
  V3 pos{0, 0, 0};
  float yaw = 0;        // degrees, 0 = facing +Z
  int team = 0;
  std::string type;     // weapon / vehicle type
};
const char* MarkerLabel(const MarkerObj& m);

struct Map {
  std::string name = "My Map";
  std::vector<TextureRef> textures;
  std::vector<Object> objects;
  std::vector<MarkerObj> markers;
};

struct Tri {
  V3 p[3];
  V3 n[3];
  float uv[3][2];
};
// Triangles of an object in world space. Front faces: cross(p1 - p0, p2 - p0) along the outward normal
// (the game's convention).
// Game models: the editor registers where their geometry comes from (model space, game coordinates).
// Returns nullptr while a model is not loaded (it then has no triangles).
using ModelSource = std::function<const mapconv::MapInput*(const std::string& asset)>;
void SetModelSource(ModelSource source);
const mapconv::MapInput* FindModel(const std::string& asset);
std::vector<Tri> ObjectTriangles(const Object& o);
// Model triangles with their material index (world space); empty for primitives.
std::vector<std::pair<Tri, int>> ModelTriangles(const Object& o);
// World transform helpers
V3 Rotate(const V3& rot_deg, const V3& v);

Map NewMap();
std::string SaveMap(const Map& m, const std::string& base_dir);            // JSON text
Map LoadMap(const std::string& json, const std::string& base_dir);        // throws on bad files
void LoadTextures(Map& m, const std::string& base_dir);                   // fills TextureRef::image
int AddBuiltinTexture(Map& m, const std::string& id);                     // returns index (reuses)
int AddGameTexture(Map& m, const std::string& name);                      // game texture by name (reuses)
mapconv::MapInput ToMapInput(const Map& m);   // models must be loaded (FindModel)
std::string ModelLabel(const std::string& asset);   // "box.smesh_xbox2" -> "box"

}  // namespace editor

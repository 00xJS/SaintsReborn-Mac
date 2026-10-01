// The converter's neutral map description: what importers (glTF today) produce and the builder consumes.
// Everything is in game space: metres, Y up, left-handed (glTF x is mirrored on import).
#pragma once
#include <array>
#include <string>
#include <vector>
#include "peg.h"

namespace mapconv {

using V3 = std::array<float, 3>;

struct InMaterial {
  std::string name;
  Image image;  // base colour (already multiplied by the material's colour factor)
};

struct InTriangle {
  V3 p[3];
  V3 n[3];
  float uv[3][2];
  int material;
};

enum class MarkerKind { Spawn, PlayerStart, Weapon, Vehicle, ChainsDropOff };

struct Marker {
  MarkerKind kind;
  V3 pos;
  float yaw = 0;    // radians
  int team = 0;     // 0 = any, 1 / 2 = team spawns
  std::string type; // weapon or vehicle type
  std::string source;
};

struct MapSettings {
  std::string display_name;  // shown in the lobby
  std::string map_id;        // internal name (mp_...)
  std::string folder;        // pack folder name
  int texture_size = 256;
  bool outdoor = true;
};

struct MapInput {
  std::vector<InMaterial> materials;
  std::vector<InTriangle> tris;
  std::vector<Marker> markers;
  std::vector<std::string> warnings;
};

}  // namespace mapconv

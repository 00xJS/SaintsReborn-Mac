// Builds a map pack folder (maps\<Folder>\) from a MapInput, using the player's own game files as the
// template (mp_crib: chunk structure, materials, loading screen) - nothing from the game is shipped.
#pragma once
#include <filesystem>
#include <functional>
#include "model.h"

namespace mapconv {

struct BuildReport {
  size_t triangles = 0, vertices = 0, meshes = 0, textures = 0, spawns = 0, weapons = 0, vehicles = 0, dropoffs = 0;
  std::vector<std::string> modes;
  std::vector<std::string> warnings;
  std::filesystem::path folder;
};

using Log = std::function<void(const std::string&)>;

BuildReport BuildPack(const MapInput& in, const MapSettings& settings, const std::filesystem::path& packfiles_dir,
                      const std::filesystem::path& maps_dir, const Log& log);

}  // namespace mapconv

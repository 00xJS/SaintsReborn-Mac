#pragma once
#include <filesystem>
#include "model.h"

namespace mapconv {

// Reads a .glb / .gltf scene. Nodes named like markers (spawn*, team1_spawn*, player_start, weapon_<type>,
// vehicle_<type>, chains_dropoff*) or with custom property sr_type become markers; every other mesh is
// map geometry. Custom properties (glTF "extras") sr_type / sr_value override the name.
MapInput ImportGltf(const std::filesystem::path& path);

}  // namespace mapconv

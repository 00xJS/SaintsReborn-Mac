// Fair play for online matches: tells the runtime whether this game is
// modded (online_integrity.cpp).
#pragma once

#include <filesystem>

namespace sr {
// Checks the enabled mods and custom maps once, then watches for cheat tools
// and debuggers, and shows online notices. Call after wml::Start.
void StartOnlineIntegrity(const std::filesystem::path& exe_dir);
}  // namespace sr

// Discord status (discord_presence.cpp): "Playing Saints Reborn", what the
// player is doing and for how long.
#pragma once

#include <cstdint>
#include <filesystem>

namespace sr {
// Starts the background thread. Off when the file discord.off is next to the
// exe or no Discord application id is known.
void StartDiscordPresence(const std::filesystem::path& exe_dir, uint8_t* guest_base);
}  // namespace sr

// Whompay's Mod Loader - game side.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace wml {

// Reads mods/modlist.ini, runs patch scripts and registers the file
// replacements of enabled mods. Call after the runtime has mounted the game
// folder, before the XEX loads.
void Initialize(const std::filesystem::path& exe_dir, const std::filesystem::path& game_dir,
                rex::filesystem::VirtualFileSystem* vfs, const std::string& game_mount_path);

// Loads code (DLL) and script (Lua) mods and runs their initialisation.
// Call after the XEX has been loaded, before the game starts running.
void Start(uint8_t* guest_base);

// Called once per presented frame from the game's render thread.
void OnFrame();
// Called at the game-loop boundary before simulation/render.
void OnGameFrame();

// True when a mod has taken over a key (wml.take_key), so the game's own
// keyboard controls should ignore it.
bool KeyTaken(int virtual_key);
// Text native mods asked to show over the game (empty = nothing).
std::string OverlayText();
// The game's own text over the view (e.g. online notices), shown with it.
void SetHostOverlayText(const std::string& text);
// Fair play check of the enabled mods (fair_play.cpp): false while it still
// runs; then, per enabled mod (not the built-in parts), its folder name and
// why it is private play only ("" = it only changes how the game looks).
bool FairCheckResults(std::vector<std::pair<std::string, std::string>>& results);
// The game's own text (SetHostOverlayText), drawn as an in-game notice panel.
std::string HostOverlayText();
// In-game chat (chat.cpp): keep the overlay drawn while chat shows; mute the
// mods' keys while the player types.
void SetChatVisible(bool visible);
void SetKeysSuppressed(bool suppressed);
// Ids (folder names) of the enabled mods, without the built-in core parts.
std::vector<std::string> EnabledModIds();
// Beams mods draw over the game (overlay_beams): copied into out (10 floats
// each); false when there are none or they are stale.
bool OverlayBeams(std::vector<float>& out);
// The game camera: position, right / up / forward rows and vertical field of
// view in degrees.
bool GameCamera(float eye[3], float right[3], float up[3], float forward[3], float& fov);
// Called (on the game thread) when mod beams appear or disappear.
void SetOverlayBeamsListener(std::function<void(bool)> listener);
// Called (on the game thread) when mod text appears or disappears.
void SetOverlayTextListener(std::function<void(bool)> listener);
// True while a mod holds a key down for the keyboard controls (wml.force_key).
bool KeyForced(int virtual_key);
// Camera turn (radians, to the right) mods asked for since the last call
// (wml.turn_camera); the keyboard/mouse camera code applies it.
double TakeCameraTurn();
// Mouse and right stick movement for mods (wml.mouse_look): how far the
// gameplay camera would turn, in radians, since a mod last asked.
void AddMouseLook(double yaw, double pitch);
void TakeMouseLook(double& yaw, double& pitch);

// Limits on turning the camera with the mouse (set by mods each frame).
// yaw/pitch are the current angles (radians) relative to what they are
// limited against; turning further than the limits is dropped.
struct CameraLimit {
  bool active = false;
  double yaw = 0, pitch = 0;
  double yaw_limit = 0, pitch_up = 0, pitch_down = 0;
};
void SetCameraLimit(const CameraLimit& limit);
bool GetCameraLimit(CameraLimit& limit);

}  // namespace wml

#pragma once

#include <cstdint>

struct PPCContext;

// Private development bridge used by SaintsReborn World Studio.  All guest
// memory is sampled or changed from the game thread; the pipe thread only
// moves newline-delimited JSON between processes.
namespace sr::world_studio {

// True only for the private editor-host runtime launched by World Studio.
// Normal SaintsReborn launches never enter this mode.
bool EditorHostEnabled();

// Move gizmo for the overlay (game view client pixels). UI thread safe.
struct GizmoDraw {
  bool visible = false;  // selected object in front of the camera
  float ox = 0, oy = 0;  // object
  float ex[3] = {}, ey[3] = {};  // X, Y, Z arrow tips
  bool axis_visible[3] = {};
  int hot = -1, active = -1;  // axis under the mouse / being dragged
  bool paused = false;        // world paused
  float view_width = 0, view_height = 0;
};
bool GetGizmoDraw(GizmoDraw& out);

// Mouse wheel over the editor host's game view (notches, + = away from you).
void AddMouseWheel(float notches);

// Called at the outer game-loop boundary. Returns false while Studio has the
// game paused (a queued single-step still returns true once).
bool BeforeGameFrame(uint8_t* base);

// Called immediately before Present, after world simulation. This is the
// stable point used for snapshots and editor commands.
void OnPresent(PPCContext& ctx, uint8_t* base);

// True while the Studio's free camera owns the game camera; the game's own
// camera update (sub_8210D518) is skipped then and this writes ours instead.
bool OwnsCamera();
// Hold V in the editor host: play as the player (game input, camera, HUD).
bool PlayMode();

// Called right after the frame timer (8262FFE0) every frame.
void AfterFrameTimer(uint8_t* base);
void WriteEditorCamera(uint8_t* base);

}  // namespace sr::world_studio

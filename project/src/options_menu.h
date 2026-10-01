// PC settings rows in the game's own OPTIONS > DISPLAY menu (options_menu.cpp).
#pragma once
#include <cstdint>
#include <functional>

namespace sr {

// Window / overlay state owned by the app (main.cpp). Setters may be called
// from any thread (they post to the UI thread).
struct OptionsHost {
  std::function<bool()> is_fullscreen;
  std::function<void(bool)> set_fullscreen;
  std::function<bool()> fps_counter_shown;
  std::function<void(bool)> set_fps_counter;
};
void SetOptionsHost(OptionsHost host);

// Resolution scale in use when res_scale.txt doesn't exist (main.cpp picks it
// from the hardware); the menu shows it.
void SetDefaultResScale(int scale);

// Game graphics settings from their files (shadows.txt), right after the XEX
// is loaded; weak GPUs get lighter defaults.
void ApplyStartupGraphics(bool weak_gpu);

// Called often (from the input hook): applies rows changed with left / right
// and keeps the widescreen setting in the game's memory.
void OptionsMenuPoll(uint8_t* base);

// Aspect ratio chosen at start-up (aspect.txt; Display > Aspect Ratio, applies
// after a restart): the frame width for the 720-line frame (0 = the game's own
// 1280, i.e. 16:9) and the ratio itself.
int AspectFrameWidth();
float AspectRatioValue();
// Frame height for the same: 720, or 480 / 600 for the low resolutions
// (frame_size.txt; Display > Resolution, after a restart; always 4:3).
int FrameHeight();

}  // namespace sr

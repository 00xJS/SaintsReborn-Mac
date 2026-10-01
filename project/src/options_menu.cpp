// PC settings rows in the game's own OPTIONS > DISPLAY menu.
//
// sub_82347478 builds the Display list. Each row is added with
// sub_8228BAB0(label, item, type, -1, 1, 0); Mini Map View is a selector item
// (static at 0x8282B16C, vtable 0x82068024) whose choices are added with
// sub_82291A58: +16 + 4i choice text, +272 + 4i choice id (-1), +528 + 4i
// colour, +784 number of choices, +788 the chosen one, byte +792 = 1. Texts are
// UTF-16 big-endian with a 0 terminator.
//
// Right after the game adds the Mini Map View row, our rows are added with
// selectors of our own in guest memory (so they sit above Restore to
// Defaults). Left / right change a row's choice; most rows apply it at once
// (polled from the input hook). Resolution applies with A / Enter: the
// draw_resolution_scale cvars change and the D3D12 command processor rebuilds
// its context with the new scale at the end of the frame (IssueSwap).
//
// Settings are saved next to the exe and read at start-up:
//   res_scale.txt (1-3), start_windowed (file = windowed), vsync (file = on),
//   fps_cap.txt, show_fps (file = shown), aspect.txt (4:3 ... 21:9, read at
//   start-up; the old widescreen_off file counts as 4:3), mouse_sensitivity.txt.

#include "options_menu.h"

#include "kbm.h"
#include "perf_monitor.h"
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>

namespace {

constexpr uint32_t kMiniMapItem = 0x8282B16Cu;
constexpr uint32_t kSelectorVtable = 0x82068024u;
constexpr uint32_t kDefaultChoiceColour = 0x82816CF0u;  // what Mini Map View passes (lwz 27888 of 0x8281)
// The game object is 808 bytes (Mini Map View at 0x8282B16C, the next static at
// 0x8282B494); +800..807 are written while the selector animates, so the texts
// start well after it.
constexpr uint32_t kItemSize = 1024;
constexpr uint32_t kRowMemory = 2048;              // item + texts per row
constexpr uint32_t kWidescreenFlag = 0x8370D672u;  // set by sub_82184260 from the video mode; read by
                                                   // the camera FOV (sub_8210A860) every frame

bool FileExists(const char* name) {
  if (FILE* f = std::fopen(name, "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}
// Aspect ratio (Display > Aspect Ratio, aspect.txt). Read once at start-up:
// the game makes its frame buffers when it starts, so a change applies after
// a restart. The frame stays 720 lines tall and its width follows the ratio
// (render_fixes.cpp); the window shows it at that shape (presenter cvars
// present_aspect_x / present_aspect_y). Wider than 21:9 doesn't fit the
// 10 MB of EDRAM in one pass.
struct AspectChoice {
  const char* name;
  int x, y;
};
constexpr AspectChoice kAspects[] = {{"4:3", 4, 3},   {"5:4", 5, 4},   {"3:2", 3, 2},
                                     {"16:10", 16, 10}, {"16:9", 16, 9}, {"21:9", 21, 9}};
constexpr int kAspectCount = int(sizeof(kAspects) / sizeof(kAspects[0]));
constexpr int kAspectDefault = 4;  // 16:9

int ReadAspectChoice();

void SetFileExists(const char* name, bool exists) {
  if (exists) {
    if (FILE* f = std::fopen(name, "wb")) std::fclose(f);
  } else {
    std::remove(name);
  }
}

int ReadAspectChoice() {
  if (FILE* f = std::fopen("aspect.txt", "rb")) {
    char buf[16] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    for (int i = 0; i < kAspectCount; ++i)
      if (text == kAspects[i].name) return i;
  }
  if (FileExists("widescreen_off")) return 0;  // the old Widescreen: Off row
  return kAspectDefault;
}
const int g_aspect_running = ReadAspectChoice();

// Low resolutions (Display > Resolution, frame_size.txt, read at start-up): a
// smaller 4:3 frame. 640 x 480 is the game's own SD (4:3 TV) mode.
struct FrameChoice {
  const char* name;
  int w, h;
};
constexpr FrameChoice kFrames[] = {{"640x480", 640, 480}, {"800x600", 800, 600}};
constexpr int kFrameCount = int(sizeof(kFrames) / sizeof(kFrames[0]));
int ReadFrameChoice() {
  if (FILE* f = std::fopen("frame_size.txt", "rb")) {
    char buf[16] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    for (int i = 0; i < kFrameCount; ++i)
      if (text == kFrames[i].name) return i;
  }
  return -1;
}
const int g_frame_running = ReadFrameChoice();
float AspectRatioOf(int i) { return float(kAspects[i].x) / float(kAspects[i].y); }

int ReadScaleFile() {
  int scale = 2;
  if (FILE* f = std::fopen("res_scale.txt", "rb")) {
    int v = 0;
    if (std::fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 3) scale = v;
    std::fclose(f);
  }
  return scale;
}

// The scale in use (main.cpp reads the same file once at start-up).
int g_running_scale = ReadScaleFile();
sr::OptionsHost g_host;

constexpr int kFpsCaps[] = {30, 60, 90, 120, 0};
constexpr double kSensitivities[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0};

constexpr uint32_t kShadowSetting = 0x827D6C90u;  // r_shadows (u32)
uint32_t GuestShadowSetting() {
  uint8_t* base = REX_KERNEL_STATE()->memory()->virtual_membase();
  uint32_t v;
  std::memcpy(&v, base + kShadowSetting, 4);
  return __builtin_bswap32(v);
}
void SetGuestShadowSetting(uint32_t v) {
  uint8_t* base = REX_KERNEL_STATE()->memory()->virtual_membase();
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(base + kShadowSetting, &be, 4);
}

struct Row {
  const char* label;
  std::vector<std::string> choices;
  int (*current)();       // choice to show when the list is built
  void (*apply)(int);     // apply a choice
  bool confirm;           // apply with A / Enter (else at once)
  uint32_t item = 0;      // guest selector
  uint32_t label_text = 0;
  std::vector<uint32_t> choice_text;
  int applied = -1;       // last applied / shown choice
};

std::vector<Row>& Rows() {
  static std::vector<Row> rows = {
      {"Resolution",
       {"640 x 480 (restart)", "800 x 600 (restart)", "1280 x 720 (native)", "2560 x 1440 (2x)",
        "3840 x 2160 (3x)"},
       [] {
         const int frame = ReadFrameChoice();
         return frame >= 0 ? frame : kFrameCount + g_running_scale - 1;
       },
       [](int c) {
         if (c < kFrameCount) {
           // A low resolution: a smaller frame after a restart, drawn at 1x.
           if (FILE* f = std::fopen("frame_size.txt", "wb")) {
             std::fprintf(f, "%s\n", kFrames[c].name);
             std::fclose(f);
           }
           c = kFrameCount;
         } else {
           std::remove("frame_size.txt");
         }
         const int scale = c - kFrameCount + 1;
         if (FILE* f = std::fopen("res_scale.txt", "wb")) {
           std::fprintf(f, "%d\n", scale);
           std::fclose(f);
         }
         if (scale == g_running_scale) return;
         g_running_scale = scale;
         rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale));
         rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale));
       },
       true},
      {"Display Mode", {"Fullscreen", "Windowed"},
       [] { return (g_host.is_fullscreen && !g_host.is_fullscreen()) ? 1 : 0; },
       [](int c) {
         SetFileExists("start_windowed", c == 1);
         if (g_host.set_fullscreen) g_host.set_fullscreen(c == 0);
       },
       false},
      {"V-Sync", {"Off", "On"},
       [] { return rex::cvar::GetFlagByName("present_vsync") == "true" ? 1 : 0; },
       [](int c) {
         SetFileExists("vsync", c == 1);
         rex::cvar::SetFlagByName("present_vsync", c == 1 ? "true" : "false");
       },
       false},
      {"Frame Rate Cap", {"30", "60", "90", "120", "Unlimited"},
       [] {
         const int cap = sr::g_fps_cap.load(std::memory_order_relaxed);
         for (int i = 0; i < 5; ++i)
           if (kFpsCaps[i] == cap) return i;
         return 1;
       },
       [](int c) { sr::SetFpsCap(kFpsCaps[c]); }, false},
      {"FPS Counter", {"Off", "On"},
       [] { return (g_host.fps_counter_shown && g_host.fps_counter_shown()) ? 1 : 0; },
       [](int c) {
         SetFileExists("show_fps", c == 1);
         if (g_host.set_fps_counter) g_host.set_fps_counter(c == 1);
       },
       false},
      {"Aspect Ratio (restart)", {"4:3", "5:4", "3:2", "16:10", "16:9", "21:9"},
       [] { return ReadAspectChoice(); },
       [](int c) {
         if (FILE* f = std::fopen("aspect.txt", "wb")) {
           std::fprintf(f, "%s\n", kAspects[c].name);
           std::fclose(f);
         }
         std::remove("widescreen_off");
       },
       false},
      // The game's own shadow setting (console variable r_shadows at
      // 0x827D6C90): 2 = shadow maps + CPU-built stencil shadows (the
      // default), 3 = shadow maps only, 1 = stencil only, 0 = none. Measured at
      // the Benchmark spot (GFXAB, 2x res, RTX 4060): 3 = +7 % fps, -6 % GPU
      // time, -0.7 CPU cores of job-thread work (sub_822447B0, the hottest
      // function of the job threads); 0 = +17 % fps, -16 % GPU; 1 is worse
      // than 3 in every way, so the menu doesn't offer it.
      {"Shadows", {"Off", "Low", "High"},
       [] {
         switch (GuestShadowSetting()) {
           case 0: return 0;
           case 3: case 1: return 1;
           default: return 2;
         }
       },
       [](int c) {
         static constexpr uint32_t kValue[] = {0, 3, 2};
         const uint32_t v = kValue[c < 0 || c > 2 ? 2 : c];
         if (FILE* f = std::fopen("shadows.txt", "wb")) {
           std::fprintf(f, "%u\n", v);
           std::fclose(f);
         }
         SetGuestShadowSetting(v);
       },
       false},
      {"Mouse Sensitivity", {"0.25", "0.5", "0.75", "1.0", "1.25", "1.5", "2.0", "2.5", "3.0", "4.0"},
       [] {
         const double s = sr::GetMouseSensitivity();
         int best = 3;
         for (int i = 0; i < 10; ++i)
           if (std::fabs(kSensitivities[i] - s) < std::fabs(kSensitivities[best] - s)) best = i;
         return best;
       },
       [](int c) {
         sr::SetMouseSensitivity(kSensitivities[c]);
         if (FILE* f = std::fopen("mouse_sensitivity.txt", "wb")) {
           std::fprintf(f, "%g\n", kSensitivities[c]);
           std::fclose(f);
         }
       },
       false},
  };
  return rows;
}

std::recursive_mutex g_mutex;
bool g_built = false;        // guest memory set up
// The game's widescreen flag: off below 16:10, as on a 4:3 TV.
bool g_widescreen_off = sr::AspectRatioValue() < 1.5f;

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
uint32_t PutText(uint8_t* base, uint32_t& at, const std::string& text) {
  const uint32_t start = at;
  for (char c : text) {
    Host(base, at)[0] = 0;
    Host(base, at)[1] = uint8_t(c);
    at += 2;
  }
  Host(base, at)[0] = Host(base, at)[1] = 0;
  at += 2;
  return start;
}

bool EnsureMemory(uint8_t* base) {
  if (g_built) return true;
  auto& rows = Rows();
  const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kRowMemory * uint32_t(rows.size()));
  if (!block) return false;
  std::memset(Host(base, block), 0, kRowMemory * rows.size());
  for (size_t r = 0; r < rows.size(); ++r) {
    Row& row = rows[r];
    row.item = block + uint32_t(r) * kRowMemory;
    uint32_t at = row.item + kItemSize;
    row.label_text = PutText(base, at, row.label);
    for (const auto& c : row.choices) row.choice_text.push_back(PutText(base, at, c));
  }
  g_built = true;
  return true;
}

// Fills a selector like the game fills Mini Map View.
void BuildItem(uint8_t* base, Row& row) {
  std::memset(Host(base, row.item), 0, kItemSize);
  W32(base, row.item + 0, kSelectorVtable);
  const uint32_t colour = R32(base, kDefaultChoiceColour);
  for (uint32_t i = 0; i < 64; ++i) {
    W32(base, row.item + 272 + i * 4, 0xFFFFFFFFu);
    W32(base, row.item + 528 + i * 4, colour);
  }
  for (uint32_t i = 0; i < row.choice_text.size(); ++i) W32(base, row.item + 16 + i * 4, row.choice_text[i]);
  W32(base, row.item + 784, uint32_t(row.choice_text.size()));
  int chosen = row.current();
  if (chosen < 0 || chosen >= int(row.choices.size())) chosen = 0;
  W32(base, row.item + 788, uint32_t(chosen));
  Host(base, row.item + 792)[0] = 1;
  row.applied = chosen;
}

void ApplyRow(uint8_t* base, Row& row) {
  const int chosen = int(R32(base, row.item + 788));
  if (chosen < 0 || chosen >= int(row.choices.size())) return;
  row.apply(chosen);
  row.applied = chosen;
  REXLOG_INFO("Options: {} = {}", row.label, row.choices[chosen]);
}

}  // namespace

void sr::ApplyStartupGraphics(bool weak_gpu) {
  // shadows.txt (r_shadows value 0-3) from the options menu; without it High
  // (2), or Low (3: shadow maps without the CPU stencil shadows) on weak GPUs.
  int shadows = weak_gpu ? 3 : 2;
  bool from_file = false;
  if (FILE* f = std::fopen("shadows.txt", "rb")) {
    int v = -1;
    if (std::fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 3) {
      shadows = v;
      from_file = true;
    }
    std::fclose(f);
  }
  SetGuestShadowSetting(uint32_t(shadows));
  REXLOG_INFO("Shadows: {} ({})",
              shadows == 2 ? "High" : shadows == 3 ? "Low (shadow maps only)" : shadows == 1 ? "stencil only" : "Off",
              from_file ? "shadows.txt" : weak_gpu ? "default for weak GPUs" : "default");
}

int sr::AspectFrameWidth() {
  if (g_frame_running >= 0) return kFrames[g_frame_running].w;
  if (g_aspect_running == kAspectDefault) return 0;
  return int(std::lround(720.0 * AspectRatioOf(g_aspect_running) / 16.0)) * 16;
}
float sr::AspectRatioValue() { return g_frame_running >= 0 ? 4.0f / 3.0f : AspectRatioOf(g_aspect_running); }
int sr::FrameHeight() { return g_frame_running >= 0 ? kFrames[g_frame_running].h : 720; }

void sr::SetDefaultResScale(int scale) {
  if (scale >= 1 && scale <= 3) g_running_scale = scale;
}

void sr::SetOptionsHost(OptionsHost host) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_host = std::move(host);
}

// Row adder: after the game's Mini Map View row, add ours.
extern "C" void __imp__sub_8228BAB0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8228BAB0) {
  const uint32_t item = ctx.r4.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_8228BAB0(ctx, base);
  {
    // Diagnostic: which lists get rows (the pause menu's Display page had none).
    static int lines = 0;
    if ((item == kMiniMapItem || item == 0x8282AE04u) && lines++ < 40)
      REXLOG_INFO("Options: row adder item {:08X} from {:08X}", item, caller);
  }
  if (item != kMiniMapItem) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!EnsureMemory(base)) return;
  // The list opens with Brightness highlighted (its row is added with r5 = 1,
  // which sets menu +62), but the screen's cursor [0x8370EB28] is never reset:
  // start both on row 0.
  W32(base, 0x8370EB28u, 0);
  for (Row& row : Rows()) {
    BuildItem(base, row);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = row.label_text;
    frame.ctx.r4.u64 = row.item;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r7.u64 = 1;
    frame.ctx.r8.u64 = 0;
    __imp__sub_8228BAB0(frame.ctx, base);
  }
  static int added = 0;
  if (added++ < 40) REXLOG_INFO("Options: {} PC rows added to the Display list", Rows().size());
}

// Diagnostic: the Display list builder (front end and pause menu).
extern "C" void __imp__sub_82347478(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82347478) {
  static int lines = 0;
  if (lines++ < 40) REXLOG_INFO("Options: Display list built (called from {:08X})", uint32_t(ctx.lr));
  __imp__sub_82347478(ctx, base);
}

void sr::OptionsMenuPoll(uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  // The window shows the frame at the chosen shape (once).
  static bool presenter_set = false;
  if (!presenter_set) {
    presenter_set = true;
    const bool own = g_aspect_running == kAspectDefault && g_frame_running < 0;
    const bool four_three = g_frame_running >= 0;
    rex::cvar::SetFlagByName("present_aspect_x",
                             std::to_string(own ? 0 : four_three ? 4 : kAspects[g_aspect_running].x));
    rex::cvar::SetFlagByName("present_aspect_y",
                             std::to_string(own ? 0 : four_three ? 3 : kAspects[g_aspect_running].y));
  }
  // Widescreen: the game sets the flag once at start-up from the video mode
  // (1 for 16:9). Aspect ratios below 16:10 force 0.
  static int game_value = -1;  // what the game had set before we forced it
  uint8_t& flag = *Host(base, kWidescreenFlag);
  if (g_widescreen_off) {
    if (flag != 0) {
      game_value = flag;
      flag = 0;
    }
  } else if (game_value >= 0) {
    flag = uint8_t(game_value);
    game_value = -1;
  }
  if (!g_built) return;
  for (Row& row : Rows()) {
    if (row.confirm) continue;
    const int chosen = int(R32(base, row.item + 788));
    if (chosen != row.applied) ApplyRow(base, row);
  }
}

// ---------------------------------------------------------------------------
// The Display screen's input (sub_82347BD0) is written for exactly 5 rows: its
// cursor [0x8370EB28] wraps at 5, left / right are handled for rows 0-3 by a
// jump table, and row 4 is Restore to Defaults (A). Our rows are 4 .. 3 + N
// and Restore is 4 + N: the cursor is translated around the original call; on
// our rows left / right go to the selector's own routines (the ones Mini Map
// View uses) and A to rows that apply on confirm. The visual list (menu +62
// row, +58 count) already has the extra rows.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kDisplayCursor = 0x8370EB28u;  // lis 0x8371, -5336
constexpr uint32_t kMenuPointer = 0x8370DDC4u;    // lis 0x8371, -8764: current list menu
constexpr uint32_t kInputLeft = 39, kInputRight = 40;

bool GuestBool(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t arg) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = arg;
  fn(frame.ctx, base);
  return (frame.ctx.r3.u32 & 0xFF) != 0;
}
}  // namespace

extern "C" void __imp__sub_82347BD0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82347248(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216ED48(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82288750(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216FCA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82292CC8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82292D68(PPCContext& ctx, uint8_t* base);

PPC_FUNC(sub_82347BD0) {
  if (!g_built) {
    __imp__sub_82347BD0(ctx, base);
    return;
  }
  const int n_rows = int(Rows().size());
  const int restore = 4 + n_rows;
  const int cur = int(R32(base, kDisplayCursor));
  if (cur >= 4 && cur < restore) {
    Row& row = Rows()[cur - 4];
    if (GuestBool(ctx, base, __imp__sub_82288750, 0)) {
      const bool left = GuestBool(ctx, base, __imp__sub_8216ED48, kInputLeft);
      const bool right = !left && GuestBool(ctx, base, __imp__sub_8216ED48, kInputRight);
      if (left || right) {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = row.item;
        frame.ctx.r4.u64 = 1;
        if (left) __imp__sub_82292CC8(frame.ctx, base);
        else __imp__sub_82292D68(frame.ctx, base);
      } else if (row.confirm && GuestBool(ctx, base, __imp__sub_8216FCA0, 0)) {
        // A / Enter (the check Restore to Defaults uses): apply.
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        ApplyRow(base, row);
      }
    }
    // The rest (up / down, back) by the game with a cursor it ignores
    // otherwise: 9 - 1 = 8 means up, (9 + 1) % 5 = 0 means down.
    W32(base, kDisplayCursor, 9);
    __imp__sub_82347BD0(ctx, base);
    const int n = int(R32(base, kDisplayCursor));
    W32(base, kDisplayCursor, uint32_t(n == 8 ? cur - 1 : n == 0 ? cur + 1 : cur));
    return;
  }
  if (cur == restore) {
    // Restore to Defaults: the game's row 4.
    W32(base, kDisplayCursor, 4);
    __imp__sub_82347BD0(ctx, base);
    const int n = int(R32(base, kDisplayCursor));
    W32(base, kDisplayCursor, uint32_t(n == 3 ? restore - 1 : n == 4 ? restore : n));
    return;
  }
  __imp__sub_82347BD0(ctx, base);
  const int n = int(R32(base, kDisplayCursor));
  if (cur == 0 && n == 4) W32(base, kDisplayCursor, uint32_t(restore));  // up from the top wraps to Restore
}

// Help text for Restore to Defaults: the game shows it on visual row 4.
PPC_FUNC(sub_82347248) {
  const uint32_t menu = g_built ? R32(base, kMenuPointer) : 0;
  if (!menu) {
    __imp__sub_82347248(ctx, base);
    return;
  }
  const int restore = 4 + int(Rows().size());
  uint8_t* row = Host(base, menu + 62);
  const uint16_t r = uint16_t(row[0] << 8 | row[1]);
  const uint16_t pretend = r == restore ? 4 : (r >= 4 && r < restore) ? 3 : r;
  row[0] = uint8_t(pretend >> 8);
  row[1] = uint8_t(pretend);
  __imp__sub_82347248(ctx, base);
  row[0] = uint8_t(r >> 8);
  row[1] = uint8_t(r);
}

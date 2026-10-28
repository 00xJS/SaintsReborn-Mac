// The MP3 player (the phone's music player, your playlists), played with
// Windows MCI.
//
// The runtime's XMP player is a stub: the game's playlist calls succeed and it
// reports "playing", but nothing is decoded ("Playlist playback not supported"
// in the log), and it never reports that a song ended, so the game never moves
// on. The game's own music logic is kept: for every song it builds a one-song
// playlist (sub_826595E0: r4 = songs, 36 bytes each, +0 = UTF-16 path such as
// GAME:\data\audio\MP3_Player\NAME.wma; r5 = how many) and plays it
// (sub_82659248); pause / resume go through sub_826591C8 (r4 = message 5 / 3),
// stop through sub_826593C8, the volume through sub_826599F0 (f1). Those calls
// are mirrored here on an MCI device (Windows plays the game's WMA songs
// itself). When a song reaches its end, the game's own next track
// (sub_82217118, on the MP3 player's state at 0x827E69B8, +28 = 1 playing,
// 2 paused) is run every frame after the player controls (sub_8216A038, the
// hook in kbm.cpp calls sr::MusicUpdate), so playlists, repeat and shuffle
// stay the game's.
//
// From SR Tweak Menu 2.6.6's music fix (itsMJQ), moved into the game. Turned
// off by itself if the runtime ever plays playlists on its own.

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <digitalv.h>
#endif

extern "C" void __imp__sub_826595E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82659248(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826593C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826591C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_826599F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82217118(PPCContext& ctx, uint8_t* base);

#ifdef _WIN32
namespace {

constexpr uint32_t kPlayerState = 0x827E69B8u;  // the MP3 player (+28 state, +20 song index)
constexpr uint32_t kMp3State = kPlayerState + 28;
constexpr uint32_t kMusicVolume = 0x827D94A8u;  // the pause menu's music volume (float)
constexpr uint32_t kMenuA = 0x839E0DF8u, kMenuB = 0x839E0FB0u;

std::recursive_mutex g_mutex;
int g_active = -1;           // -1 not checked yet, 0 off (the runtime plays music), 1 on
std::wstring g_next_song;    // the song of the playlist made last
std::wstring g_song;         // the song playing
MCIDEVICEID g_device = 0;
int g_state = 0;             // 0 stopped, 1 playing, 2 paused
float g_volume = -1.0f;
bool g_ended = false;        // a song reached its end: the next one is due
unsigned g_poll = 0;

bool ImageHas(HMODULE module, const char* needle) {
  if (!module) return false;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(module) + dos->e_lfanew);
  const auto* p = reinterpret_cast<const uint8_t*>(module);
  const size_t size = nt->OptionalHeader.SizeOfImage, n = std::strlen(needle);
  for (size_t i = 0; i + n <= size; ++i) {
    // Only committed, readable pages: walk the image region by region.
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p + i, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      i = size_t(reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - p) - 1;
      continue;
    }
    const size_t end = std::min(size, size_t(reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - p));
    for (; i + n <= end; ++i)
      if (p[i] == uint8_t(needle[0]) && std::memcmp(p + i, needle, n) == 0) return true;
    i = end - 1;
  }
  return false;
}

// On while the runtime's XMP player is the stub (its warning text is in
// rexruntime.dll).
bool Active() {
  if (g_active < 0) {
    g_active = ImageHas(GetModuleHandleW(L"rexruntime.dll"), "Playlist playback not supported") ? 1 : 0;
    REXLOG_INFO("Music: {}", g_active ? "MP3 player on Windows MCI (the runtime's player is a stub)"
                                      : "the runtime plays music itself; the MCI player stays off");
  }
  return g_active == 1;
}

std::wstring GameFolder() {
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe, n);
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) dir.resize(slash);
  return dir + L"\\game\\";
}

// A guest UTF-16 (big-endian) path GAME:\data\... as a file in the game folder.
std::wstring HostPath(uint8_t* base, uint32_t guest) {
  if (guest < 0x10000u || guest >= 0xC0000000u) return {};
  std::wstring text;
  for (uint32_t i = 0; i < 512; ++i) {
    const uint16_t c = PPC_LOAD_U16(guest + i * 2);
    if (!c) break;
    text.push_back(wchar_t(c));
  }
  std::wstring lower = text;
  for (auto& c : lower) c = towlower(c);
  if (lower.rfind(L"game:\\", 0) == 0) text = text.substr(6);
  else if (lower.rfind(L"d:\\", 0) == 0) text = text.substr(3);
  if (text.empty() || text.find(L"..") != std::wstring::npos || text.find(L':') != std::wstring::npos) return {};
  for (auto& c : text)
    if (c == L'/') c = L'\\';
  return GameFolder() + text;
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
  return s;
}

void ApplyVolume() {
  if (!g_device || g_volume < 0.0f) return;
  MCI_DGV_SETAUDIO_PARMSW p = {};
  p.dwItem = MCI_DGV_SETAUDIO_VOLUME;
  p.dwValue = DWORD(g_volume * 1000.0f);
  mciSendCommandW(g_device, MCI_SETAUDIO, MCI_DGV_SETAUDIO_ITEM | MCI_DGV_SETAUDIO_VALUE, DWORD_PTR(&p));
}

void Close() {
  if (g_device) mciSendCommandW(g_device, MCI_CLOSE, MCI_WAIT, 0);
  g_device = 0;
  g_state = 0;
}

void Play(uint8_t* base, const std::wstring& path) {
  Close();
  g_ended = false;
  if (path.empty()) return;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    REXLOG_WARN("Music: song not found: {}", Narrow(path));
    return;
  }
  MCI_OPEN_PARMSW open = {};
  open.lpstrDeviceType = L"mpegvideo";
  open.lpstrElementName = path.c_str();
  MCIERROR e = mciSendCommandW(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_ELEMENT | MCI_WAIT, DWORD_PTR(&open));
  if (e) {
    REXLOG_WARN("Music: can't open {} (MCI {})", Narrow(path), uint32_t(e));
    return;
  }
  g_device = open.wDeviceID;
  if (g_volume < 0.0f) {
    uint32_t bits = PPC_LOAD_U32(kMusicVolume);
    float v;
    std::memcpy(&v, &bits, 4);
    if (v >= 0.0f && v <= 1.0f) g_volume = v;
  }
  ApplyVolume();
  MCI_PLAY_PARMS run = {};
  e = mciSendCommandW(g_device, MCI_PLAY, 0, DWORD_PTR(&run));
  if (e) {
    REXLOG_WARN("Music: can't play {} (MCI {})", Narrow(path), uint32_t(e));
    Close();
    return;
  }
  g_song = path;
  g_state = 1;
  REXLOG_INFO("Music: playing {}", Narrow(path));
}

}  // namespace

// A playlist (the game makes one per song): remember its first song.
PPC_FUNC(sub_826595E0) {
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t songs = ctx.r4.u32, count = ctx.r5.u32;
    if (Active() && songs >= 0x10000u && songs < 0xC0000000u && count > 0 && count < 4096)
      g_next_song = HostPath(base, PPC_LOAD_U32(songs));
  }
  __imp__sub_826595E0(ctx, base);
}

// Play a playlist.
PPC_FUNC(sub_82659248) {
  __imp__sub_82659248(ctx, base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (Active() && !g_next_song.empty()) Play(base, g_next_song);
}

// Stop.
PPC_FUNC(sub_826593C8) {
  __imp__sub_826593C8(ctx, base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (Active()) {
    Close();
    g_ended = false;
  }
}

// Player messages: 3 continue, 5 pause (6 next / 7 previous make new playlists).
PPC_FUNC(sub_826591C8) {
  const uint32_t message = ctx.r4.u32 & 0xFFFF;
  __imp__sub_826591C8(ctx, base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!Active() || !g_device) return;
  if (message == 5 && g_state == 1) {
    MCI_GENERIC_PARMS p = {};
    if (!mciSendCommandW(g_device, MCI_PAUSE, 0, DWORD_PTR(&p))) g_state = 2;
  } else if (message == 3 && g_state == 2) {
    MCI_PLAY_PARMS p = {};
    if (!mciSendCommandW(g_device, MCI_RESUME, 0, DWORD_PTR(&p))) g_state = 1;
  }
}

// Music volume (0..1). The XMP player keeps its remembered volume when told
// exactly 1 (a title's "no attenuation" default), so 1 goes out as 0.999.
PPC_FUNC(sub_826599F0) {
  if (ctx.f1.f64 >= 0.999) ctx.f1.f64 = 0.999;
  const double v = ctx.f1.f64;
  __imp__sub_826599F0(ctx, base);
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (v >= 0.0 && v <= 1.0) {
    g_volume = float(v);
    ApplyVolume();
  }
}

namespace sr {
// Every frame on the game thread (after the player controls, kbm.cpp): a song
// that reached its end makes the game go on to its next song.
void MusicUpdate(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (g_active != 1) return;
  if (g_device && g_state == 1 && (++g_poll & 15) == 0) {
    MCI_STATUS_PARMS p = {};
    p.dwItem = MCI_STATUS_MODE;
    if (!mciSendCommandW(g_device, MCI_STATUS, MCI_STATUS_ITEM, DWORD_PTR(&p)) && p.dwReturn == MCI_MODE_STOP) {
      Close();
      g_ended = true;
    }
  }
  // Not while a menu is up (the phone's own screens change the playlist).
  if (!g_ended || PPC_LOAD_U32(kMenuA) != 0 || PPC_LOAD_U32(kMenuB) != 0) return;
  g_ended = false;
  REXLOG_INFO("Music: {} ended; next song", Narrow(g_song));
  // The game's next track refuses a stopped player; keep it "playing" while
  // it picks and starts the next song.
  const uint32_t state = PPC_LOAD_U32(kMp3State);
  if (state == 0) PPC_STORE_U32(kMp3State, 1);
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = kPlayerState;
  frame.ctx.r4.u64 = 0;
  sub_82217118(frame.ctx, base);
  if (frame.ctx.r3.u32 == 0 && state == 0) PPC_STORE_U32(kMp3State, 0);
}
}  // namespace sr
#endif

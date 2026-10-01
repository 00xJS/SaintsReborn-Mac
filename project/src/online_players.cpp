// MULTIPLAYER > PLAYERS: how many people play Saints Reborn online right now.
//
// The MULTIPLAYER screen (menu id 6, sub_82330E48) adds its tabs with
// sub_822903E8(name, selected, menu id): QUICK MATCH 7, CUSTOM MATCH 8,
// CREATE PARTY 9, GANGS 13, CUSTOMIZATION 10, STATS 11, LEADERBOARDS 12,
// OPTIONS 21, HELP 30. PLAYERS is added after LEADERBOARDS as menu id 47 (an
// unused entry of the menu table 0x827B05F8, 16 bytes each: build, exit,
// update, draw), which gets the OPTIONS tab's functions (sub_823487B8 build,
// sub_82349018 update). While id 47 is built, the OPTIONS list's title is
// ours and, at its finisher (sub_8228CAD8), the first OPTIONS row becomes the
// player count and the others go back to the row pool. A on it is eaten in
// coop_menu.cpp (it would open Controls); LT/RT and B work as on every tab.
//
// The count comes from the runtime (eos_lan.cpp): cvar online_players, first
// line "#ready" / "#connecting" / "#failed" / "#coop" / "#off", then one
// "flags|name|activity" line per player. The cvar online_wanted starts Epic
// when MULTIPLAYER opens; co-op online borrows Epic from the runtime
// (SrEosLend / SrEosReturn) and the list comes back after co-op.
#include "online_players.h"

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void __imp__sub_822903E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_823487B8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228BE98(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8251EBC8(PPCContext& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuCurrent = 0x8370DDACu, kMenuRequested = 0x8370DDB0u;
constexpr uint32_t kMenuPointer = 0x8370DDC4u;  // current list (+0 first row, +58 rows, +62 cursor, +64 selectable)
constexpr uint32_t kMenuTable = 0x827B05F8u;
constexpr uint32_t kOptionsId = 21, kLeaderboardsId = 12, kPlayersId = 47;
constexpr uint32_t kLeaderboardsAdded = 0x82330FA8u;  // return address of the LEADERBOARDS add in sub_82330E48
constexpr uint32_t kTabState = 0x8370E9A0u;           // the MULTIPLAYER tab being shown (menu id)
constexpr uint32_t kOptionsTitle = 0x840BBF1Cu;       // OPTIONS list title (string pointer), init flag +4 bit 0
constexpr uint32_t kSubtitle = 0x82FFE3E0u;           // text under the list title
constexpr uint32_t kTopMode = 0x827D578Cu;            // 4 free roam, 5 mission, 6 multiplayer lobby, 13+ match
constexpr uint32_t kMpMenusFlag = 0x8370E927u;        // byte, set while the MULTIPLAYER screen is up
constexpr int kHeaderRows = 3, kMaxPlayerRows = 16;
constexpr uint32_t kChars = 72;  // per text

std::recursive_mutex g_mutex;
uint32_t g_block = 0;  // guest texts: label, title, subtitle, headers, rows
uint32_t g_label = 0, g_title = 0, g_subtitle = 0, g_header[kHeaderRows] = {}, g_row[kMaxPlayerRows + 1] = {};
uint32_t g_saved_title = 0;
bool g_title_swapped = false;
uint32_t g_saved_subtitle = 0;  // the list subtitle (and its flag byte) before PLAYERS set ours
uint8_t g_saved_subtitle_flag = 0;
bool g_subtitle_swapped = false;
std::string g_built_from;  // the cvar text the list shows
std::chrono::steady_clock::time_point g_built_at{};
int g_reselect = -1;
std::string g_activity;
std::chrono::steady_clock::time_point g_activity_at{};

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t R32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
uint16_t R16(uint8_t* base, uint32_t a) {
  const uint8_t* p = Host(base, a);
  return uint16_t(p[0] << 8 | p[1]);
}
void W16(uint8_t* base, uint32_t a, uint16_t v) {
  uint8_t* p = Host(base, a);
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void Text(uint8_t* base, uint32_t at, const std::string& s) {
  uint32_t n = 0;
  for (char c : s) {
    if (n + 1 >= kChars) break;
    W16(base, at + n * 2, uint16_t(uint8_t(c)));
    ++n;
  }
  W16(base, at + n * 2, 0);
}

bool EnsureTexts(uint8_t* base) {
  if (g_block) return true;
  constexpr uint32_t count = 3 + kHeaderRows + kMaxPlayerRows + 1;
  g_block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(count * kChars * 2);
  if (!g_block) return false;
  uint32_t at = g_block;
  auto next = [&]() { const uint32_t a = at; at += kChars * 2; W16(base, a, 0); return a; };
  g_label = next();
  g_title = next();
  g_subtitle = next();
  for (auto& h : g_header) h = next();
  for (auto& r : g_row) r = next();
  Text(base, g_label, "PLAYERS");
  Text(base, g_title, "PLAYERS ONLINE");
  Text(base, g_subtitle, "Players online in Saints Reborn right now");
  return true;
}

struct Player {
  std::string flags, name, activity;
};
std::vector<Player> Parse(const std::string& text, std::string& state) {
  std::vector<Player> out;
  std::istringstream in(text);
  std::string line;
  state.clear();
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') { state = line.substr(1); continue; }
    const size_t a = line.find('|'), b = a == std::string::npos ? a : line.find('|', a + 1);
    if (b == std::string::npos) continue;
    out.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return out;
}

}  // namespace

void sr::PlayersAfterTabAdded(PPCContext& ctx, uint8_t* base, uint32_t lr, uint32_t id) {
  if (id != kLeaderboardsId || lr != kLeaderboardsAdded) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!EnsureTexts(base)) return;
  for (uint32_t i = 0; i < 4; ++i)
    W32(base, kMenuTable + kPlayersId * 16 + i * 4, R32(base, kMenuTable + kOptionsId * 16 + i * 4));
  rex::cvar::SetFlagByName("online_wanted", "true");
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = g_label;
  frame.ctx.r4.u64 = R32(base, kTabState) == kPlayersId ? 1 : 0;
  frame.ctx.r5.u64 = kPlayersId;
  __imp__sub_822903E8(frame.ctx, base);
}

bool sr::PlayersBuilding(uint8_t* base) { return R32(base, kMenuRequested) == kPlayersId; }
bool sr::PlayersCurrent(uint8_t* base) { return R32(base, kMenuCurrent) == kPlayersId; }

// The OPTIONS build (also PLAYERS): the list title is swapped while PLAYERS
// is built and put back for OPTIONS.
PPC_FUNC(sub_823487B8) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const bool players = R32(base, kMenuRequested) == kPlayersId && EnsureTexts(base);
  if (players) {
    if (!(R32(base, kOptionsTitle + 4) & 1)) {
      // The title isn't set up yet: do what the build does first.
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = 0x8206A800u;  // "MENU_OPTIONS_TITLE"
      frame.ctx.r4.u64 = 0x8205EC14u;
      __imp__sub_8251EBC8(frame.ctx, base);
      W32(base, kOptionsTitle, frame.ctx.r3.u32);
      W32(base, kOptionsTitle + 4, R32(base, kOptionsTitle + 4) | 1);
    }
    if (!g_title_swapped) {
      g_saved_title = R32(base, kOptionsTitle);
      g_title_swapped = true;
    }
    W32(base, kOptionsTitle, g_title);
  } else {
    if (g_title_swapped) {
      W32(base, kOptionsTitle, g_saved_title);
      g_title_swapped = false;
    }
    if (g_subtitle_swapped) {
      W32(base, kSubtitle, g_saved_subtitle);
      Host(base, kSubtitle - 4)[0] = g_saved_subtitle_flag;
      g_subtitle_swapped = false;
    }
  }
  __imp__sub_823487B8(ctx, base);
}

// Row pool of the menu lists (sub_8228BAB0 takes rows from it): a circular
// list, node +0 previous, +4 next, the pointer at kRowPool its head.
constexpr uint32_t kRowPool = 0x82FFB6BCu;
static void FreeRow(uint8_t* base, uint32_t node) {
  const uint32_t head = R32(base, kRowPool);
  if (!head) {
    W32(base, node + 0, node);
    W32(base, node + 4, node);
    W32(base, kRowPool, node);
    return;
  }
  const uint32_t tail = R32(base, head + 0);
  W32(base, node + 4, head);
  W32(base, node + 0, tail);
  W32(base, tail + 4, node);
  W32(base, head + 0, node);
}

static std::string CountKey(const std::string& text) {
  std::string state;
  const size_t n = Parse(text, state).size();
  return state + ":" + std::to_string(n);
}

void sr::PlayersFillList(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t menu = R32(base, kMenuPointer);
  if (!menu || !EnsureTexts(base)) return;
  const std::string text = rex::cvar::GetFlagByName("online_players");
  std::string state;
  const size_t count = Parse(text, state).size();
  static const bool no_epic = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m && GetProcAddress(m, "SrEosStub") != nullptr;
  }();
  if (state.empty() && no_epic) state = "nosdk";
  g_built_from = CountKey(text);
  g_built_at = std::chrono::steady_clock::now();

  std::string line;
  if (state == "ready") line = std::to_string(count) + (count == 1 ? " PLAYER ONLINE" : " PLAYERS ONLINE");
  else if (state == "coop") line = "PAUSED DURING CO-OP ONLINE";
  else if (state == "failed") line = "ONLINE LIST UNAVAILABLE";
  else if (state == "nosdk") line = "OFFLINE - EPIC FILES MISSING";
  else line = "CONNECTING...";

  // The OPTIONS build made its rows (Controls, Display, Audio): the first
  // becomes the count, the others go back to the row pool.
  int rows = int(R16(base, menu + 58));
  const uint32_t first = R32(base, menu + 0);
  if (!first || rows < 1) return;
  while (rows > 1) {
    const uint32_t last = R32(base, first + 0);
    if (last == first) break;
    const uint32_t prev = R32(base, last + 0);
    W32(base, prev + 4, first);
    W32(base, first + 0, prev);
    FreeRow(base, last);
    --rows;
  }
  W16(base, menu + 58, uint16_t(rows));
  Text(base, g_header[0], line);
  W32(base, first + 8, g_header[0]);
  Host(base, menu + 64)[0] = 1;  // highlighted like a normal row; A does nothing here
  W16(base, menu + 62, 0);
  if (!g_subtitle_swapped) {
    g_saved_subtitle = R32(base, kSubtitle);
    g_saved_subtitle_flag = Host(base, kSubtitle - 4)[0];
    g_subtitle_swapped = true;
  }
  W32(base, kSubtitle, g_subtitle);
  Host(base, kSubtitle - 4)[0] = 0;
  REXLOG_INFO("Players tab: {} ({} player(s))", state.empty() ? "no list yet" : state, count);
}

void sr::PlayersAfterFinish(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_reselect = -1;
  if (!R32(base, kMenuPointer)) return;
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = 0;
  __imp__sub_8228BE98(frame.ctx, base);
}

void sr::PlayersUpdate(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  // Menu manager +12: 0 idle, 1/2 fading between menus (see the draw in
  // sub_822FEC20); rebuild only while idle. An LT/RT/B this frame still wins:
  // the OPTIONS update runs after this and requests its own menu.
  if (R32(base, kMenuCurrent + 12) != 0) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_built_at < std::chrono::seconds(2)) return;
  if (CountKey(rex::cvar::GetFlagByName("online_players")) == g_built_from) return;  // only the count shows
  const uint32_t menu = R32(base, kMenuPointer);
  g_reselect = menu ? int(R16(base, menu + 62)) : -1;
  g_built_at = now;
  W32(base, kMenuRequested, kPlayersId);  // build the list again
}

// Lobby invites (runtime eos_lan.cpp / xlivebase_app.cpp): the game's
// XInviteSend wrapper (sub_8265BBC0: user, count, XUIDs, text, XOVERLAPPED)
// hands the invitees to the runtime, which delivers them over Epic.
extern "C" void __imp__sub_8265BBC0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8265BBC0) {
  using Send = uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t);
  static const Send send = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Send>(GetProcAddress(m, "SrLiveInviteSend")) : nullptr;
  }();
  if (!send) {
    __imp__sub_8265BBC0(ctx, base);
    return;
  }
  ctx.r3.u64 = send(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32);
}

// Xbox Live storage (the multiplayer character, XSTORAGE:/3/MPStorage, 27,724
// bytes from 0x83822500): the game's XStorageUploadFromMemory (sub_8265C6E0:
// user, server path, size, buffer, XOVERLAPPED) and XStorageDownloadToMemory
// (sub_8265C400: user, server path, size, buffer, results size, results,
// XOVERLAPPED) wrappers talk to Live's servers. The runtime keeps the files
// next to the profile settings instead (SrStorageUpload / SrStorageDownload,
// xlivebase_app.cpp), so the character is still there after a restart.
extern "C" void __imp__sub_8265C6E0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8265C6E0) {
  using Upload = uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  static const Upload upload = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Upload>(GetProcAddress(m, "SrStorageUpload")) : nullptr;
  }();
  if (!upload) {
    __imp__sub_8265C6E0(ctx, base);
    return;
  }
  ctx.r3.u64 = upload(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

extern "C" void __imp__sub_8265C400(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8265C400) {
  using Download = uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  static const Download download = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Download>(GetProcAddress(m, "SrStorageDownload")) : nullptr;
  }();
  if (!download) {
    __imp__sub_8265C400(ctx, base);
    return;
  }
  ctx.r3.u64 = download(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

// Map check. sub_82373788(mode, id) finds a multiplayer map record by id
// (0 = no such map here). Called from sub_82359950 (return 0x82359AA0) when
// the host's lobby settings arrive: then the runtime compares the host's map
// pack with this game's (SrMapCheck, eos_lan.cpp). Missing or another version:
// the settings are ignored (as the game does for an unknown id) and the
// runtime stops listening to that host, so the game drops back to the menu
// instead of starting a match on a map it doesn't have.
extern "C" void __imp__sub_82373788(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82373788) {
  constexpr uint32_t kHostSettingsReturn = 0x82359AA0u;
  const uint32_t lr = uint32_t(ctx.lr);
  const int16_t id = int16_t(ctx.r4.u32 & 0xFFFF);
  __imp__sub_82373788(ctx, base);
  if (lr != kHostSettingsReturn || id == -1) return;
  using Check = int (*)(uint32_t, int);
  static const Check check = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Check>(GetProcAddress(m, "SrMapCheck")) : nullptr;
  }();
  const bool found = ctx.r3.u32 != 0;
  if (check && !check(uint32_t(uint16_t(id)), found ? 1 : 0)) {
    REXLOG_WARN("Map check: host's map id {} not usable here ({}), settings ignored", int(id),
                found ? "different version" : "missing");
    ctx.r3.u64 = 0;
  }
}

// Invite join (sub_8236CBD0, every frame while the invite join waits): after
// the QoS probe to the host (XNQOS* at 0x83068624) it starts the join only when
// bytes 0x8370F203 / 0x8370F20E / 0x8370F1FE are 0 and 0x8370EB7E (party state)
// or 0x8370EB94 (the Marketplace offer check has finished) is set. From the main
// menu that check never runs on this PC, so the join waited forever
// ("connecting to invite"). Logs the gates once a second; when everything else
// is ready and only that flag is missing, it is set.
extern "C" void __imp__sub_8236CBD0(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_8236CBD0) {
  const uint32_t qos = R32(base, 0x83068624u);
  if (qos) {
    static std::chrono::steady_clock::time_point last{};
    static bool forced_logged = false;
    const uint8_t f1ff = Host(base, 0x8370F1FFu)[0], f203 = Host(base, 0x8370F203u)[0],
                  f20e = Host(base, 0x8370F20Eu)[0], f1fe = Host(base, 0x8370F1FEu)[0],
                  eb7e = Host(base, 0x8370EB7Eu)[0], eb94 = Host(base, 0x8370EB94u)[0];
    const uint32_t pending = R32(base, qos + 4);
    const uint16_t data_len = R16(base, qos + 8 + 6);
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
      last = now;
      REXLOG_INFO("Invite join gates: F1FF {} F203 {} F20E {} F1FE {} EB7E {} EB94 {} | qos pending {} data {}", f1ff,
                  f203, f20e, f1fe, eb7e, eb94, pending, data_len);
    }
    if (!eb7e && !eb94 && !pending && data_len && !f203 && !f20e && !f1fe && f1ff != 1) {
      Host(base, 0x8370EB94u)[0] = 1;
      if (!forced_logged) {
        forced_logged = true;
        REXLOG_INFO("Invite join: Marketplace check flag set so the join can start");
      }
    }
  }
  __imp__sub_8236CBD0(ctx, base);
}

// F10 accepts a waiting invite (cvar online_invite_from set by the runtime).
static void InviteKeyPoll() {
  static bool was_down = false;
  const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool pressed = down && !was_down;
  was_down = down;
  if (!pressed || rex::cvar::GetFlagByName("online_invite_from").empty()) return;
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid != GetCurrentProcessId()) return;
  REXLOG_INFO("Invite: F10 pressed, joining");
  rex::cvar::SetFlagByName("online_invite_accept", "true");
}

void sr::PlayersActivityPoll(uint8_t* base) {
  InviteKeyPoll();
  const auto now = std::chrono::steady_clock::now();
  if (now - g_activity_at < std::chrono::seconds(1)) return;
  g_activity_at = now;
  const uint32_t mode = R32(base, kTopMode);
  std::string a;
  if (mode == 6) a = "In a multiplayer lobby";
  else if (mode >= 13 && mode <= 31) a = "In a multiplayer match";
  else if (mode == 4) a = "Playing the story";
  else if (mode == 5) a = "Playing a mission";
  else if (Host(base, kMpMenusFlag)[0]) a = "In the Multiplayer menus";
  else a = "In the menus";
  if (HMODULE m = GetModuleHandleW(L"WhompaysCoop.dll")) {
    if (auto f = reinterpret_cast<int (*)()>(GetProcAddress(m, "WhompaysCoopState")))
      if (f() & 3) a = "Playing co-op";
  }
  if (a != g_activity) {
    g_activity = a;
    rex::cvar::SetFlagByName("online_activity", a);
  }
}

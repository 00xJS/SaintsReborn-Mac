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
// MULTIPLAYER > LOBBIES (menu id 46, the last unused entry of the menu table,
// also with the OPTIONS tab's functions) comes right after CUSTOM MATCH: every
// public Player / Ranked lobby an unmodded game hosts (runtime export
// SrPublicLobbies, xgi_live.inc), one row each ("host - mode - players"). A on
// a row joins it like an accepted invite (SrJoinLobby). Private Parties and
// modded games never show (fair play).
//
// The count comes from the runtime (eos_lan.cpp): cvar online_players, first
// line "#ready" / "#connecting" / "#failed" / "#coop" / "#off", then one
// "flags|name|activity" line per player. The cvar online_wanted starts Epic
// when MULTIPLAYER opens; co-op online borrows Epic from the runtime
// (SrEosLend / SrEosReturn) and the list comes back after co-op.
#include "online_players.h"

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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
extern "C" void __imp__sub_8228BAB0(PPCContext& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuCurrent = 0x8370DDACu, kMenuRequested = 0x8370DDB0u;
constexpr uint32_t kMenuPointer = 0x8370DDC4u;  // current list (+0 first row, +58 rows, +62 cursor, +64 selectable)
constexpr uint32_t kMenuTable = 0x827B05F8u;
constexpr uint32_t kOptionsId = 21, kLeaderboardsId = 12, kPlayersId = 47;
constexpr uint32_t kCustomMatchId = 8, kLobbiesId = 46;
constexpr uint32_t kCustomMatchAdded = 0x82330F1Cu;   // return address of the CUSTOM MATCH add in sub_82330E48
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

// ---- LOBBIES ----
constexpr int kMaxLobbyRows = 16;
uint32_t g_lob_block = 0, g_lob_label = 0, g_lob_title = 0, g_lob_subtitle = 0, g_lob_row[kMaxLobbyRows] = {};
struct Lobby {
  std::string hex, name;
  bool ranked = false, in_match = false, searching = false;
  int mode = -1, players = 1, max_players = 12;
};
std::vector<Lobby> g_lobbies;     // the rows shown, in order
std::string g_lob_built_key;      // what the list shows
std::chrono::steady_clock::time_point g_lob_built_at{};
std::string g_lob_reselect_hex;   // keep the cursor on this lobby after a rebuild
int g_lob_reselect = 0;

bool EnsureLobbyTexts(uint8_t* base) {
  if (g_lob_block) return true;
  constexpr uint32_t count = 3 + kMaxLobbyRows;
  g_lob_block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(count * kChars * 2);
  if (!g_lob_block) return false;
  uint32_t at = g_lob_block;
  auto next = [&]() { const uint32_t a = at; at += kChars * 2; W16(base, a, 0); return a; };
  g_lob_label = next();
  g_lob_title = next();
  g_lob_subtitle = next();
  for (auto& r : g_lob_row) r = next();
  Text(base, g_lob_label, "LOBBIES");
  Text(base, g_lob_title, "PUBLIC LOBBIES");
  return true;
}

std::vector<Lobby> ReadLobbies(std::string& state) {
  std::vector<Lobby> out;
  state.clear();
  using Fn = int (*)(char*, int);
  static const Fn fn = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, "SrPublicLobbies")) : nullptr;
  }();
  if (!fn) {
    state = "off";
    return out;
  }
  std::vector<char> buf(32 * 1024);
  if (fn(buf.data(), int(buf.size())) <= 0) return out;
  std::istringstream in(buf.data());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '#') { state = line.substr(1); continue; }
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
      const size_t b = line.find('|', a);
      f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (f.size() < 7 || f[0].size() != 120) continue;
    Lobby l;
    l.hex = f[0];
    l.name = f[1].empty() ? std::string("Player") : f[1];
    l.ranked = f[2] == "r";
    l.mode = std::atoi(f[3].c_str());
    l.players = std::max(1, std::atoi(f[4].c_str()));
    l.max_players = std::max(l.players, std::atoi(f[5].c_str()));
    l.in_match = f[6] == "1";
    l.searching = f[6] != "0" && f[6] != "1";
    out.push_back(l);
  }
  // Joinable lobbies first (joins only work while the host is in its lobby).
  std::stable_sort(out.begin(), out.end(),
                   [](const Lobby& a, const Lobby& b) { return !(a.in_match || a.searching) && (b.in_match || b.searching); });
  if (out.size() > size_t(kMaxLobbyRows)) out.resize(kMaxLobbyRows);
  return out;
}

std::string LobbyKey(const std::string& state, const std::vector<Lobby>& lobbies) {
  std::string k = state + ";";
  for (const auto& l : lobbies)
    k += l.hex.substr(0, 16) + ":" + std::to_string(l.players) + "/" + std::to_string(l.max_players) +
         (l.in_match ? "m" : l.searching ? "s" : "l") + std::to_string(l.mode) + ";";
  return k;
}

// The game's own mode names (US_Strings MULTI_MODE_13 .. 19), in the order of
// the lobby's Mode list, which is what the X_CONTEXT_GAME_MODE value counts.
std::string ModeName(int mode, bool ranked) {
  static const char* kModes[] = {"Gangsta Brawl",       "Team Gangsta Brawl", "Big Ass Chains", "Team Big Ass Chains",
                                 "Blinged Out Ride",    "Protect tha Pimp",   "Co-op"};
  if (mode >= 0 && mode < 7) return std::string(ranked ? "Ranked " : "") + kModes[mode];
  return ranked ? "Ranked Match" : "Player Match";
}

std::string LobbyRowText(const Lobby& l) {
  std::string t = l.name + "  -  " + ModeName(l.mode, l.ranked) + "  -  " + std::to_string(l.players) + "/" +
                  std::to_string(l.max_players);
  if (l.in_match) t += "  (In Match)";
  else if (l.searching) t += "  (Searching)";
  return t;
}

}  // namespace

// Diagnostic for the tab bar (LB/RB in MULTIPLAYER): every tab added while the
// MULTIPLAYER screen is up, and every change of the shown / requested menu.
static int g_tab_logs = 0;
void sr::PlayersAfterTabAdded(PPCContext& ctx, uint8_t* base, uint32_t lr, uint32_t id) {
  if (Host(base, kMpMenusFlag)[0] && g_tab_logs < 200) {
    ++g_tab_logs;
    REXLOG_INFO("MP TABS: added id {} (from {:08X}), shown {}", id, lr, R32(base, kTabState));
  }
  if (id == kCustomMatchId && lr == kCustomMatchAdded) {
    // LOBBIES right after CUSTOM MATCH.
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!EnsureLobbyTexts(base)) return;
    for (uint32_t i = 0; i < 4; ++i)
      W32(base, kMenuTable + kLobbiesId * 16 + i * 4, R32(base, kMenuTable + kOptionsId * 16 + i * 4));
    rex::cvar::SetFlagByName("online_wanted", "true");
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_lob_label;
    frame.ctx.r4.u64 = R32(base, kTabState) == kLobbiesId ? 1 : 0;
    frame.ctx.r5.u64 = kLobbiesId;
    __imp__sub_822903E8(frame.ctx, base);
    return;
  }
  if (id != kLeaderboardsId || lr != kLeaderboardsAdded) return;
  static const bool tab_off = [] {
    FILE* f = std::fopen("players_tab.off", "rb");
    if (f) std::fclose(f);
    if (f) REXLOG_INFO("MP TABS: PLAYERS tab off (players_tab.off)");
    return f != nullptr;
  }();
  if (tab_off) return;
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

bool sr::PlayersBuilding(uint8_t* base) {
  const uint32_t r = R32(base, kMenuRequested);
  return r == kPlayersId || r == kLobbiesId;
}
bool sr::LobbiesCurrent(uint8_t* base) { return R32(base, kMenuCurrent) == kLobbiesId; }
bool sr::PlayersCurrent(uint8_t* base) { return R32(base, kMenuCurrent) == kPlayersId; }

// The OPTIONS build (also PLAYERS): the list title is swapped while PLAYERS
// is built and put back for OPTIONS.
PPC_FUNC(sub_823487B8) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t requested = R32(base, kMenuRequested);
  const bool lobbies = requested == kLobbiesId && EnsureLobbyTexts(base);
  const bool players = lobbies || (requested == kPlayersId && EnsureTexts(base));
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
    W32(base, kOptionsTitle, lobbies ? g_lob_title : g_title);
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

static void LobbiesFillList(PPCContext& ctx, uint8_t* base);
void sr::PlayersFillList(PPCContext& ctx, uint8_t* base) {
  if (R32(base, kMenuRequested) == kLobbiesId) {
    LobbiesFillList(ctx, base);
    return;
  }
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
  const uint32_t menu = R32(base, kMenuPointer);
  if (!menu) return;
  uint32_t row = 0;
  if (R32(base, kMenuCurrent) == kLobbiesId || R32(base, kMenuRequested) == kLobbiesId) {
    const int rows = int(R16(base, menu + 58));
    row = uint32_t(std::clamp(g_lob_reselect, 0, std::max(0, rows - 1)));
  }
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = row;
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

// LOBBIES: the OPTIONS build made its rows (Controls, Display, Audio); the
// first becomes the first lobby (or the status line), the others go back to
// the row pool and one row per further lobby is added like any list row.
static void LobbiesFillList(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t menu = R32(base, kMenuPointer);
  if (!menu || !EnsureLobbyTexts(base)) return;
  std::string state;
  g_lobbies = ReadLobbies(state);
  g_lob_built_key = LobbyKey(state, g_lobbies);
  g_lob_built_at = std::chrono::steady_clock::now();
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
  std::string status, subtitle;
  if (g_lobbies.empty()) {
    if (state == "ready") status = "NO PUBLIC LOBBIES RIGHT NOW";
    else if (state == "modded") status = "PUBLIC LOBBIES NEED AN UNMODDED GAME";
    else if (state == "off") status = "OFFLINE - EPIC FILES MISSING";
    else status = "CONNECTING...";
    subtitle = state == "modded" ? "Turn off your mods to see and join public lobbies"
                                 : "Open Player Match and Ranked lobbies you can join";
    Text(base, g_lob_row[0], status);
  } else {
    subtitle = std::to_string(g_lobbies.size()) + (g_lobbies.size() == 1 ? " public lobby" : " public lobbies") +
               " - select one to join";
    Text(base, g_lob_row[0], LobbyRowText(g_lobbies[0]));
  }
  W32(base, first + 8, g_lob_row[0]);
  Host(base, menu + 64)[0] = 1;
  for (size_t i = 1; i < g_lobbies.size() && i < size_t(kMaxLobbyRows); ++i) {
    Text(base, g_lob_row[i], LobbyRowText(g_lobbies[i]));
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = g_lob_row[i];
    frame.ctx.r4.u64 = 0;  // plain text row
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r7.u64 = 1;
    frame.ctx.r8.u64 = 0;
    __imp__sub_8228BAB0(frame.ctx, base);
  }
  // Cursor: the lobby it was on before the rebuild.
  g_lob_reselect = 0;
  for (size_t i = 0; i < g_lobbies.size(); ++i)
    if (!g_lob_reselect_hex.empty() && g_lobbies[i].hex == g_lob_reselect_hex) g_lob_reselect = int(i);
  W16(base, menu + 62, uint16_t(g_lob_reselect));
  Text(base, g_lob_subtitle, subtitle);
  if (!g_subtitle_swapped) {
    g_saved_subtitle = R32(base, kSubtitle);
    g_saved_subtitle_flag = Host(base, kSubtitle - 4)[0];
    g_subtitle_swapped = true;
  }
  W32(base, kSubtitle, g_lob_subtitle);
  Host(base, kSubtitle - 4)[0] = 0;
  REXLOG_INFO("Lobbies tab: {} ({} lobby/lobbies)", state.empty() ? "no list yet" : state, g_lobbies.size());
}

void sr::LobbiesUpdate(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (R32(base, kMenuCurrent + 12) != 0) return;  // fading between menus
  const auto now = std::chrono::steady_clock::now();
  if (now - g_lob_built_at < std::chrono::seconds(2)) return;
  g_lob_built_at = now;
  std::string state;
  const auto lobbies = ReadLobbies(state);
  if (LobbyKey(state, lobbies) == g_lob_built_key) return;
  const uint32_t menu = R32(base, kMenuPointer);
  const int cursor = menu ? int(R16(base, menu + 62)) : 0;
  g_lob_reselect_hex = cursor >= 0 && size_t(cursor) < g_lobbies.size() ? g_lobbies[size_t(cursor)].hex : std::string();
  W32(base, kMenuRequested, kLobbiesId);  // build the list again
}

void sr::LobbiesConfirm(uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t menu = R32(base, kMenuPointer);
  if (!menu) return;
  const int cursor = int(R16(base, menu + 62));
  if (cursor < 0 || size_t(cursor) >= g_lobbies.size()) return;  // the status line
  using Fn = int (*)(const char*);
  static const Fn join = [] {
    HMODULE m = GetModuleHandleW(L"rexruntime.dll");
    return m ? reinterpret_cast<Fn>(GetProcAddress(m, "SrJoinLobby")) : nullptr;
  }();
  const Lobby& l = g_lobbies[size_t(cursor)];
  if (l.in_match || l.searching) {
    REXLOG_INFO("Lobbies tab: A on '{}' - {}, not joined", l.name, l.in_match ? "a match is running" : "searching");
    rex::cvar::SetFlagByName("online_notice", l.in_match ? l.name + "'s match has already started. Try again when they're back in the lobby."
                                                         : l.name + " is searching for players. Try again when they're in their lobby.");
    return;
  }
  REXLOG_INFO("Lobbies tab: A on '{}' ({}) -> join", l.name, LobbyRowText(l));
  if (join) join(l.hex.c_str());
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

// The PLAYERS / LOBBIES subtitle (kSubtitle) used to be put back only when
// the OPTIONS tab was built again. Leaving LOBBIES for the tabs next to it
// (and from there Wardrobe, Purchase Clothing, Purchase Tattoos, which don't
// set a subtitle of their own) kept "N public lobbies - select one to join"
// on screen over those menus. Now it goes back as soon as neither tab is
// shown or being built - unless the next menu already wrote its own.
static void RestoreSubtitle(uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_subtitle_swapped) return;
  const uint32_t current = R32(base, kMenuCurrent), requested = R32(base, kMenuRequested);
  if (current == kPlayersId || current == kLobbiesId || requested == kPlayersId || requested == kLobbiesId) return;
  const uint32_t shown = R32(base, kSubtitle);
  if ((g_subtitle && shown == g_subtitle) || (g_lob_subtitle && shown == g_lob_subtitle)) {
    W32(base, kSubtitle, g_saved_subtitle);
    Host(base, kSubtitle - 4)[0] = g_saved_subtitle_flag;
  }
  g_subtitle_swapped = false;
  REXLOG_INFO("MP TABS: list subtitle put back (menu {})", current);
}

void sr::PlayersActivityPoll(uint8_t* base) {
  InviteKeyPoll();
  RestoreSubtitle(base);
  {
    static uint32_t last[6] = {};
    static int logs = 0;
    const uint32_t menu = R32(base, kMenuPointer);
    const uint32_t now_state[6] = {R32(base, kMenuCurrent), R32(base, kMenuRequested), R32(base, kTabState), menu,
                                   menu ? R16(base, menu + 58) : 0u, uint32_t(Host(base, kMpMenusFlag)[0])};
    if (std::memcmp(now_state, last, sizeof(last)) != 0 && (now_state[5] || last[5]) && logs < 400) {
      ++logs;
      std::memcpy(last, now_state, sizeof(last));
      REXLOG_INFO("MP MENU: current {} requested {} tab {} list {:08X} rows {} mp {}", now_state[0], now_state[1],
                  now_state[2], now_state[3], now_state[4], now_state[5]);
    } else {
      std::memcpy(last, now_state, sizeof(last));
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_activity_at < std::chrono::seconds(1)) return;
  g_activity_at = now;
  const uint32_t mode = R32(base, kTopMode);
  std::string a;
  // Matchmaking (Player Match / Ranked search, or hosting while it waits for
  // players: bytes 0x8370EB4F +1606 host loop, +1608 searching, as in
  // coop_menu.cpp SoloStartPoll): the matchmaking screen runs as top mode 4.
  const bool matchmaking = Host(base, 0x8370EB4Fu + 1606)[0] || Host(base, 0x8370EB4Fu + 1608)[0];
  if (mode == 6) a = "In a multiplayer lobby";
  else if (matchmaking && !(mode >= 13 && mode <= 31)) a = "Searching for players";
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

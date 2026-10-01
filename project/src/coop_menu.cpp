// Co-op in the game's own menus.
//
// Story pause menu, OPTIONS tab (built by sub_822B7328: Controls, Display,
// Audio, Quit Game; its update sub_822B7D68 acts on A for rows 0-3 through a
// jump table and moves the cursor with the generic list code): rows are added
// after Quit Game - "Host Co-op" and "Join Co-op", or "End Co-op" / "Leave
// Co-op" during a session. Rows past 3 are ignored by the game's A handler, so
// ours are acted on here.
//
// Main menu (built by sub_822852B0, 7 rows; its update sub_822861F0 keeps the
// cursor at menu object +8 and dispatches A on it): "Join Co-op" is added as
// the last row. It asks for the host's join code or IP, then opens the load
// screen the way LOAD GAME does (0x827AF780 = 0, 0x8300FD3C/40 = 4, front-end
// state 0x8370DDB0 = 44, see kbm.cpp); once the picked save has loaded, the
// co-op mod joins.
//
// Both lists are finished with sub_8228CAD8; our rows are added right before
// that (recognised by the return address), with the generic row adder
// sub_8228BAB0 like the PC rows in options_menu.cpp.

#include "coop_menu.h"
#include "online_players.h"

#include <sr_reserved_names.h>

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
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

namespace {

constexpr uint32_t kMenuPointer = 0x8370DDC4u;       // current list menu (+58 rows, +62 cursor)
constexpr uint32_t kPauseOptionsDone = 0x822B7C04u;  // return address of sub_8228CAD8 in sub_822B7328
constexpr uint32_t kMainMenuDone = 0x822861B8u;      // return address of sub_8228CAD8 in sub_822852B0
constexpr uint32_t kPlayer = 0x8309ABECu;
constexpr uint32_t kActiveScreen = 0x839E0DF8u;

enum Action { kHost, kJoin, kStop, kName };
// 0-3: rows of the pause menu's CO-OP tab (mixed case like the game's own
// rows), 4: the tab's name, 5: the main menu row (upper case like its rows).
const char* const kLabels[] = {"Host Co-op", "Join Co-op", "End Co-op", "Leave Co-op", "CO-OP", "JOIN CO-OP"};
constexpr int kLabelCount = 6;
constexpr int kTabLabel = 4, kMainLabel = 5;

// Pause menu CO-OP tab. The pause menu builder sub_822BFA88 adds its tabs
// with sub_822903E8(r3 = name, r4 = selected, r5 = menu id): MAP 26, INFO 32,
// HELP 42, PHONE 29, MP3 27, SAVE/LOAD 44, OPTIONS 43. A menu id indexes the
// table at 0x827B05F8 (16 bytes: build, exit, update, draw); ids 46 and 47
// are unused (no-op functions). The menu manager at 0x8370DDAC (+0 current id,
// +4 requested id) calls exit of the current menu and build of the
// requested one, then update of the current one every frame; the tabs'
// LT/RT switching (sub_822C06B8, called by each tab's update) requests ids.
// Id 46 gets the OPTIONS functions; their overrides below see the id and
// build / run the CO-OP list instead.
constexpr uint32_t kMenuCurrent = 0x8370DDACu, kMenuRequested = 0x8370DDB0u;
constexpr uint32_t kMenuTable = 0x827B05F8u;
constexpr uint32_t kOptionsId = 43, kCoopId = 46;
// Main menu > SYSTEM LINK (id 19, sub_82331070: tabs 20 SYSTEM LINK, 21
// OPTIONS, 30 HELP) and XBOX LIVE (id 6, sub_82330E48: its own tabs plus the
// same 21 OPTIONS and 30 HELP). The OPTIONS tab (id 21, build sub_823487B8,
// update sub_82349018) lists Controls / Display / Audio (A requests menu 22 /
// 24 / 25 for cursor 0 / 1 / 2; other rows only play the select sound); a
// "Player Name: X" row is added after Audio.
constexpr uint32_t kMpOptionsId = 21;
constexpr uint32_t kMpOptionsDone = 0x82348F50u;  // return address of sub_8228CAD8 in sub_823487B8
int g_mp_name_row = -1;                           // our row in that list
int g_mp_add_row = -1;                            // "Add Friend" row (friend code row above it)
std::string g_code_shown;                         // friend code in the row now
bool g_mp_reselect = false;                       // put the cursor back on our row after a rebuild
constexpr uint32_t kOptionsTabAdded = 0x822BFCB0u;  // return address of the OPTIONS tab add in sub_822BFA88
constexpr uint32_t kMultiplayerFlag = 0x8370E9F6u;   // byte, nonzero in multiplayer (other tab set)

std::recursive_mutex g_mutex;
uint32_t g_text[kLabelCount] = {};  // guest UTF-16BE label texts
uint32_t g_info_text = 0;       // guest UTF-16BE: join code / IP row (kInfoChars)
uint32_t g_name_text = 0;       // guest UTF-16BE: "Player Name: X" row (kInfoChars)
uint32_t g_code_text = 0;       // guest UTF-16BE: "Friend Code: XXXX-XXXX" row (kInfoChars)
uint32_t g_addfriend_text = 0;  // guest UTF-16BE: "Add Friend" row (kInfoChars)
constexpr uint32_t kInfoChars = 64;
int g_pause_first = -1;         // first of our rows in the pause OPTIONS list
std::vector<Action> g_pause_actions;
// Main menu: our row sits under LOAD GAME (list row 2); the game's own rows
// from XBOX LIVE on move down one. menu +62 (the list cursor) counts our row,
// the menu object's +8 (the game's own row number, used by its A handler and
// when the list is built again) does not: our row is 0xFFFF there.
constexpr uint32_t kMainMenuObject = 0x82816CC0u;  // the main menu screen
constexpr int kMainJoinRow = 2;
int g_main_row = -1;            // our row in the main menu list
uint32_t g_main_object = 0;
uint16_t g_main_cursor_before = 0;

// Main menu join: dialog -> load screen -> join after the save loaded.
bool g_open_load = false;
bool g_join_pending = false;
std::string g_join_code;
std::chrono::steady_clock::time_point g_join_since{}, g_join_ready{};

// The code box is the game's own message dialog (the screen behind the debug
// command message_test, sub_8229ED60): vtable 0x8205FD28, +8 title and +12
// text (UTF-16BE; the text is copied and word-wrapped when it opens,
// sub_8229EF58; button pictures are the font's button characters, 0x7F A and
// 0x81 B, the codes the game turns "<btn A>" / "<btn B>" into (table at
// 0x8205F6C0); glyphs.cpp swaps their pictures for keyboard/mouse), +588/+592 close callback
// (0 / -1), +596 keeps the menus behind it from taking input, +597 shown
// (0 fades it out), +600 fade (float). Pushed with sub_8229B6C8(object, 0).
// Its update sub_8229F288 returns 0 once faded out, which closes the screen.
constexpr uint32_t kMessageDialogVtable = 0x8205FD28u;
constexpr uint32_t kDialogSize = 640, kTitleSize = 64, kTextSize = 512;
std::atomic<bool> g_dialog_open{false};
enum class DialogState { kClosed, kOpen, kClosing, kWaitRelease } g_dialog_state = DialogState::kClosed;
uint32_t g_dialog = 0, g_dialog_title = 0, g_dialog_text = 0;
std::string g_typed;
bool g_typed_changed = false;
bool g_name_changed = false;  // rebuild the CO-OP tab (new name row text)
bool g_key_was_down[256] = {};
enum class DialogFor { kNone, kPause, kMainMenu, kName, kFriend } g_dialog_for = DialogFor::kNone;

// From the pause menu, hosting / joining starts once the game runs again
// (hosting online right away with the pause menu up crashed): the pause
// menu is closed with a short Start press (sent by kbm.cpp), then the action
// runs when no menu is open (or after 4 s regardless).
int g_resume_action = 0;  // 1 host, 2 join
std::string g_resume_code;
std::chrono::steady_clock::time_point g_resume_since{};
bool g_resume_clock = false;

void StartAfterResume(int action, const std::string& code) {
  g_resume_action = action;
  g_resume_code = code;
  g_resume_clock = false;
}

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

// The co-op mod's exports (nullptr when it isn't loaded).
template <class F>
F CoopFunction(const char* name) {
#ifdef _WIN32
  HMODULE m = GetModuleHandleW(L"WhompaysCoop.dll");
  return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
#else
  (void)name;
  return nullptr;
#endif
}
bool CoopLoaded() { return CoopFunction<int (*)()>("WhompaysCoopState") != nullptr; }
int CoopState() {
  auto f = CoopFunction<int (*)()>("WhompaysCoopState");
  return f ? f() : 0;
}
void CoopHost() {
  if (auto f = CoopFunction<void (*)()>("WhompaysCoopHost")) f();
}
void CoopStop() {
  if (auto f = CoopFunction<void (*)()>("WhompaysCoopStop")) f();
}
void CoopJoin(const std::string& code) {
  if (auto f = CoopFunction<void (*)(const char*)>("WhompaysCoopJoin")) f(code.c_str());
}
// The session line for the pause menu ("JOIN CODE: X  (CONNECTED)").
std::string CoopInfo() {
  char buf[128] = {};
  if (auto f = CoopFunction<int (*)(char*, int)>("WhompaysCoopInfo")) f(buf, int(sizeof buf));
  return buf;
}
std::string CoopLastJoin() {
  char buf[128] = {};
  if (auto f = CoopFunction<int (*)(char*, int)>("WhompaysCoopLastJoin")) f(buf, int(sizeof buf));
  std::string s = buf;
  return s == "127.0.0.1" ? std::string() : s;
}

// Player name (player_name.txt next to the exe; main.cpp puts it into the SDK
// cvar xam_player_name at start): the name other players see in co-op, System
// Link and the Xbox Live menus. Up to 15 characters.
std::string PlayerName() {
  std::string n = rex::cvar::GetFlagByName("xam_player_name");
  return n;
}
// Reserved names ("Whompay" and look-alikes, sr_reserved_names.h) only on
// the owner's PC (owner.key next to the exe; other players' games check the
// owner's signature, online_identity.cpp in the runtime).
std::chrono::steady_clock::time_point g_name_refused_until{};
bool NameAllowed(const std::string& name) {
  if (!sr_names::IsReserved(name)) return true;
  FILE* f = std::fopen("owner.key", "rb");
  if (f) std::fclose(f);
  return f != nullptr;
}
std::string NameRowText() {
  if (std::chrono::steady_clock::now() < g_name_refused_until) return "Player Name: that name is taken";
  const std::string name = PlayerName();
  return "Player Name: " + (name.empty() ? std::string("(not set)") : name);
}
void SetPlayerName(const std::string& name) {
  rex::cvar::SetFlagByName("xam_player_name", name);
  if (FILE* f = std::fopen("player_name.txt", "wb")) {
    std::fwrite(name.data(), 1, name.size(), f);
    std::fclose(f);
  }
  REXLOG_INFO("Player name set to '{}'", name);
}

bool EnsureTexts(uint8_t* base) {
  if (g_text[0]) return true;
  const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(256 + kInfoChars * 8);
  if (!block) return false;
  g_info_text = block + 256;
  W16(base, g_info_text, 0);
  g_name_text = g_info_text + kInfoChars * 2;
  W16(base, g_name_text, 0);
  g_code_text = g_name_text + kInfoChars * 2;
  W16(base, g_code_text, 0);
  g_addfriend_text = g_code_text + kInfoChars * 2;
  W16(base, g_addfriend_text, 0);
  uint32_t at = block;
  for (int i = 0; i < kLabelCount; ++i) {
    g_text[i] = at;
    for (const char* c = kLabels[i]; *c; ++c, at += 2) W16(base, at, uint16_t(uint8_t(*c)));
    W16(base, at, 0);
    at += 2;
  }
  return true;
}

bool GuestBool(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t arg) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = arg;
  fn(frame.ctx, base);
  return (frame.ctx.r3.u32 & 0xFF) != 0;
}

void WriteText(uint8_t* base, uint32_t at, const std::string& text, uint32_t size) {
  uint32_t n = 0;
  for (char c : text) {
    if (n + 2 >= size / 2) break;
    W16(base, at + n * 2, uint16_t(uint8_t(c)));
    ++n;
  }
  W16(base, at + n * 2, 0);
}

std::string DialogBody() {
  if (g_dialog_for == DialogFor::kFriend)
    return "Type your friend's friend code\n(they find theirs under MULTIPLAYER > OPTIONS)\n\n" + g_typed +
           "_\n\n\x7F Add     \x81 Cancel";
  if (g_dialog_for == DialogFor::kName)
    return "Type the name other players see\n\n" + g_typed + "_\n\n\x7F Save     \x81 Cancel";
  return "Type the host's join code or IP\n\n" + g_typed + "_\n\n\x7F Join     \x81 Cancel";
}

bool KeyDown(int vk) {
#ifdef _WIN32
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
  (void)vk;
  return false;
#endif
}

bool WindowFocused() {
#ifdef _WIN32
  HWND foreground = GetForegroundWindow();
  DWORD pid = 0;
  if (foreground) GetWindowThreadProcessId(foreground, &pid);
  return pid == GetCurrentProcessId();
#else
  return false;
#endif
}

// New presses since the last call (keys held when the box opened don't count).
bool Pressed(int vk) {
  const bool down = KeyDown(vk);
  const bool was = g_key_was_down[vk & 0xFF];
  g_key_was_down[vk & 0xFF] = down;
  return down && !was;
}

std::string Clipboard() {
  std::string out;
#ifdef _WIN32
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_TEXT)) {
    if (const char* p = static_cast<const char*>(GlobalLock(h))) {
      out = p;
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
#endif
  return out;
}

void AddTyped(const std::string& text) {
  const bool name = g_dialog_for == DialogFor::kName;
  const bool friend_code = g_dialog_for == DialogFor::kFriend;
  for (char c : text) {
    if (friend_code) {
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
      if (((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') && g_typed.size() < 9u) {
        g_typed += c;
        g_typed_changed = true;
      }
      continue;
    }
    if (!name && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const bool ok = name ? ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' ||
                            c == '_' || c == '-' || c == '.')
                         : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-');
    if (ok && g_typed.size() < (name ? 15u : 40u)) {
      g_typed += c;
      g_typed_changed = true;
    }
  }
}

// Keyboard typing into the box: letters, digits, '.', ':', '-', Backspace,
// Ctrl+V. Returns 1 for Enter, -1 for Esc, 0 otherwise.
int PollTyping() {
  if (!WindowFocused()) {
    for (int vk = 0; vk < 256; ++vk) g_key_was_down[vk] = KeyDown(vk);
    return 0;
  }
  const bool ctrl = KeyDown(VK_CONTROL);
  const bool upper = KeyDown(VK_SHIFT) != ((GetKeyState(VK_CAPITAL) & 1) != 0);
  for (int vk = 'A'; vk <= 'Z'; ++vk)
    if (Pressed(vk)) {
      if (ctrl && vk == 'V') AddTyped(Clipboard());
      else if (!ctrl) AddTyped(std::string(1, char(upper ? vk : vk - 'A' + 'a')));
    }
  if (Pressed(VK_SPACE)) AddTyped(" ");
  for (int vk = '0'; vk <= '9'; ++vk)
    if (Pressed(vk) && !ctrl) AddTyped(std::string(1, char(vk)));
  for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
    if (Pressed(vk)) AddTyped(std::string(1, char('0' + vk - VK_NUMPAD0)));
  const bool shift = KeyDown(VK_SHIFT);
  if (Pressed(VK_OEM_PERIOD) || Pressed(VK_DECIMAL)) AddTyped(".");
  if (Pressed(VK_OEM_1) && shift) AddTyped(":");
  if (Pressed(VK_OEM_MINUS) || Pressed(VK_SUBTRACT)) AddTyped(KeyDown(VK_SHIFT) ? "_" : "-");
  if (Pressed(VK_BACK) && !g_typed.empty()) {
    g_typed.pop_back();
    g_typed_changed = true;
  }
  if (Pressed(VK_RETURN)) return 1;
  if (Pressed(VK_ESCAPE)) return -1;
  return 0;
}

void ShowDialog(PPCContext& ctx, uint8_t* base, DialogFor why);

}  // namespace

extern "C" void __imp__sub_8229B6C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8229EF58(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8229F288(PPCContext& ctx, uint8_t* base);

namespace {
void ShowDialog(PPCContext& ctx, uint8_t* base, DialogFor why) {
  if (g_dialog_state != DialogState::kClosed) return;
  if (!g_dialog) {
    const uint32_t block = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kDialogSize + kTitleSize + kTextSize);
    if (!block) return;
    g_dialog = block;
    g_dialog_title = block + kDialogSize;
    g_dialog_text = g_dialog_title + kTitleSize;
  }
  std::memset(Host(base, g_dialog), 0, kDialogSize);
  W32(base, g_dialog + 0, kMessageDialogVtable);
  W32(base, g_dialog + 8, g_dialog_title);
  W32(base, g_dialog + 12, g_dialog_text);
  W32(base, g_dialog + 588, 0);
  W32(base, g_dialog + 592, 0xFFFFFFFFu);
  Host(base, g_dialog + 596)[0] = 1;
  g_dialog_for = why;
  if (why == DialogFor::kName) {
    g_typed = PlayerName().substr(0, 15);
  } else if (why == DialogFor::kFriend) {
    g_typed.clear();
  } else {
    g_typed = CoopLastJoin();
    for (char& c : g_typed)
      if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
  }
  g_typed_changed = false;
  WriteText(base, g_dialog_title,
            why == DialogFor::kName ? "PLAYER NAME" : why == DialogFor::kFriend ? "ADD FRIEND" : "JOIN CO-OP", kTitleSize);
  WriteText(base, g_dialog_text, DialogBody(), kTextSize);
  for (int vk = 0; vk < 256; ++vk) g_key_was_down[vk] = KeyDown(vk);
  g_dialog_for = why;
  g_dialog_state = DialogState::kOpen;
  g_dialog_open.store(true);
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = g_dialog;
  frame.ctx.r4.u64 = 0;
  __imp__sub_8229B6C8(frame.ctx, base);
}
}  // namespace

extern "C" void __imp__sub_8228BAB0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CAD8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228BE98(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216EE80(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82288750(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216FCA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822B7D68(PPCContext& ctx, uint8_t* base);

namespace {
void AddRow(PPCContext& ctx, uint8_t* base, uint32_t text, bool selectable = true) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = text;
  frame.ctx.r4.u64 = 0;  // plain text row
  frame.ctx.r5.u64 = 0;
  frame.ctx.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
  frame.ctx.r7.u64 = selectable ? 1 : 0;
  frame.ctx.r8.u64 = 0;
  __imp__sub_8228BAB0(frame.ctx, base);
}
// A (Enter / left click) this frame, the test the OPTIONS list uses.
bool ConfirmPressed(PPCContext& ctx, uint8_t* base) {
  return GuestBool(ctx, base, __imp__sub_8216EE80, 55) && GuestBool(ctx, base, __imp__sub_82288750, 0);
}
// The main menu reads the pads itself; this is the general confirm test the
// PC option rows use (options_menu.cpp).
bool MainConfirmPressed(PPCContext& ctx, uint8_t* base) {
  return GuestBool(ctx, base, __imp__sub_82288750, 0) && GuestBool(ctx, base, __imp__sub_8216FCA0, 0);
}
}  // namespace

// Moves the row just added (the last, `last`) to list row `to`: relinks its
// node and moves the per-row selectable bytes (menu +64) along. The list is
// circular from menu +0 (the first row); rows follow each other through node
// +4, node +0 points back (sub_8228BAB0 links each new row in before the
// first one).
bool MoveLastRow(uint8_t* base, uint32_t menu, int last, int to) {
  if (to <= 0 || to >= last) return false;
  const uint32_t head = R32(base, menu + 0);
  if (!head) return false;
  auto next = [&](uint32_t n) { return R32(base, n + 4); };
  auto prev = [&](uint32_t n) { return R32(base, n + 0); };
  const uint32_t node = prev(head);  // the last row
  uint32_t before = head;            // row `to - 1`
  for (int i = 0; i < to - 1; ++i) before = next(before);
  if (node == head || node == before) return false;
  // Unlink.
  const uint32_t p = prev(node), n = next(node);
  W32(base, p + 4, n);
  W32(base, n + 0, p);
  // Link in after `before`.
  const uint32_t after = next(before);
  W32(base, before + 4, node);
  W32(base, node + 0, before);
  W32(base, node + 4, after);
  W32(base, after + 0, node);
  uint8_t* sel = Host(base, menu + 64);
  const uint8_t ours = sel[last];
  for (int i = last; i > to; --i) sel[i] = sel[i - 1];
  sel[to] = ours;
  return true;
}

// Game row number (menu object +8) <-> list row.
uint16_t ListRow(uint16_t game_row) {
  if (g_main_row < 0) return game_row;
  if (game_row == 0xFFFF) return uint16_t(g_main_row);
  return game_row >= g_main_row ? uint16_t(game_row + 1) : game_row;
}
uint16_t GameRow(uint16_t list_row) {
  if (g_main_row < 0) return list_row;
  if (list_row == g_main_row) return 0xFFFF;
  return list_row > g_main_row ? uint16_t(list_row - 1) : list_row;
}

void Select(PPCContext& ctx, uint8_t* base, uint32_t row) {
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = row;
  __imp__sub_8228BE98(frame.ctx, base);
}

// List finisher: add our row to the main menu.
PPC_FUNC(sub_8228CAD8) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (lr == kMpOptionsDone && sr::PlayersBuilding(base)) {
    // MULTIPLAYER > PLAYERS (online_players.cpp) uses the OPTIONS build.
    sr::PlayersFillList(ctx, base);
    __imp__sub_8228CAD8(ctx, base);
    sr::PlayersAfterFinish(ctx, base);
    return;
  }
  if (lr == kMpOptionsDone) {
    // Multiplayer OPTIONS tab: "Player Name: X" after Audio.
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_mp_name_row = -1;
    g_mp_add_row = -1;
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && EnsureTexts(base)) {
      WriteText(base, g_name_text, NameRowText(), kInfoChars * 2);
      g_mp_name_row = int(R16(base, menu + 58));
      AddRow(ctx, base, g_name_text);
      // Friends (runtime eos_lan.cpp): this player's code, and Add Friend.
      rex::cvar::SetFlagByName("online_wanted", "true");
      g_code_shown = rex::cvar::GetFlagByName("online_friend_code");
      WriteText(base, g_code_text,
                "Friend Code: " + (g_code_shown.empty() ? std::string("connecting...") : g_code_shown), kInfoChars * 2);
      AddRow(ctx, base, g_code_text, false);
      WriteText(base, g_addfriend_text, "Add Friend", kInfoChars * 2);
      g_mp_add_row = int(R16(base, menu + 58));
      AddRow(ctx, base, g_addfriend_text);
    }
    __imp__sub_8228CAD8(ctx, base);
    if (g_mp_reselect && g_mp_name_row >= 0) Select(ctx, base, uint32_t(g_mp_name_row));
    g_mp_reselect = false;
    return;
  }
  if (lr == kMainMenuDone && CoopLoaded()) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && EnsureTexts(base)) {
      const int first = int(R16(base, menu + 58));
      AddRow(ctx, base, g_text[kMainLabel]);
      if (MoveLastRow(base, menu, first, kMainJoinRow)) g_main_row = kMainJoinRow;
      else g_main_row = first;
    }
  }
  __imp__sub_8228CAD8(ctx, base);
  if (lr == kMainMenuDone && g_main_row >= 0) {
    // The builder put the cursor on the game's remembered row before ours
    // was there: put it on the same row in the new order.
    const uint32_t object = g_main_object ? g_main_object : kMainMenuObject;
    Select(ctx, base, ListRow(R16(base, object + 8)));
  }
}

extern "C" void __imp__sub_822903E8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822B7328(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82348750(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228F878(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228B9D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8216ED48(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CBA0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8228CC90(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82287FC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_822C06B8(PPCContext& ctx, uint8_t* base);

namespace {
bool CoopTabOn(uint8_t* base) { return Host(base, kMultiplayerFlag)[0] == 0 && CoopLoaded(); }

void Call0(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*)) {
  rex::CallFrame frame(parent);
  fn(frame.ctx, base);
}
void Call1(PPCContext& parent, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint64_t r3) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = r3;
  fn(frame.ctx, base);
}

// The CO-OP tab's list, built the way the OPTIONS tab builds its own
// (sub_822B7328): list start, rows, finisher, first row selected, input on.
void BuildCoopTab(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  Call0(ctx, base, __imp__sub_82348750);
  Call0(ctx, base, __imp__sub_8228F878);
  W32(base, 0x82FFE3E0u, 0x8203C19Cu);  // as OPTIONS in the story (no subtitle)
  Host(base, 0x82FFE3DCu)[0] = 0;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = 0;
    frame.ctx.r4.u64 = 0;
    frame.ctx.r5.u64 = 0;
    frame.ctx.r6.u64 = 1;
    frame.ctx.r7.u64 = 0;
    __imp__sub_8228B9D0(frame.ctx, base);
  }
  const uint32_t menu = R32(base, kMenuPointer);
  g_pause_first = menu ? int(R16(base, menu + 58)) : 0;
  const int s = CoopState();
  g_pause_actions.clear();
  if (s & 3) g_pause_actions = {kStop};
  else g_pause_actions = {kHost, kJoin};
  // Player name row (only while no session: the name is sent when joining).
  if (!(s & 3)) g_pause_actions.push_back(kName);
  for (Action a : g_pause_actions) {
    if (a == kName) {
      WriteText(base, g_name_text, NameRowText(), kInfoChars * 2);
      AddRow(ctx, base, g_name_text);
      continue;
    }
    AddRow(ctx, base, g_text[a == kHost ? 0 : a == kJoin ? 1 : (s & 1) ? 2 : 3]);
  }
  // During a session: the join code / IP (and whether the other player is
  // in) as a greyed row under End Co-op.
  const std::string info = (s & 3) ? CoopInfo() : std::string();
  if (!info.empty() && g_info_text) {
    WriteText(base, g_info_text, info, kInfoChars * 2);
    AddRow(ctx, base, g_info_text, false);
  }
  {
    float f;
    const uint32_t bits = R32(base, 0x820875ECu);
    std::memcpy(&f, &bits, 4);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r4.u64 = 0xFFFFFFFFFFFFFFFFull;
    frame.ctx.r6.u64 = 1;
    frame.ctx.f1.f64 = double(f);
    __imp__sub_8228CAD8(frame.ctx, base);
  }
  Call1(ctx, base, __imp__sub_8228BE98, 0);
  W32(base, 0x8370DDCCu, 1);
  const uint32_t input = 0x83710690u;
  W32(base, input + 88, 0);
  W32(base, input + 96, 0);
  for (uint32_t o : {32u, 33u, 34u, 35u, 36u, 37u, 72u, 73u, 74u, 75u, 76u, 77u}) Host(base, input + o)[0] = 1;
  REXLOG_INFO("Co-op menu: CO-OP tab built ({} rows)", g_pause_actions.size() + (info.empty() ? 0 : 1));
}

// The CO-OP tab's update, as the OPTIONS tab's (sub_822B7D68): A on a row,
// up / down, then the pause menu's shared update (tabs, back).
void UpdateCoopTab(PPCContext& ctx, uint8_t* base) {
  if (g_name_changed && !g_dialog_open.load()) {
    g_name_changed = false;
    W32(base, kMenuRequested, kCoopId);  // build the tab again (new name)
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    bool acted = false;
    if (menu && !g_dialog_open.load() && g_pause_first >= 0 &&
        g_pause_first + int(g_pause_actions.size()) <= int(R16(base, menu + 58))) {
      // A row that no longer fits the session state is greyed out.
      const bool active = (CoopState() & 3) != 0;
      for (size_t i = 0; i < g_pause_actions.size(); ++i)
        Host(base, menu + 64 + uint32_t(g_pause_first) + uint32_t(i))[0] =
            g_pause_actions[i] == kName ? uint8_t(!active) : uint8_t((g_pause_actions[i] == kStop) == active);
      if (ConfirmPressed(ctx, base)) {
        acted = true;
        const int row = int(R16(base, menu + 62)) - g_pause_first;
        if (row >= 0 && row < int(g_pause_actions.size()) &&
            Host(base, menu + 64 + uint32_t(g_pause_first + row))[0]) {
          const Action a = g_pause_actions[size_t(row)];
          REXLOG_INFO("Co-op menu: {}", a == kHost ? "host" : a == kJoin ? "join" : a == kName ? "name" : "end");
          {
            rex::CallFrame frame(ctx);
            frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
            frame.ctx.r4.u64 = 1;
            frame.ctx.r5.u64 = 1;
            __imp__sub_82287FC0(frame.ctx, base);
          }
          if (a == kHost) StartAfterResume(1, {});
          else if (a == kStop) {
            CoopStop();
            W32(base, kMenuRequested, kCoopId);  // build the tab again (Host / Join)
          } else if (a == kName) {
            ShowDialog(ctx, base, DialogFor::kName);
          } else ShowDialog(ctx, base, DialogFor::kPause);
        }
      }
    }
    if (!acted && !g_dialog_open.load()) {
      if (GuestBool(ctx, base, __imp__sub_8216ED48, 37) && GuestBool(ctx, base, __imp__sub_82288750, 0))
        Call1(ctx, base, __imp__sub_8228CBA0, 1);
      else if (GuestBool(ctx, base, __imp__sub_8216ED48, 38) && GuestBool(ctx, base, __imp__sub_82288750, 0))
        Call1(ctx, base, __imp__sub_8228CC90, 1);
    }
  }
  Call0(ctx, base, __imp__sub_822C06B8);
}
}  // namespace

// Tab adder: after the story pause menu's OPTIONS tab, add CO-OP (menu id 46
// with the OPTIONS functions, see above).
PPC_FUNC(sub_822903E8) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t id = ctx.r5.u32;
  __imp__sub_822903E8(ctx, base);
  sr::PlayersAfterTabAdded(ctx, base, lr, id);  // MULTIPLAYER: PLAYERS after LEADERBOARDS
  if (id != kOptionsId || lr != kOptionsTabAdded || !CoopTabOn(base)) return;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!EnsureTexts(base)) return;
  for (uint32_t i = 0; i < 4; ++i)
    W32(base, kMenuTable + kCoopId * 16 + i * 4, R32(base, kMenuTable + kOptionsId * 16 + i * 4));
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = g_text[kTabLabel];
  frame.ctx.r4.u64 = 0;
  frame.ctx.r5.u64 = kCoopId;
  __imp__sub_822903E8(frame.ctx, base);
}

// OPTIONS build, also used by the CO-OP tab.
PPC_FUNC(sub_822B7328) {
  const uint32_t requested = R32(base, kMenuRequested);
  if (requested == kCoopId && CoopLoaded()) {
    BuildCoopTab(ctx, base);
    return;
  }
  __imp__sub_822B7328(ctx, base);
}

// OPTIONS update, also used by the CO-OP tab.
PPC_FUNC(sub_822B7D68) {
  const uint32_t current = R32(base, kMenuCurrent);
  if (current == kCoopId && CoopLoaded()) {
    UpdateCoopTab(ctx, base);
    return;
  }
  __imp__sub_822B7D68(ctx, base);
}

// Multiplayer OPTIONS update: A on our row opens the name box (the game's
// handler would only play the select sound for it).
extern "C" void __imp__sub_82349018(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82349018) {
  if (sr::PlayersCurrent(base)) {
    // MULTIPLAYER > PLAYERS: refresh the list; A on a greyed info row would
    // open the OPTIONS sub menus (Controls / Display / Audio), so it's eaten.
    sr::PlayersUpdate(ctx, base);
    const uint32_t menu = R32(base, kMenuPointer);
    if (menu && R16(base, menu + 62) < 3 && MainConfirmPressed(ctx, base)) return;
    __imp__sub_82349018(ctx, base);
    return;
  }
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const uint32_t menu = R32(base, kMenuPointer);
    if (R32(base, kMenuCurrent) == kMpOptionsId && menu && g_mp_name_row >= 0 &&
        g_mp_name_row < int(R16(base, menu + 58)) && !g_dialog_open.load()) {
      if (g_name_changed) {
        g_name_changed = false;
        g_mp_reselect = true;
        W32(base, kMenuRequested, kMpOptionsId);  // build the list again (new name)
      } else if (rex::cvar::GetFlagByName("online_friend_code") != g_code_shown) {
        W32(base, kMenuRequested, kMpOptionsId);  // the friend code arrived: show it
      } else if (g_mp_add_row >= 0 && R16(base, menu + 62) == uint16_t(g_mp_add_row) && MainConfirmPressed(ctx, base)) {
        REXLOG_INFO("Co-op menu: add friend (multiplayer options)");
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
          frame.ctx.r4.u64 = 1;
          frame.ctx.r5.u64 = 1;
          __imp__sub_82287FC0(frame.ctx, base);
        }
        ShowDialog(ctx, base, DialogFor::kFriend);
        return;
      } else if (R16(base, menu + 62) == uint16_t(g_mp_name_row) && MainConfirmPressed(ctx, base)) {
        REXLOG_INFO("Co-op menu: name (multiplayer options)");
        {
          rex::CallFrame frame(ctx);
          frame.ctx.r3.u64 = R32(base, 0x827B05B8u);  // the menu's select sound
          frame.ctx.r4.u64 = 1;
          frame.ctx.r5.u64 = 1;
          __imp__sub_82287FC0(frame.ctx, base);
        }
        ShowDialog(ctx, base, DialogFor::kName);
        return;
      }
    }
  }
  __imp__sub_82349018(ctx, base);
}

bool sr::CoopMainMenuPre(PPCContext& ctx, uint8_t* base) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_main_object = ctx.r3.u32;
  g_main_cursor_before = 0xFFFF;
  if (g_open_load) {
    // Open the load screen like LOAD GAME (see kbm.cpp, World Studio).
    g_open_load = false;
    PPC_STORE_U32(0x827AF780u, 0);
    PPC_STORE_U32(0x8300FD3Cu, 4);
    PPC_STORE_U32(0x8300FD40u, 4);
    PPC_STORE_U32(0x8370DDB0u, 44);
    REXLOG_INFO("Co-op menu: join code set, opening the load screen");
    ctx.r3.u64 = 1;
    return true;
  }
  const uint32_t menu = R32(base, kMenuPointer);
  if (g_main_row < 0 || !g_main_object || !menu || g_main_row >= int(R16(base, menu + 58))) return false;
  const uint16_t cursor = R16(base, menu + 62);
  // The game's A handler reads its own row number (it skips 0xFFFF, ours).
  W16(base, g_main_object + 8, GameRow(cursor));
  g_main_cursor_before = cursor;
  if (cursor == uint16_t(g_main_row) && !g_dialog_open.load() && (CoopState() & 3) == 0 &&
      MainConfirmPressed(ctx, base)) {
    REXLOG_INFO("Co-op menu: join from the main menu");
    ShowDialog(ctx, base, DialogFor::kMainMenu);
  }
  return false;
}

void sr::CoopMainMenuPost(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  const uint32_t menu = R32(base, kMenuPointer);
  if (g_main_cursor_before == 0xFFFF || !g_main_object || !menu) return;
  // After moving, the game stores the list cursor as its row number.
  const uint16_t cursor = R16(base, menu + 62);
  if (cursor != g_main_cursor_before) W16(base, g_main_object + 8, GameRow(cursor));
}

void DialogReleasePoll();

bool sr::CoopResumePulse() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_resume_action || !g_resume_clock) return false;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - g_resume_since).count();
  return ms >= 100 && ms < 250;
}

void sr::CoopMenuPoll(uint8_t* base) {
  sr::PlayersActivityPoll(base);  // what others see this player doing (PLAYERS tab)
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  DialogReleasePoll();
  if (g_resume_action && !g_dialog_open.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (!g_resume_clock) {
      g_resume_clock = true;
      g_resume_since = now;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_resume_since).count();
    const bool menu_open = R32(base, kActiveScreen) != 0;
    if ((!menu_open && ms >= 400) || ms >= 4000) {
      REXLOG_INFO("Co-op menu: {} ({} ms after leaving the menu, menu {})",
                  g_resume_action == 1 ? "hosting" : "joining", ms, menu_open ? "still open" : "closed");
      if (g_resume_action == 1) CoopHost();
      else CoopJoin(g_resume_code);
      g_resume_action = 0;
    }
  }
  if (!g_join_pending) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_join_since > std::chrono::minutes(5)) {
    g_join_pending = false;
    REXLOG_INFO("Co-op menu: no save loaded within 5 minutes; join cancelled");
    return;
  }
  // In the game (player exists, no menu, no cutscene) for 3 s in a row.
  const bool playing = R32(base, kPlayer) != 0 && R32(base, kActiveScreen) == 0 &&
                       !Host(base, 0x8370D991u)[0] && !Host(base, 0x8370D990u)[0];
  if (!playing) {
    g_join_ready = {};
    return;
  }
  if (g_join_ready.time_since_epoch().count() == 0) g_join_ready = now;
  if (now - g_join_ready < std::chrono::seconds(3)) return;
  g_join_pending = false;
  REXLOG_INFO("Co-op menu: save loaded, joining");
  CoopJoin(g_join_code);
}

bool sr::CoopDialogOpen() { return g_dialog_open.load(); }

namespace {
void DialogDone(bool ok) {
  std::string code;
  for (char c : g_typed)
    if (c != ' ') code += c;
  if (g_dialog_for == DialogFor::kFriend) {
    if (ok && !code.empty()) rex::cvar::SetFlagByName("online_friend_add", code);
    REXLOG_INFO("Co-op menu: friend code box {}", ok && !code.empty() ? "sent" : "cancelled");
    g_dialog_for = DialogFor::kNone;
    return;
  }
  if (g_dialog_for == DialogFor::kName) {
    std::string name = g_typed;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (ok && !name.empty() && !NameAllowed(name)) {
      REXLOG_WARN("Co-op menu: the name '{}' is reserved", name);
      g_name_refused_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      g_name_changed = true;
    } else if (ok && !name.empty()) {
      SetPlayerName(name);
      g_name_changed = true;
    }
    REXLOG_INFO("Co-op menu: name box {}", ok && !name.empty() ? "saved" : "cancelled");
    g_dialog_for = DialogFor::kNone;
    return;
  }
  if (ok && !code.empty()) {
    if (g_dialog_for == DialogFor::kPause) {
      StartAfterResume(2, code);
    } else if (g_dialog_for == DialogFor::kMainMenu) {
      g_join_code = code;
      g_join_pending = true;
      g_join_since = std::chrono::steady_clock::now();
      g_join_ready = {};
      g_open_load = true;
    }
  }
  REXLOG_INFO("Co-op menu: code box {}", ok && !code.empty() ? "join" : "cancelled");
  g_dialog_for = DialogFor::kNone;
}
}  // namespace

// The message dialog's update: typing, Enter / Esc for our code box.
PPC_FUNC(sub_8229F288) {
  const uint32_t object = ctx.r3.u32;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  if (!g_dialog || object != g_dialog) {
    __imp__sub_8229F288(ctx, base);
    return;
  }
  if (g_dialog_state == DialogState::kOpen) {
    const int r = PollTyping();
    if (g_typed_changed) {
      g_typed_changed = false;
      // Wrap the new text again, keeping the box shown as it is.
      uint32_t fade;
      std::memcpy(&fade, Host(base, object + 600), 4);
      const uint8_t shown = Host(base, object + 597)[0];
      WriteText(base, g_dialog_text, DialogBody(), kTextSize);
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = object;
      __imp__sub_8229EF58(frame.ctx, base);
      std::memcpy(Host(base, object + 600), &fade, 4);
      Host(base, object + 597)[0] = shown;
    }
    if (r != 0) {
      DialogDone(r > 0);
      Host(base, object + 597)[0] = 0;  // fade out
      g_dialog_state = DialogState::kClosing;
    }
  }
  __imp__sub_8229F288(ctx, base);
  if (g_dialog_state == DialogState::kClosing && (ctx.r3.u32 & 0xFF) == 0)
    g_dialog_state = DialogState::kWaitRelease;
}

void DialogReleasePoll() {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  // Menus get the keyboard back once Enter / Esc are let go (or they would
  // take the same press).
  if (g_dialog_state == DialogState::kWaitRelease && !KeyDown(VK_RETURN) && !KeyDown(VK_ESCAPE)) {
    g_dialog_state = DialogState::kClosed;
    g_dialog_open.store(false);
  }
}

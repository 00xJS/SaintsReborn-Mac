// MULTIPLAYER > GANGS > "Invite New Gang Member" on PC.
//
// On the 360 the gang manager (sub_82356998) adds a custom action to the
// Guide's player card with XCustomSetAction(0, "Invite to Gang") while the
// player may invite (owner / admin; "Invites disabled" otherwise). "Invite
// New Gang Member" (sub_82333C70) opens the Guide friends list
// (XamShowFriendsUI via sub_827172F8); picking a friend's "Invite to Gang"
// makes XCustomGetLastActionPress return (user, action 0, friend XUID), which
// sub_8235DEB8 turns into the DemonWare gang invite.
//
// Here the friends list is a picker (chat.cpp PickerBegin, drawn by
// fps_overlay.cpp) with the Saints Reborn friends from the runtime
// (SrFriendsSnapshot: the same XUIDs the game's friends enumerator uses);
// Enter on a friend presses action 0 for them.
#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include "chat.h"

#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &be, 4);
}
void W64(uint8_t* base, uint32_t a, uint64_t v) {
  const uint64_t be = __builtin_bswap64(v);
  std::memcpy(Host(base, a), &be, 8);
}

std::string ReadWide(uint8_t* base, uint32_t a, size_t max = 256) {
  std::string s;
  if (!a) return s;
  std::wstring w;
  for (size_t i = 0; i < max; ++i) {
    const uint8_t* p = Host(base, a + uint32_t(i * 2));
    const wchar_t c = wchar_t(p[0] << 8 | p[1]);
    if (!c) break;
    w.push_back(c);
  }
#ifdef _WIN32
  if (!w.empty()) {
    char out[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out, sizeof(out), nullptr, nullptr);
    if (n > 0) s.assign(out, size_t(n));
  }
#endif
  return s;
}

struct Friend {
  uint64_t xuid = 0;
  std::string name;
  bool online = false, mutual = false;
};
std::vector<Friend> Friends() {
  std::vector<Friend> out;
#ifdef _WIN32
  using Fn = int (*)(char*, int);
  HMODULE m = GetModuleHandleW(L"rexruntime.dll");
  const Fn fn = m ? reinterpret_cast<Fn>(GetProcAddress(m, "SrFriendsSnapshot")) : nullptr;
  if (!fn) return out;
  std::vector<char> buf(64 * 1024);
  if (fn(buf.data(), int(buf.size())) <= 0) return out;
  std::istringstream in(buf.data());
  std::string line;
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
      const size_t b = line.find('\t', a);
      f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
      if (b == std::string::npos) break;
      a = b + 1;
    }
    if (f.size() < 4) continue;
    Friend fr;
    fr.xuid = std::strtoull(f[0].c_str(), nullptr, 16);
    fr.name = f[1];
    fr.online = f[2] == "1";
    fr.mutual = f[3] == "1";
    if (fr.xuid) out.push_back(fr);
  }
#endif
  return out;
}

std::mutex g_mutex;
std::string g_action[4];  // XCustomSetAction texts by index ("" = none)
bool g_pressed = false;
uint32_t g_press_user = 0, g_press_action = 0;
uint64_t g_press_xuid = 0;

constexpr uint32_t kUserIndex = 0x827ADEC2u;  // signed byte: the signed-in player's controller

}  // namespace

// XCustomSetAction(index, text, flags): an action on the Guide's player card.
PPC_FUNC_IMPL(__imp__XCustomSetAction) {
  const uint32_t index = ctx.r3.u32;
  const std::string text = ReadWide(base, ctx.r4.u32);
  if (index < 4) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_action[index] != text) REXLOG_INFO("Friends UI: player card action {} = \"{}\"", index, text);
    g_action[index] = text;
  }
  ctx.r3.u64 = 0;
}

// XCustomGetLastActionPress(&user, &action, &xuid) -> TRUE once per press.
PPC_FUNC_IMPL(__imp__XCustomGetLastActionPress) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_pressed) {
    ctx.r3.u64 = 0;
    return;
  }
  g_pressed = false;
  if (ctx.r3.u32) W32(base, ctx.r3.u32, g_press_user);
  if (ctx.r4.u32) W32(base, ctx.r4.u32, g_press_action);
  if (ctx.r5.u32) W64(base, ctx.r5.u32, g_press_xuid);
  REXLOG_INFO("Friends UI: action {} pressed for {:016X}", g_press_action, g_press_xuid);
  ctx.r3.u64 = 1;
}

// XamShowFriendsUI(user) wrapper -> the friends picker.
PPC_FUNC(sub_827172F8) {
  std::string action;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    action = g_action[0];
  }
  const auto friends = Friends();
  std::vector<sr::PickerItem> items;
  for (const auto& f : friends)
    items.push_back({f.name, f.online ? std::string("Online") : std::string("Offline"), !f.online});
  const bool can_invite = !action.empty() && action.find("isabled") == std::string::npos;
  std::string note;
  if (friends.empty()) note = "No friends yet - add them in MULTIPLAYER > PLAYERS";
  else if (!can_invite) note = action.empty() ? "Only the gang's owner and admins can invite" : action;
  else note = "Pick a friend to invite to your gang";
  const int8_t user = int8_t(*Host(base, kUserIndex));
  sr::PickerBegin(can_invite ? "INVITE TO GANG" : "FRIENDS", note, items, [friends, can_invite, user](int i) {
    if (i < 0 || size_t(i) >= friends.size() || !can_invite) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_press_user = uint32_t(int32_t(user));
    g_press_action = 0;
    g_press_xuid = friends[size_t(i)].xuid;
    g_pressed = true;
    REXLOG_INFO("Friends UI: invite {} ({:016X})", friends[size_t(i)].name, g_press_xuid);
    // The game reads the press only when the Guide says one happened: its
    // notification loop (sub_8235DEB8) calls XCustomGetLastActionPress on
    // XN_CUSTOM_ACTIONPRESSED (0x06000003). Without it the choice was never
    // picked up and no gang invite was ever sent.
    if (auto* ks = REX_KERNEL_STATE()) ks->BroadcastNotification(0x06000003u, 0);
  });
  ctx.r3.u64 = 0;
}

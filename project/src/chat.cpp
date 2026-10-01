// In-game chat. T (game window in front, a lobby / match or a co-op session
// even alone) opens a "Say:" line under the chat lines; letters,
// digits and punctuation from the keyboard layout, Backspace, Enter sends,
// Esc cancels. While typing, the game and the mods get no keys (kbm.cpp,
// wml::SetKeysSuppressed). Lines: runtime eos_lan.cpp SrChatSend / SrChatPoll
// (lobbies and matches over Epic) and WhompaysCoop.dll WhompaysCoopChatSend /
// WhompaysCoopChatPoll (co-op). Drawn by fps_overlay.cpp (DrawChat).
#include "chat.h"

#include "coop_menu.h"
#include "wml/mod_loader.h"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kMaxInput = 120;
constexpr auto kShowFor = std::chrono::seconds(12);
constexpr auto kFade = std::chrono::seconds(2);

struct Line {
  std::string name, text;
  Clock::time_point at;
  bool system = false;
};

std::mutex g_mutex;
std::deque<Line> g_lines;
bool g_typing = false;
bool g_release_wait = false;  // Enter / Esc that closed the chat still held: the game doesn't get them yet
std::string g_input;
bool g_visible = false;
bool g_down[256] = {};

#ifdef _WIN32
template <class F>
F Export(const wchar_t* module, const char* name) {
  HMODULE m = GetModuleHandleW(module);
  return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}
#endif

int LobbyPlayers() {
#ifdef _WIN32
  using Fn = int (*)();
  static const Fn fn = Export<Fn>(L"rexruntime.dll", "SrChatAvailable");
  return fn ? fn() : 0;
#else
  return 0;
#endif
}
bool InSession() {
#ifdef _WIN32
  using Fn = int (*)();
  static const Fn fn = Export<Fn>(L"rexruntime.dll", "SrChatInSession");
  return fn && fn() != 0;
#else
  return false;
#endif
}
// Hosting or joined co-op (connected or still waiting for the other player).
bool CoopRunning() {
#ifdef _WIN32
  using Fn = int (*)();
  const Fn fn = Export<Fn>(L"WhompaysCoop.dll", "WhompaysCoopState");
  return fn && (fn() & 3);
#else
  return false;
#endif
}
bool CoopConnected() {
#ifdef _WIN32
  using Fn = int (*)();
  const Fn fn = Export<Fn>(L"WhompaysCoop.dll", "WhompaysCoopState");
  return fn && (fn() & 4);
#else
  return false;
#endif
}

std::string MyName() {
  std::string n = rex::cvar::GetFlagByName("xam_player_name");
  if (n.empty()) {
    const char* env = std::getenv("USERNAME");
    n = env && *env ? env : "Player";
  }
  return n.substr(0, 15);
}

void AddLine(const std::string& name, const std::string& text, bool system) {
  g_lines.push_back(Line{name, text, Clock::now(), system});
  while (g_lines.size() > 30) g_lines.pop_front();
}

bool Focused() {
#ifdef _WIN32
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
#else
  return false;
#endif
}

// New presses since the last poll.
bool Pressed(int vk) {
#ifdef _WIN32
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
  const bool down = false;
#endif
  const bool was = g_down[vk & 0xFF];
  g_down[vk & 0xFF] = down;
  return down && !was;
}
void ResetKeys() {
  for (int vk = 0; vk < 256; ++vk) {
#ifdef _WIN32
    g_down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
#endif
  }
}

// The character a key gives with the keyboard layout (UTF-8), "" for none.
std::string KeyText(int vk) {
#ifdef _WIN32
  BYTE state[256] = {};
  auto held = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
  if (held(VK_SHIFT)) state[VK_SHIFT] = 0x80;
  if (held(VK_CONTROL)) state[VK_CONTROL] = 0x80;
  if (held(VK_MENU)) state[VK_MENU] = 0x80;
  if (held(VK_RMENU)) { state[VK_CONTROL] = 0x80; state[VK_MENU] = 0x80; }  // AltGr
  if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
  if (held(VK_CONTROL) && !held(VK_RMENU)) return "";  // Ctrl shortcuts aren't text
  const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
  wchar_t w[4] = {};
  const int n = ToUnicodeEx(UINT(vk), MapVirtualKeyExW(UINT(vk), MAPVK_VK_TO_VSC, layout), state, w, 4, 0, layout);
  if (n != 1 || w[0] < 32 || w[0] == 127) return "";
  char out[8] = {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, 1, out, sizeof(out), nullptr, nullptr);
  return bytes > 0 ? std::string(out, size_t(bytes)) : std::string();
#else
  (void)vk;
  return "";
#endif
}

void Send(const std::string& text) {
#ifdef _WIN32
  using SendFn = int (*)(const char*);
  static const SendFn lobby = Export<SendFn>(L"rexruntime.dll", "SrChatSend");
  const SendFn coop = Export<SendFn>(L"WhompaysCoop.dll", "WhompaysCoopChatSend");
  int reached = 0;
  if (lobby) reached += lobby(text.c_str());
  if (coop && CoopConnected()) reached += coop(text.c_str());
  AddLine(MyName(), text, false);
  if (!reached) AddLine("", "(nobody else is here to read it)", true);
  REXLOG_INFO("Chat: sent ({} player(s))", reached);
#else
  (void)text;
#endif
}

void PollIncoming() {
#ifdef _WIN32
  using PollFn = int (*)(char*, int);
  static const PollFn lobby = Export<PollFn>(L"rexruntime.dll", "SrChatPoll");
  const PollFn coop = Export<PollFn>(L"WhompaysCoop.dll", "WhompaysCoopChatPoll");
  char buf[512];
  for (PollFn fn : {lobby, coop}) {
    if (!fn) continue;
    for (int i = 0; i < 16 && fn(buf, sizeof(buf)) > 0; ++i) {
      const std::string line(buf);
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      AddLine(line.substr(0, tab), line.substr(tab + 1), false);
    }
  }
#endif
}

}  // namespace

void sr::ChatPoll() {
  std::lock_guard<std::mutex> lock(g_mutex);
  PollIncoming();
#ifdef _WIN32
  if (g_release_wait && !(GetAsyncKeyState(VK_RETURN) & 0x8000) && !(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
    g_release_wait = false;
    wml::SetKeysSuppressed(false);
  }
#endif
  if (g_release_wait) {
    // (the keys are still held)
  } else if (!g_typing) {
    const bool t = Pressed('T');
    if (t && Focused() && !sr::CoopDialogOpen() && (LobbyPlayers() > 0 || InSession() || CoopRunning())) {
      g_typing = true;
      g_input.clear();
      ResetKeys();
      wml::SetKeysSuppressed(true);
    }
  } else if (!Focused()) {
    ResetKeys();  // typing continues when the window is back in front
  } else {
    for (int vk = 8; vk < 256; ++vk) {
      if (!Pressed(vk)) continue;
      if (vk == VK_RETURN) {
        std::string text = g_input;
        while (!text.empty() && text.back() == ' ') text.pop_back();
        g_typing = false;
        g_input.clear();
        if (!text.empty()) Send(text);
        break;
      }
      if (vk == VK_ESCAPE) {
        g_typing = false;
        g_input.clear();
        break;
      }
      if (vk == VK_BACK) {
        while (!g_input.empty() && (uint8_t(g_input.back()) & 0xC0) == 0x80) g_input.pop_back();  // UTF-8 tail
        if (!g_input.empty()) g_input.pop_back();
        continue;
      }
      const std::string ch = KeyText(vk);
      if (!ch.empty() && g_input.size() + ch.size() <= kMaxInput) g_input += ch;
    }
    if (!g_typing) {
      ResetKeys();
      g_release_wait = true;  // Enter / Esc still held: not for the game
    }
  }
  bool visible = g_typing;
  const auto now = Clock::now();
  for (const auto& l : g_lines) visible |= now - l.at < kShowFor;
  if (visible != g_visible) {
    g_visible = visible;
    wml::SetChatVisible(visible);
  }
}

bool sr::ChatTyping() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_typing || g_release_wait;
}

bool sr::ChatSnapshot(std::vector<ChatLine>& lines, std::string& input) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  const size_t max_lines = g_typing ? 8 : 6;
  for (auto it = g_lines.rbegin(); it != g_lines.rend() && lines.size() < max_lines; ++it) {
    float alpha = 1.0f;
    if (!g_typing) {
      const auto age = now - it->at;
      if (age >= kShowFor) break;
      if (age > kShowFor - kFade)
        alpha = std::chrono::duration<float>(kShowFor - age).count() / std::chrono::duration<float>(kFade).count();
    }
    lines.insert(lines.begin(), ChatLine{it->name, it->text, alpha, it->system});
  }
  input = g_input;
  return g_typing;
}

// Discord status: shows "Playing Saints Reborn" on the player's Discord
// profile with what they are doing (main menu, story, co-op, multiplayer,
// driving, cutscene ...) and how long they have been playing.
//
// Talks to the Discord app on the same PC over its local pipe
// (\\.\pipe\discord-ipc-N, frames of u32 op + u32 length + JSON), so no
// Discord library is needed. Nothing happens when Discord isn't running; the
// game checks again every 20 seconds. Turned off by a file named discord.off
// next to saintsrow.exe.
//
// The name shown ("Saints Reborn") and the picture (art asset "logo") belong
// to the Discord application whose id is kApplicationId (a file
// discord_app_id.txt next to the exe overrides it).

#include "discord_presence.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sr {
bool PlayerDriving(uint8_t* base);  // kbm.cpp
bool PauseMenuOpen();               // kbm.cpp
}  // namespace sr

namespace {

constexpr const char* kApplicationId = "1555304828085870663";

uint8_t* g_base = nullptr;

uint32_t Load32(uint32_t address) {
  uint32_t v;
  std::memcpy(&v, g_base + address, 4);
  return _byteswap_ulong(v);
}
uint8_t Load8(uint32_t address) { return g_base[address]; }

std::string Json(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
    else out += char(c);
  }
  return out + "\"";
}

int CoopState() {
#ifdef _WIN32
  HMODULE m = GetModuleHandleW(L"WhompaysCoop.dll");
  auto f = m ? reinterpret_cast<int (*)()>(GetProcAddress(m, "WhompaysCoopState")) : nullptr;
  return f ? f() : 0;
#else
  return 0;
#endif
}

// What the player is doing: two lines (Discord's "details" and "state").
void Describe(std::string& details, std::string& state) {
  const uint32_t player = Load32(0x8309ABECu);
  const bool creator = Load32(0x827B04D4u) == 1 || Load32(0x839E0DF8u) == 0x82FFB7BCu;
  const std::string online = rex::cvar::GetFlagByName("online_session");
  const int coop = CoopState();  // 1 hosting, 2 joined, 4 connected
  if (creator) {
    details = "Creating a character";
    state = "";
    return;
  }
  if (!player) {
    details = online.empty() ? "In the main menu" : "Multiplayer";
    state = online;
    return;
  }
  if (!online.empty()) {
    details = "Multiplayer";
  } else if (coop & 4) {
    details = (coop & 1) ? "Story co-op (hosting)" : "Story co-op";
  } else {
    details = "Story mode";
  }
  if (Load8(0x8370D991u) || Load8(0x8370D990u)) state = "Watching a cutscene";
  else if (sr::PauseMenuOpen()) state = "Paused";
  else if (sr::PlayerDriving(g_base)) state = online.empty() ? "Driving around Stilwater" : "Driving";
  else state = online.empty() ? "On foot in Stilwater" : online;
}

#ifdef _WIN32
class Pipe {
 public:
  ~Pipe() { Close(); }
  bool Open() {
    for (int i = 0; i < 10; ++i) {
      const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
      h_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h_ != INVALID_HANDLE_VALUE) return true;
    }
    return false;
  }
  void Close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  // Sends one frame and reads Discord's answer (it answers every frame).
  bool Call(uint32_t op, const std::string& json, std::string* reply = nullptr) {
    std::string frame(8, '\0');
    const uint32_t len = uint32_t(json.size());
    std::memcpy(&frame[0], &op, 4);
    std::memcpy(&frame[4], &len, 4);
    frame += json;
    DWORD done = 0;
    if (!WriteFile(h_, frame.data(), DWORD(frame.size()), &done, nullptr) || done != frame.size()) return false;
    uint32_t head[2];
    if (!ReadAll(head, 8)) return false;
    if (head[1] > (1u << 20)) return false;
    std::string body(head[1], '\0');
    if (head[1] && !ReadAll(&body[0], head[1])) return false;
    if (reply) *reply = body;
    return head[0] != 2;  // 2 = close
  }

 private:
  bool ReadAll(void* out, uint32_t n) {
    auto* p = static_cast<char*>(out);
    while (n) {
      DWORD got = 0;
      if (!ReadFile(h_, p, n, &got, nullptr) || !got) return false;
      p += got;
      n -= got;
    }
    return true;
  }
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

void Run(std::string app_id) {
  const long long started =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  bool logged_missing = false;
  int nonce = 0;
  while (true) {
    Pipe pipe;
    if (!pipe.Open()) {
      if (!logged_missing) REXLOG_INFO("Discord: not running (checking again every 20 s)");
      logged_missing = true;
      std::this_thread::sleep_for(std::chrono::seconds(20));
      continue;
    }
    std::string reply;
    if (!pipe.Call(0, "{\"v\":1,\"client_id\":" + Json(app_id) + "}", &reply) ||
        reply.find("\"READY\"") == std::string::npos) {
      REXLOG_WARN("Discord: handshake refused ({})", reply.substr(0, 200));
      std::this_thread::sleep_for(std::chrono::seconds(60));
      continue;
    }
    REXLOG_INFO("Discord: connected, showing the game status");
    logged_missing = false;
    std::string last;
    auto last_sent = std::chrono::steady_clock::time_point{};
    while (true) {
      std::string details, state;
      Describe(details, state);
      const std::string key = details + "\n" + state;
      const auto now = std::chrono::steady_clock::now();
      // Discord takes at most 5 updates per 20 s; send on change (and once a minute).
      if (key != last || now - last_sent > std::chrono::seconds(60)) {
        std::string activity = "{\"details\":" + Json(details);
        if (!state.empty()) activity += ",\"state\":" + Json(state);
        activity += ",\"timestamps\":{\"start\":" + std::to_string(started) + "}";
        activity += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Saints Reborn\"}}";
        const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
                                ",\"activity\":" + activity + "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
        if (!pipe.Call(1, cmd, &reply)) break;
        if (reply.find("\"ERROR\"") != std::string::npos) REXLOG_WARN("Discord: {}", reply.substr(0, 300));
        last = key;
        last_sent = now;
      }
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    REXLOG_INFO("Discord: disconnected");
    std::this_thread::sleep_for(std::chrono::seconds(20));
  }
}
#endif

}  // namespace

void sr::StartDiscordPresence(const std::filesystem::path& exe_dir, uint8_t* guest_base) {
#ifdef _WIN32
  std::error_code ec;
  if (std::filesystem::exists(exe_dir / "discord.off", ec)) {
    REXLOG_INFO("Discord: status off (discord.off)");
    return;
  }
  std::string id = kApplicationId;
  {
    std::ifstream in(exe_dir / "discord_app_id.txt");
    std::string line;
    if (std::getline(in, line)) {
      line.erase(0, line.find_first_not_of(" \t\r\n"));
      line.erase(line.find_last_not_of(" \t\r\n") + 1);
      if (!line.empty()) id = line;
    }
  }
  if (id.empty()) {
    REXLOG_INFO("Discord: no application id; status off");
    return;
  }
  g_base = guest_base;
  std::thread([id]() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Run(id);
  }).detach();
#else
  (void)exe_dir;
  (void)guest_base;
#endif
}

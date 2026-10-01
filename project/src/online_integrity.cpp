// Fair play for online matches (Saints Reborn).
//
// Public Xbox Live matches keep modded and unmodded games apart, and ranked
// is for unmodded games only; private play (System Link, co-op by join code)
// has no such rule. The rules themselves live in the runtime (online_identity
// .cpp, eos_lan.cpp, xgi_live.inc); this file decides what "modded" means and
// tells the runtime through the cvars online_modded / online_modded_reason:
//
//  - any enabled mod that is not part of the standard game (kOnlineSafeMods),
//  - custom map packs in maps\ (other players may not have them),
//  - the World Studio editor host,
//  - cheat tools or debuggers (checked every few seconds; once seen, the game
//    stays modded until it is restarted without them).
//
// It also shows the runtime's online notices (cvar online_notice) over the
// game for a few seconds.
#include "online_integrity.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "wml/mod_loader.h"
#include "wml/mod_list.h"
#include "world_studio_bridge.h"

namespace {

// Mods that ship with the game and don't change how a match plays.
const char* const kOnlineSafeMods[] = {"SixtyFPS", "Multiplayer", "WhompaysCoop"};

// Cheat tools / debuggers, matched against lower-case process and module names.
const char* const kCheatProcesses[] = {"cheatengine", "cheat engine", "artmoney", "wemod",     "x64dbg",
                                       "x32dbg",      "ollydbg",      "gamehacker", "squalr",  "reclass",
                                       "scanmem",     "trainer.exe"};
const char* const kCheatModules[] = {"speedhack", "vehdebug", "luaclient", "cheatengine", "allochook",
                                     "dbk64",     "dbk32",    "wemod",     "artmoney"};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
  return s;
}

std::string g_static_reason;          // mods / maps / studio (decided at start)
std::string g_tool_reason;            // cheat tool or debugger seen (sticky)

void Publish() {
  std::string reason = g_static_reason;
  if (!g_tool_reason.empty()) reason += (reason.empty() ? "" : "; ") + g_tool_reason;
  // Reason first, then the flag: the runtime reads the reason once it sees the flag.
  rex::cvar::SetFlagByName("online_modded_reason", reason);
  rex::cvar::SetFlagByName("online_modded", reason.empty() ? "false" : "true");
}

std::string FindCheatTool() {
  if (IsDebuggerPresent()) return "a debugger is attached";
  BOOL remote = FALSE;
  if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) && remote) return "a debugger is attached";
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
      const std::string name = Lower(Narrow(pe.szExeFile));
      for (const char* bad : kCheatProcesses)
        if (name.find(bad) != std::string::npos) {
          CloseHandle(snap);
          return "cheat tool running (" + name + ")";
        }
    }
    CloseHandle(snap);
  }
  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap != INVALID_HANDLE_VALUE) {
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
      const std::string name = Lower(Narrow(me.szModule));
      for (const char* bad : kCheatModules)
        if (name.find(bad) != std::string::npos) {
          CloseHandle(snap);
          return "cheat tool loaded into the game (" + name + ")";
        }
    }
    CloseHandle(snap);
  }
  return {};
}

void Watch() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  auto next_scan = std::chrono::steady_clock::now();
  auto notice_until = std::chrono::steady_clock::time_point{};
  bool notice_shown = false;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (g_tool_reason.empty() && now >= next_scan) {
      next_scan = now + std::chrono::seconds(5);
      const std::string tool = FindCheatTool();
      if (!tool.empty()) {
        g_tool_reason = tool;
        REXLOG_WARN("Online fair play: {} - this game counts as modded until it is restarted without it", tool);
        Publish();
      }
    }
    const std::string notice = rex::cvar::GetFlagByName("online_notice");
    if (!notice.empty()) {
      rex::cvar::SetFlagByName("online_notice", "");
      wml::SetHostOverlayText(notice);
      notice_until = now + std::chrono::seconds(12);
      notice_shown = true;
    } else if (notice_shown && now >= notice_until) {
      wml::SetHostOverlayText("");
      notice_shown = false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

}  // namespace

void sr::StartOnlineIntegrity(const std::filesystem::path& exe_dir) {
  std::vector<std::string> mods;
  for (const std::string& id : wml::EnabledModIds()) {
    bool safe = false;
    for (const char* ok : kOnlineSafeMods) safe |= _stricmp(id.c_str(), ok) == 0;
    if (!safe) mods.push_back(id);
  }
  std::vector<std::string> maps;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(exe_dir / "maps", ec)) {
    if (e.is_directory(ec) && std::filesystem::exists(e.path() / "bundle_info.xtbl", ec))
      maps.push_back(wml::PathToUtf8(e.path().filename()));
  }
  auto join = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
  };
  std::string reason;
  if (!mods.empty()) reason = "mods " + join(mods);
  if (!maps.empty()) reason += (reason.empty() ? "" : "; ") + ("custom maps " + join(maps));
  if (sr::world_studio::EditorHostEnabled()) reason += (reason.empty() ? "" : "; ") + std::string("World Studio");
  g_static_reason = reason;
  Publish();
  if (reason.empty())
    REXLOG_INFO("Online fair play: unmodded (ranked and unmodded public matches allowed)");
  else
    REXLOG_INFO("Online fair play: modded ({}) - public matches with other modded games, private play, no ranked",
                reason);
  std::thread(Watch).detach();
}

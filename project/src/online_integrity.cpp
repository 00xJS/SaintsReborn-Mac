// Fair play for online matches (Saints Reborn).
//
// Mods, trainers and cheat tools only work in a Private Party (invite-only
// Live session); public matches (Player, Ranked) and System Link are for
// unmodded games only. Story co-op by join code has no such rule. The rules
// themselves live in the runtime (online_identity.cpp, eos_lan.cpp,
// xgi_live.inc); this file decides what "modded" means and
// tells the runtime through the cvars online_modded / online_modded_reason:
//
//  - any enabled mod that is not part of the standard game (kOnlineSafeMods)
//    and changes more than how the game looks (wml/fair_play.cpp decides:
//    textures, fonts and fair lighting are fine; approved code mods too),
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
#include <mutex>
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

std::mutex g_reason_mutex;
std::string g_static_reason;          // mods / maps / studio (decided at start)
std::string g_tool_reason;            // cheat tool or debugger seen (sticky)

void Publish() {
  std::lock_guard<std::mutex> l(g_reason_mutex);
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

// Online notices: shown for 12 s, then taken away. Its own thread, so a slow
// cheat-tool scan (Watch) can never leave a notice on screen.
void Notices() {
  auto notice_until = std::chrono::steady_clock::time_point{};
  bool notice_shown = false;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const std::string notice = rex::cvar::GetFlagByName("online_notice");
    if (!notice.empty()) {
      rex::cvar::SetFlagByName("online_notice", "");
      wml::SetHostOverlayText(notice);
      notice_until = now + std::chrono::seconds(13);  // panel shows 12 s, then fades
      notice_shown = true;
    } else if (notice_shown && now >= notice_until) {
      wml::SetHostOverlayText("");
      notice_shown = false;
      REXLOG_INFO("Online notice hidden");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

void Watch() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  while (g_tool_reason.empty()) {
    const std::string tool = FindCheatTool();
    if (!tool.empty()) {
      g_tool_reason = tool;
      REXLOG_WARN("Online fair play: {} - this game counts as modded until it is restarted without it", tool);
      Publish();
      break;
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }
}

}  // namespace

void sr::StartOnlineIntegrity(const std::filesystem::path& exe_dir) {
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
  std::string other;
  if (!maps.empty()) other = "custom maps " + join(maps);
  if (sr::world_studio::EditorHostEnabled()) other += (other.empty() ? "" : "; ") + std::string("World Studio");

  // Mods: the fair play check runs in the background (packfiles are big);
  // until it is done the game counts as modded if it has any mods enabled.
  auto mods_reason = [](bool& done) {
    std::vector<std::pair<std::string, std::string>> results;
    done = wml::FairCheckResults(results);
    std::vector<std::string> mods;
    for (const std::string& id : wml::EnabledModIds()) {
      bool safe = false;
      for (const char* ok : kOnlineSafeMods) safe |= _stricmp(id.c_str(), ok) == 0;
      if (safe) continue;
      bool looks_only = false;
      for (const auto& [rid, why] : results) looks_only |= rid == id && why.empty();
      if (!looks_only) mods.push_back(id);
    }
    if (mods.empty()) return std::string();
    std::string s;
    for (const auto& x : mods) s += (s.empty() ? "" : ", ") + x;
    return (done ? "mods " : "mods (still being checked) ") + s;
  };
  auto apply = [other](const std::string& mods) {
    std::string reason = mods;
    if (!other.empty()) reason += (reason.empty() ? "" : "; ") + other;
    {
      std::lock_guard<std::mutex> l(g_reason_mutex);
      g_static_reason = reason;
    }
    Publish();
    return reason;
  };
  bool done = false;
  const std::string first = apply(mods_reason(done));
  auto report = [](const std::string& reason) {
    if (reason.empty())
      REXLOG_INFO("Online fair play: unmodded or looks-only mods (all online play allowed)");
    else
      REXLOG_INFO("Online fair play: modded ({}) - Private Parties and story co-op only (no public matches, no "
                  "System Link)",
                  reason);
  };
  report(first);
  if (!done) {
    std::thread([mods_reason, apply, report]() {
      bool finished = false;
      std::string mods;
      while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        mods = mods_reason(finished);
      }
      report(apply(mods));
    }).detach();
  }
  std::thread(Watch).detach();
  std::thread(Notices).detach();
}

#include "profiler.h"

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <timeapi.h>
#endif

namespace sr {

#ifdef _WIN32
namespace {

std::atomic<bool> g_running{false};
std::thread g_thread;

struct Module {
  uintptr_t base = 0, end = 0;
  std::string name;
  std::vector<std::pair<uint32_t, std::string>> exports;  // sorted by RVA
};
std::vector<Module> g_modules;
std::vector<std::pair<uintptr_t, uint32_t>> g_guest;  // host address -> guest address
uintptr_t g_exe_base = 0, g_exe_end = 0;
std::vector<std::pair<uint32_t, std::string>> g_map;  // exe RVA -> linker map symbol

// saintsrow.map (written by the linker, copied next to the exe) names the
// host code in the executable, e.g. overrides and import thunks, which the
// guest function table can't.
void LoadMap() {
  FILE* f = std::fopen("saintsrow.map", "rb");
  if (!f) return;
  char line[1024];
  unsigned long long preferred = 0x140000000ull;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned long long p = 0;
    if (std::sscanf(line, " Preferred load address is %llx", &p) == 1) { preferred = p; continue; }
    unsigned sec = 0, off = 0;
    char name[512];
    unsigned long long address = 0;
    if (std::sscanf(line, " %x:%x %511s %llx", &sec, &off, name, &address) == 4 && sec && address > preferred)
      g_map.emplace_back(uint32_t(address - preferred), name);
  }
  std::fclose(f);
  std::sort(g_map.begin(), g_map.end());
}

void LoadModules() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    Module m;
    m.base = uintptr_t(me.modBaseAddr);
    m.end = m.base + me.modBaseSize;
    char name[260] = {};
    WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name) - 1, nullptr, nullptr);
    m.name = name;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress && dir.Size) {
      auto* ex = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(m.base + dir.VirtualAddress);
      auto* functions = reinterpret_cast<DWORD*>(m.base + ex->AddressOfFunctions);
      auto* names = reinterpret_cast<DWORD*>(m.base + ex->AddressOfNames);
      auto* ordinals = reinterpret_cast<WORD*>(m.base + ex->AddressOfNameOrdinals);
      for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        DWORD rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) continue;  // forwarder
        m.exports.emplace_back(rva, reinterpret_cast<const char*>(m.base + names[i]));
      }
      std::sort(m.exports.begin(), m.exports.end());
    }
    if (GetModuleHandleW(nullptr) == me.hModule) {
      g_exe_base = m.base;
      g_exe_end = m.end;
    }
    g_modules.push_back(std::move(m));
  }
  CloseHandle(snap);
  for (const PPCFuncMapping* f = PPCFuncMappings; f->host; ++f) {
    g_guest.emplace_back(uintptr_t(f->host), uint32_t(f->guest));
  }
  std::sort(g_guest.begin(), g_guest.end());
  LoadMap();
}

std::string Resolve(uintptr_t address) {
  char text[400];
  if (address >= g_exe_base && address < g_exe_end && !g_guest.empty()) {
    auto it = std::upper_bound(g_guest.begin(), g_guest.end(),
                               std::make_pair(address, uint32_t(0xFFFFFFFF)));
    const uint32_t rva = uint32_t(address - g_exe_base);
    auto mit = std::upper_bound(g_map.begin(), g_map.end(), std::make_pair(rva, std::string("\xff")));
    if (mit != g_map.begin()) {
      --mit;
      // A linker symbol closer than the guest function start is the better name.
      if (it == g_guest.begin() || mit->first >= uint32_t(std::prev(it)->first - g_exe_base)) {
        if (mit->second.rfind("sub_", 0) == 0) return mit->second;
        std::snprintf(text, sizeof(text), "exe!%s", mit->second.c_str());
        return text;
      }
    }
    if (it != g_guest.begin()) {
      --it;
      if (address - it->first < 0x40000) {
        std::snprintf(text, sizeof(text), "sub_%08X", it->second);
        return text;
      }
    }
    std::snprintf(text, sizeof(text), "exe+%llX", (unsigned long long)(address - g_exe_base));
    return text;
  }
  for (const Module& m : g_modules) {
    if (address < m.base || address >= m.end) continue;
    uint32_t rva = uint32_t(address - m.base);
    auto it = std::upper_bound(m.exports.begin(), m.exports.end(),
                               std::make_pair(rva, std::string("\xff")));
    if (it != m.exports.begin()) {
      --it;
      // Shorten C++ decorated names to "Class::Function".
      std::string sym = it->second;
      if (!sym.empty() && sym[0] == '?') {
        size_t at = sym.find("@@");
        std::string head = sym.substr(1, at == std::string::npos ? std::string::npos : at - 1);
        size_t a = head.find('@');
        if (a != std::string::npos) {
          size_t b = head.find('@', a + 1);
          sym = head.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1) +
                "::" + head.substr(0, a);
        } else {
          sym = head;
        }
      }
      std::snprintf(text, sizeof(text), "%s!%s", m.name.c_str(), sym.c_str());
    } else {
      std::snprintf(text, sizeof(text), "%s+%X", m.name.c_str(), rva);
    }
    return text;
  }
  std::snprintf(text, sizeof(text), "%llX", (unsigned long long)address);
  return text;
}

std::string ThreadName(HANDLE thread) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!fn) return {};
  PWSTR wide = nullptr;
  if (FAILED(fn(thread, &wide)) || !wide) return {};
  char narrow[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
  LocalFree(wide);
  return narrow;
}

struct Target {
  std::string label;
  HANDLE handle = nullptr;
  uint64_t samples = 0;
  std::unordered_map<uintptr_t, uint64_t> leaf;             // raw leaf address
  std::unordered_map<std::string, uint64_t> exclusive;      // resolved leaf
  std::unordered_map<std::string, uint64_t> inclusive;      // resolved, per sample once
  std::unordered_map<std::string, uint64_t> via;            // host leaf: nearest game function
};

bool FindTargets(std::vector<Target>& targets) {
  // The render thread, the game's main thread, and every other guest thread
  // (the game's workers and jobs).
  const char* wanted[] = {"GPU Commands", "Main XThread", "XThread"};
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, te.th32ThreadID);
    if (!h) continue;
    std::string name = ThreadName(h);
    bool keep = false;
    for (const char* w : wanted) {
      if (name.rfind(w, 0) == 0) {
        bool dup = false;
        for (auto& t : targets) dup |= t.label == name;
        if (!dup && targets.size() < 24) {
          Target t;
          t.label = name;
          t.handle = h;
          targets.push_back(std::move(t));
          keep = true;
        }
        break;
      }
    }
    if (!keep) CloseHandle(h);
  }
  CloseHandle(snap);
  return targets.size() >= 2;
}

constexpr int kMaxDepth = 24;

// Captures the call stack of a suspended thread without allocating.
int Capture(HANDLE thread, uintptr_t (&frames)[kMaxDepth]) {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(thread) == DWORD(-1)) return 0;
  int n = 0;
  if (GetThreadContext(thread, &ctx)) {
    for (; n < kMaxDepth && ctx.Rip; ++n) {
      frames[n] = uintptr_t(ctx.Rip);
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
      if (!fn) {
        if (!ctx.Rsp) break;
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher, nullptr);
      }
    }
  }
  ResumeThread(thread);
  return n;
}

void Report(std::vector<Target>& targets) {
  for (Target& t : targets) {
    if (!t.samples) continue;
    auto dump = [&](const char* kind, std::unordered_map<std::string, uint64_t>& counts, size_t top) {
      std::vector<std::pair<uint64_t, std::string>> v;
      for (auto& [k, c] : counts) v.emplace_back(c, k);
      std::sort(v.rbegin(), v.rend());
      std::string line = "PROFILE " + t.label + " " + kind + " (" + std::to_string(t.samples) + " samples):";
      for (size_t i = 0; i < v.size() && i < top; ++i) {
        char part[480];
        std::snprintf(part, sizeof(part), " | %.1f%% %s", 100.0 * v[i].first / t.samples,
                      v[i].second.c_str());
        line += part;
      }
      REXLOG_INFO("{}", line);
    };
    dump("self", t.exclusive, 40);
    dump("total", t.inclusive, 60);
    dump("host-time-via", t.via, 30);
    t.samples = 0;
    t.exclusive.clear();
    t.inclusive.clear();
    t.via.clear();
  }
}

void Loop() {
  timeBeginPeriod(1);
  std::vector<Target> targets;
  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::unordered_map<uintptr_t, std::string> names;
  auto name_of = [&](uintptr_t a) -> const std::string& {
    auto it = names.find(a);
    if (it != names.end()) return it->second;
    return names.emplace(a, Resolve(a)).first->second;
  };
  while (g_running.load()) {
    // 100 samples a second per thread: suspending every guest thread 1000
    // times a second cost the game noticeable fps while profiling.
    Sleep(10);
    if (targets.size() < 2) {
      for (auto& t : targets) CloseHandle(t.handle);
      targets.clear();
      if (!FindTargets(targets)) {
        Sleep(500);
        continue;
      }
      if (g_modules.empty()) LoadModules();
    }
    for (Target& t : targets) {
      uintptr_t frames[kMaxDepth];
      int n = Capture(t.handle, frames);
      if (!n) continue;
      ++t.samples;
      const std::string& leaf = name_of(frames[0]);
      t.exclusive[leaf]++;
      // Time spent in host code (locks, waits, runtime): charge it to the game
      // function that called into it, with the host function it ended in.
      auto is_game = [](const std::string& n) {
        return n.rfind("sub_", 0) == 0 || n.rfind("exe!__imp__sub_", 0) == 0;
      };
      if (!is_game(leaf)) {
        // The nearest runtime (rexruntime) frame says which kernel service
        // it was; the nearest game function says who asked for it.
        const std::string* runtime = nullptr;
        for (int i = 1; i < n; ++i) {
          const std::string& s = name_of(frames[i]);
          if (!runtime && s.rfind("rexruntime.dll!", 0) == 0 && s.find("HostToGuest") == std::string::npos &&
              s.find("thread::Wait") == std::string::npos)
            runtime = &s;
          if (is_game(s)) {
            char rva[48] = "";
            if (frames[i] >= g_exe_base && frames[i] < g_exe_end)
              std::snprintf(rva, sizeof(rva), "@exe+%llX", (unsigned long long)(frames[i] - g_exe_base));
            t.via[s + rva + " > " + (runtime ? *runtime + " > " : std::string()) + leaf]++;
            break;
          }
        }
      }
      std::vector<const std::string*> seen;
      for (int i = 0; i < n; ++i) {
        const std::string& s = name_of(frames[i]);
        if (std::find(seen.begin(), seen.end(), &s) != seen.end()) continue;
        seen.push_back(&s);
        t.inclusive[s]++;
      }
    }
    if (std::chrono::steady_clock::now() >= next_report) {
      Report(targets);
      next_report += std::chrono::seconds(10);
      // Guest threads come and go; look again next time.
      for (auto& t : targets) CloseHandle(t.handle);
      targets.clear();
    }
  }
  for (auto& t : targets) CloseHandle(t.handle);
  timeEndPeriod(1);
}

}  // namespace

void StartProfiler() {
  FILE* f = std::fopen("perf_profile", "rb");
  if (!f) return;
  std::fclose(f);
  g_running = true;
  g_thread = std::thread(Loop);
  REXLOG_INFO("Sampling profiler enabled");
}

void StopProfiler() {
  if (!g_running.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}
#else
void StartProfiler() {}
void StopProfiler() {}
#endif

}  // namespace sr

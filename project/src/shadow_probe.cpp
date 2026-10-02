// Native version + probe of the character/vehicle shadow-volume builder sub_822447B0 (the
// hottest function of the game's job worker threads: 11-26 % of each worker's
// samples). Only active with a file named "shadow_probe" next to the exe:
// logs every 10 s "SHADOW PROBE: calls/s, us per call, CPU ms/s, by caller
// and by object type (vtable)".
//
// Callers: sub_82234CA8 and sub_82237560 (both clear a 64 KB stack buffer,
// let the object fill it through vtable +48, then call this with the buffer,
// object +8 (position) and object +20 (rotation)).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <rex/logging.h>
#include <rex/ppc/function.h>
#include <rex/ppc/intrinsics.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void __imp__sub_822447B0(PPCContext& ctx, uint8_t* base);

namespace {

bool Enabled() {
  static const bool on = [] {
    FILE* f = std::fopen("shadow_probe", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  return on;
}

int64_t Now() {
#ifdef _WIN32
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
#else
  return 0;
#endif
}
int64_t Freq() {
#ifdef _WIN32
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart;
#else
  return 1;
#endif
}

struct Stat {
  uint64_t calls = 0;
  int64_t ticks = 0;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Stat> g_by_caller, g_by_vtable;
Stat g_total;
int64_t g_window_start = 0;

uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

std::string Top(std::unordered_map<uint32_t, Stat>& m, double freq) {
  std::vector<std::pair<uint32_t, Stat>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
  std::string s;
  char buf[96];
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    std::snprintf(buf, sizeof buf, " [%08X %llu calls %.1f us]", v[i].first,
                  static_cast<unsigned long long>(v[i].second.calls),
                  v[i].second.calls ? v[i].second.ticks * 1e6 / freq / v[i].second.calls : 0.0);
    s += buf;
  }
  return s;
}

}  // namespace

#include "native_shadow.inc"
#include "native_math.inc"
#include "native_vfetch.inc"
#include "native_flush.inc"
#include "native_clip.inc"

PPC_FUNC(sub_822447B0) {
  if (!Enabled()) {
    const int mode = native_shadow::Mode();
    if (mode == 1) {
      if (native_shadow::Run(ctx, base, true)) return;
    } else if (mode == 2) {
      static thread_local uint32_t tick = 0;
      if ((++tick & 31) == 0) { native_shadow::Verify(ctx, base); return; }
      if (native_shadow::Run(ctx, base, true)) return;
    }
    __imp__sub_822447B0(ctx, base);
    return;
  }
  const uint32_t caller = uint32_t(ctx.lr);
  const uint32_t obj = ctx.r4.u32 - 8;
  const uint32_t vtable = obj >= 0x10000 ? Load32(base, obj) : 0;
  const int64_t t0 = Now();
  __imp__sub_822447B0(ctx, base);
  const int64_t dt = Now() - t0;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_total.calls++;
  g_total.ticks += dt;
  auto& c = g_by_caller[caller];
  c.calls++;
  c.ticks += dt;
  auto& v = g_by_vtable[vtable];
  v.calls++;
  v.ticks += dt;
  if (!g_window_start) g_window_start = t0;
  static const double freq = double(Freq());
  const double secs = (t0 - g_window_start) / freq;
  if (secs >= 10.0) {
    REXLOG_INFO("SHADOW PROBE 10 s: {:.0f} calls/s, {:.1f} us per call, {:.1f} ms/s CPU | by caller:{} | by vtable:{}",
                g_total.calls / secs, g_total.calls ? g_total.ticks * 1e6 / freq / g_total.calls : 0.0,
                g_total.ticks * 1e3 / freq / secs, Top(g_by_caller, freq), Top(g_by_vtable, freq));
    g_total = {};
    g_by_caller.clear();
    g_by_vtable.clear();
    g_window_start = t0;
  }
}

// ALLOC PROBE (file "alloc_probe"): the game's VirtualAlloc wrapper 8271B570(r3 address, r4 size,
// r5 type, r6 protect) and VirtualFree 82717360(r3 address, r4 size, r5 type): calls and bytes per
// second by caller, every 10 s ("ALLOC PROBE"). Committing memory the game had decommitted makes the
// runtime zero it.
extern "C" void __imp__sub_8271B570(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82717360(PPCContext& ctx, uint8_t* base);
namespace alloc_probe {
bool On() {
  static const bool on = [] {
    FILE* f = std::fopen("alloc_probe", "rb");
    if (f) std::fclose(f);
    return f != nullptr;
  }();
  return on;
}
struct S { uint64_t calls = 0, bytes = 0, ticks = 0; };
std::mutex m;
std::unordered_map<uint64_t, S> by;  // (kind << 63) | (type << 32) | caller
int64_t start = 0;
void Add(bool fr, uint32_t caller, uint32_t type, uint32_t size, int64_t dt) {
  std::lock_guard<std::mutex> lock(m);
  auto& s = by[(uint64_t(fr) << 63) | (uint64_t(type) << 32) | caller];
  ++s.calls; s.bytes += size; s.ticks += dt;
  const int64_t now = Now();
  if (!start) start = now;
  static const double freq = double(Freq());
  const double secs = (now - start) / freq;
  if (secs >= 10.0) {
    std::vector<std::pair<uint64_t, S>> v(by.begin(), by.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ticks > b.second.ticks; });
    std::string t;
    for (size_t i = 0; i < v.size() && i < 10; ++i) {
      char buf[160];
      std::snprintf(buf, sizeof buf, " [%s %08X type %08X: %.0f/s, %.1f MB/s, %.1f ms/s]",
                    (v[i].first >> 63) ? "free" : "alloc", uint32_t(v[i].first),
                    uint32_t(v[i].first >> 32) & 0x7FFFFFFF, v[i].second.calls / secs,
                    v[i].second.bytes / secs / 1048576.0, v[i].second.ticks * 1e3 / freq / secs);
      t += buf;
    }
    REXLOG_INFO("ALLOC PROBE 10 s:{}", t);
    by.clear();
    start = now;
  }
}
}  // namespace alloc_probe

// ARENA COMMITS WITHOUT KERNEL ZEROING (2026-10-02). The game's job arenas (82633108 reserve, grown by
// 82633340 in 64 KB commits, shrunk by 826335C0, released by 82633240) are created and released ~1000x/s and
// recommit ~300 MB/s; the kernel zero-fills every commit (Memory::Zero) - ~5 % of all busy CPU at the air spot,
// on the job workers and the main thread. Arena memory below the high-water mark is reused with stale contents
// anyway, and arenas that need zeros (flag byte +56) memset it themselves, so the commit at 826334A8 gets
// X_MEM_NOZERO when dist\arena_nozero exists (opt-in); dist\arena_poison = NOZERO and fill with 0xCD (test that nothing
// relies on zeroed pages).
namespace arena_fast {
int Mode() {
  static const int m = [] {
    // Opt-in since 2026-10-02 20:30: one GPU device-removed (TDR) happened in an "on" bench run (nz_on3) - not
    // proven related, but no gain on 28 threads either. File "arena_nozero" = on.
    int r = 0;
    if (FILE* f = std::fopen("arena_nozero", "rb")) { std::fclose(f); r = 1; }
    else if (FILE* f2 = std::fopen("arena_poison", "rb")) { std::fclose(f2); r = 2; }
    REXLOG_INFO("Arena commits: {}", r == 0 ? "kernel zeroing (original)" : r == 1 ? "no kernel zeroing" :
                "no kernel zeroing, POISON fill 0xCD (test mode)");
    return r;
  }();
  return m;
}
}  // namespace arena_fast

PPC_FUNC(sub_8271B570) {
  if (uint32_t(ctx.lr) == 0x826334A8u && (ctx.r5.u32 & 0x1000u) && arena_fast::Mode() != 0) {
    ctx.r5.u64 = ctx.r5.u32 | 0x00800000u;  // X_MEM_NOZERO
    if (arena_fast::Mode() == 2) {
      const uint32_t size = ctx.r4.u32;
      __imp__sub_8271B570(ctx, base);
      if (ctx.r3.u32) std::memset(base + ctx.r3.u32, 0xCD, size);
      return;
    }
  }
  if (!alloc_probe::On()) { __imp__sub_8271B570(ctx, base); return; }
  const uint32_t caller = uint32_t(ctx.lr), size = ctx.r4.u32, type = ctx.r5.u32;
  const int64_t t0 = Now();
  __imp__sub_8271B570(ctx, base);
  alloc_probe::Add(false, caller, type, size, Now() - t0);
}
PPC_FUNC(sub_82717360) {
  if (!alloc_probe::On()) { __imp__sub_82717360(ctx, base); return; }
  const uint32_t caller = uint32_t(ctx.lr), size = ctx.r4.u32, type = ctx.r5.u32;
  const int64_t t0 = Now();
  __imp__sub_82717360(ctx, base);
  alloc_probe::Add(true, caller, type, size, Now() - t0);
}

// Probe of the character/vehicle shadow-volume builder sub_822447B0 (the
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

PPC_FUNC(sub_822447B0) {
  if (!Enabled()) {
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

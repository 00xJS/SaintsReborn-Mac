// Optional performance log: when a file named "perf_log" exists next to the
// exe, a line with frame timing and the busiest threads is written to
// saintsrow_sdk.log every two seconds.
#pragma once

#include <atomic>

#include <cstdint>

namespace sr {

void StartPerfMonitor();
void StopPerfMonitor();

// Called by the present hook around the game's present call.
void PerfFrameBegin();
void PerfFrameEnd();

// Wait accounting for the game's main thread: true if waits on this thread
// should be timed, and the result keyed by the guest caller address.
bool PerfTrackWaits();
void PerfRecordWait(uint32_t guest_caller, uint64_t microseconds);
bool PerfWaitsOn();
void PerfRecordThreadWait(uint32_t wait_function, uint32_t guest_caller, uint64_t microseconds);

// Counters shown in the performance log (per second).
enum PerfCounter { kPerfTilingExecutor, kPerfTilingReplay, kPerfCommandBuffers, kPerfCounterCount };
void PerfCount(PerfCounter counter);

}  // namespace sr

namespace sr {

// Holds the game to at most `fps` frames per second (0 = no limit). Called
// by the present hook before each present.
void LimitFrameRate(double fps);

// Frame rate cap used by the present hook. F10 cycles it through
// 30/60/90/120; the choice is kept in fps_cap.txt next to the executable.
extern std::atomic<int> g_fps_cap;
void LoadFpsCap();
int CycleFpsCap();
// Options menu: set (and save) a cap from 30/60/90/120/0 (0 = unlimited).
void SetFpsCap(int cap);

}  // namespace sr

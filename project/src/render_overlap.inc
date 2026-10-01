// 24. RENDER OVERLAP (2026-09-30, build ov1). SR1's frame is a serial hand-off: the main thread's game update
//    starts by waiting for the render thread's "render done" event (82189418, event handle [0x829D4F3C+12]), so
//    update N+1 never overlaps render N (TIMELINE 02:32: both wait ~5-6 ms per frame for each other).
//    render_overlap.txt = where the render thread signals "done" early:
//      1 = after the colour pass is joined (821929C8)   (tail: 82192CE0, effects, 82181BC8, callbacks)
//      2 = after the colour pass job is started (82192850)
//      3 = before the frame render 82193A20 (after visibility) - most overlap, most risk
//    Its normal end-of-frame SetEvent (82716D20 returning to 82189244) is skipped for that frame, and the kick
//    821888C0 (pool/list reset, camera copy, callback queue copy A->B, wakes the render thread) first waits until
//    the render thread has really finished the previous frame (host counters, 2 s safety timeout).
//    render_overlap_ab = overlap on/off every 10 s: log "RENDER OVERLAP A/B" (fps per mode, kick waits).
extern "C" void __imp__sub_82716D20(PPCContext& ctx, uint8_t* base);
extern "C" __declspec(dllimport) int __stdcall SwitchToThread(void);
namespace render_overlap {
int Point() {
    static const int p = [] {
        int v = 0;
        if (FILE* f = std::fopen("render_overlap.txt", "rb")) {
            if (std::fscanf(f, "%d", &v) != 1) v = 0;
            std::fclose(f);
        }
        return (v >= 1 && v <= 3) ? v : 0;
    }();
    return p;
}
bool AbOn() { static const bool on = FileExists("render_overlap_ab"); return on; }
std::atomic<bool> ab_active{true};
std::atomic<uint64_t> kicked{0}, done{0};
std::atomic<bool> early{false};  // early signal sent for the frame being rendered
std::atomic<uint64_t> early_count{0}, kick_wait_ticks{0}, kick_waits{0}, timeouts{0};
inline bool Active() { return Point() > 0 && (!AbOn() || ab_active.load(std::memory_order_relaxed)); }
void Signal(PPCContext& ctx, uint8_t* base) {
    if (early.load(std::memory_order_relaxed)) return;
    thread_local PPCContext saved;
    saved = ctx;
    ctx.r3.u64 = Rd32(base, 0x829D4F3C + 12);
    ctx.lr = 0;
    __imp__sub_82716D20(ctx, base);  // SetEvent(render done)
    const auto fp = ctx.fpscr;
    ctx = saved;
    ctx.fpscr = fp;
    early.store(true, std::memory_order_release);
    early_count.fetch_add(1, std::memory_order_relaxed);
}
// Called by the SR_RT_TIMED wrappers. true = skip the call.
inline bool Before(PPCContext& ctx, uint8_t* base, uint32_t fn) {
    if (fn == 0x82716D20) {
        if (uint32_t(ctx.lr) != 0x82189244) return false;
        const bool skip = early.exchange(false, std::memory_order_acq_rel);
        done.fetch_add(1, std::memory_order_release);  // the render thread has finished this frame
        if (skip) { ctx.r3.u64 = 1; return true; }
        return false;
    }
    if (fn == 0x82193A20 && Point() == 3 && Active()) Signal(ctx, base);
    return false;
}
inline void After(PPCContext& ctx, uint8_t* base, uint32_t fn) {
    if ((fn == 0x821929C8 && Point() == 1) || (fn == 0x82192850 && Point() == 2))
        if (Active()) Signal(ctx, base);
}
// Main thread, start of the kick.
void KickWait() {
    if (!Point()) return;
    const uint64_t k = kicked.load(std::memory_order_relaxed);
    if (done.load(std::memory_order_acquire) < k) {
        const uint64_t t0 = __rdtsc();
        const auto start = std::chrono::steady_clock::now();
        int spins = 0;
        while (done.load(std::memory_order_acquire) < k) {
            if (++spins < 2000) _mm_pause();
            else {
                SwitchToThread();
                if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                    timeouts.fetch_add(1);
                    REXLOG_WARN("RENDER OVERLAP: kick waited 2 s for the render thread - continuing (kicked {}, done {})",
                                k, done.load());
                    done.store(k);
                    break;
                }
            }
        }
        kick_wait_ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        kick_waits.fetch_add(1, std::memory_order_relaxed);
    }
    kicked.store(k + 1, std::memory_order_relaxed);
    // Statistics + A/B switching (main thread only).
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    static auto mode_start = std::chrono::steady_clock::now();
    static uint64_t frames[2] = {}, frames_now = 0;
    static double secs[2] = {};
    ++frames_now;
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        const int m = Active() ? 1 : 0;
        frames[m] += frames_now; secs[m] += std::chrono::duration<double>(now - mode_start).count();
        frames_now = 0; mode_start = now; next = now + std::chrono::seconds(10);
        const double us = mesh_census::TickUs();
        const uint64_t w = kick_waits.exchange(0), wt = kick_wait_ticks.exchange(0);
        REXLOG_INFO("RENDER OVERLAP point {}: this window {} | early signals {} | kick waited for the render thread {} "
                    "times, avg {:.2f} ms | timeouts {} | cumulative fps: overlap OFF {:.1f} ({:.0f} s), ON {:.1f} ({:.0f} s)",
                    Point(), m ? "ON" : "OFF", early_count.exchange(0), w, w ? wt * us / 1000.0 / w : 0.0,
                    timeouts.load(), secs[0] > 0 ? frames[0] / secs[0] : 0.0, secs[0],
                    secs[1] > 0 ? frames[1] / secs[1] : 0.0, secs[1]);
        if (AbOn()) ab_active.store(!ab_active.load());
    }
}
}  // namespace render_overlap

// NATIVE CONSTANT FLUSH + 68-BYTE COPY (2026-10-02).
// 825DB5C8(r3 device, r4 dirty mask (64 bits), r5 first register, r6 source): D3D's float-constant flush. For every
// run of set bits (MSB first; one bit = 16 registers = 64 bytes) it writes a PM4 type-0 packet into the device's
// command buffer (+40 write pointer, +44 limit): 0x80000000 pads until the header sits at 12 mod 16, the header
// ((count - 1) << 16 | register), then the constants copied with aligned 16-byte loads/stores (lvx/stvx).
// ~2 % of busy CPU on the render thread and the colour worker each. The native copy does the same bytes in the
// same places; when the whole mask does not fit before the limit (the original then grows the buffer through
// 825DAF48 in the middle) the original runs.
// 82129CA0(r3 dst, r4 src): copies 17 floats (68 bytes) with lfs/stfs - a plain word copy, same order.
// dist\native_flush_off = originals; dist\native_flush_verify = 1 call in 32 computes the native result, runs the
// original and compares ("NATIVE FLUSH VERIFY").
extern "C" void __imp__sub_825DB5C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82129CA0(PPCContext& ctx, uint8_t* base);
namespace native_flush {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline int Ones(uint64_t m) { return ~m == 0 ? 64 : __builtin_clzll(~m); }
inline u32 Rd(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline void Wr(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
int Mode() {
    static const int m = [] {
        if (FILE* f = std::fopen("native_flush_off", "rb")) { std::fclose(f); return 0; }
        if (FILE* f = std::fopen("native_flush_verify", "rb")) { std::fclose(f); return 2; }
        return 1;
    }();
    return m;
}
std::atomic<uint64_t> v_flush{0}, v_flush_bad{0}, v_copy{0}, v_copy_bad{0}, grow{0};
std::atomic<int64_t> v_last{0};
void VerifyLog() {
    const int64_t now = int64_t(GetTickCount64());
    int64_t last = v_last.load();
    if (now - last > 10000 && v_last.compare_exchange_strong(last, now))
        REXLOG_INFO("NATIVE FLUSH VERIFY: constant flushes {} compared, {} differ ({} grew the buffer -> original) | "
                    "68-byte copies {} compared, {} differ", v_flush.load(), v_flush_bad.load(), grow.load(),
                    v_copy.load(), v_copy_bad.load());
}
// Bytes the flush writes for this mask (worst case of the original's fit test summed per run).
inline bool Fits(u64 mask, u32 wp, u32 limit) {
    // The original checks each run: wp + (dwords + 3) * 4 < limit, with wp advancing by the run.
    while (mask) {
        const int lz = __builtin_clzll(mask);
        mask <<= lz;
        const int run = Ones(mask);  // ones
        const u32 dwords = u32(run) * 16;
        if (!(wp + (dwords + 3) * 4 < limit)) return false;
        // padding to 12 mod 16, header, data
        u32 p = wp;
        const u32 m = p & 15;
        if (m == 4) p += 4; else if (m == 0) p += 8; else if (m == 12) p += 12;
        p += 4 + dwords * 4;
        wp = p;
        mask = run >= 64 ? 0 : mask << run;
    }
    return true;
}
// Writes into guest memory; returns the final write pointer and register index.
inline void Flush(u8* b, u64 mask, u32& wp, u32& reg, u32 src) {
    u32 s = src;
    while (mask) {
        const int lz = __builtin_clzll(mask);
        s += u32(lz) * 64;
        reg += u32(lz) * 16;
        mask <<= lz;
        const int run = Ones(mask);
        const u32 dwords = u32(run) * 16;
        const u32 m = wp & 15;
        if (m == 4) { wp += 4; Wr(b, wp, 0x80000000u); }
        else if (m == 0) { Wr(b, wp + 4, 0x80000000u); Wr(b, wp + 8, 0x80000000u); wp += 8; }
        else if (m == 12) { Wr(b, wp + 4, 0x80000000u); Wr(b, wp + 8, 0x80000000u); Wr(b, wp + 12, 0x80000000u); wp += 12; }
        wp += 4;
        Wr(b, wp, ((dwords - 1) << 16) | reg);
        reg += dwords;
        for (int i = 0; i < run; ++i) {
            // lvx/stvx: 16-byte aligned addresses
            for (u32 k = 0; k < 64; k += 16)
                std::memcpy(G(b, (wp + 4 + k) & ~15u), G(b, (s + k) & ~15u), 16);
            s += 64;
            wp += 64;
        }
        mask = run >= 64 ? 0 : mask << run;
    }
}
}  // namespace native_flush

PPC_FUNC(sub_825DB5C8) {
    using namespace native_flush;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_825DB5C8(ctx, base); return; }
    const u32 dev = ctx.r3.u32;
    const u64 mask = ctx.r4.u64;
    const u32 wp0 = Rd(base, dev + 40), limit = Rd(base, dev + 44);
    if (!Fits(mask, wp0, limit)) {
        if (mode == 2) ++grow;
        __imp__sub_825DB5C8(ctx, base);
        return;
    }
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            // Native into a copy of the output area, then the original for real; compare.
            u32 wp = wp0, reg = ctx.r5.u32;
            u32 end_est = wp0;
            {   // size of the output
                u64 mm = mask; u32 p = wp0;
                while (mm) { const int lz = __builtin_clzll(mm); mm <<= lz; const int run = Ones(mm);
                    const u32 m = p & 15; if (m == 4) p += 4; else if (m == 0) p += 8; else if (m == 12) p += 12;
                    p += 4 + u32(run) * 64; mm = run >= 64 ? 0 : mm << run; }
                end_est = p;
            }
            const u32 lo = (wp0 + 4) & ~15u, hi = (end_est + 4 + 15) & ~15u;
            std::vector<u8> before(G(base, lo), G(base, lo) + (hi - lo));
            Flush(base, mask, wp, reg, ctx.r6.u32);
            std::vector<u8> mine(G(base, lo), G(base, lo) + (hi - lo));
            std::memcpy(G(base, lo), before.data(), hi - lo);
            __imp__sub_825DB5C8(ctx, base);
            const bool bad = Rd(base, dev + 40) != wp || std::memcmp(mine.data(), G(base, lo), hi - lo) != 0;
            ++v_flush;
            if (bad) ++v_flush_bad;
            VerifyLog();
            return;
        }
    }
    u32 wp = wp0, reg = ctx.r5.u32;
    Flush(base, mask, wp, reg, ctx.r6.u32);
    Wr(base, dev + 40, wp);
    ctx.r4.u64 = wp;
    ctx.r5.u64 = reg;
}

// Called from the SR_RT_TIMED wrapper of 82129CA0 (render_fixes.cpp). true = done natively.
bool sr_native_copy68(PPCContext& ctx, uint8_t* base) {
    using namespace native_flush;
    const int mode = Mode();
    if (mode == 0) return false;
    const u32 dst = ctx.r3.u32, src = ctx.r4.u32;
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            u8 want[68];
            for (u32 k = 0; k < 68; k += 4) std::memcpy(want + k, G(base, src + k), 4);
            __imp__sub_82129CA0(ctx, base);
            const bool bad = std::memcmp(want, G(base, dst), 68) != 0;
            ++v_copy;
            if (bad && v_copy_bad.fetch_add(1) < 12) {
                std::string d;
                for (u32 k = 0; k < 68; k += 4) {
                    u32 a, b2; std::memcpy(&a, want + k, 4); std::memcpy(&b2, G(base, dst + k), 4);
                    if (a != b2) d += fmt::format(" +{}:{:08X}>{:08X}", k, __builtin_bswap32(a), __builtin_bswap32(b2));
                }
                REXLOG_INFO("NATIVE FLUSH copy differs (dst {:08X} src {:08X}):{}", dst, src, d);
            }
            VerifyLog();
            return true;
        }
    }
    for (u32 k = 0; k < 68; k += 4) std::memcpy(G(base, dst + k), G(base, src + k), 4);
    float last;
    const u32 bits = Rd(base, src + 64);
    std::memcpy(&last, &bits, 4);
    ctx.f0.f64 = double(last);
    ctx.r11.u64 = src + 12;
    ctx.r10.u64 = dst + 12;
    return true;
}

// 826306A8 = "current render context" (D3D device/render struct block of this thread), ~40k calls per frame
// (render thread + colour worker). r3 = [r13+7716] unless this thread is neither the render thread
// ([0x83710344]) nor a job worker currently running for it (82637670: worker table 0x840B3C2C, 6 x 144 bytes,
// +8 thread, +44 -> +36 owner) - then the default block 0x84095BF0 is swapped for 0x8409F730.
extern "C" void __imp__sub_826306A8(PPCContext& ctx, uint8_t* base);
namespace native_flush {
std::atomic<uint64_t> v_ctx{0}, v_ctx_bad{0};
inline u32 CurrentContext(PPCContext& ctx, u8* b) {
    const u32 thread = Rd(b, Rd(b, ctx.r13.u32 + 256) + 332);
    const u32 render = Rd(b, 0x83710344u);
    const u32 cur = Rd(b, Rd(b, ctx.r13.u32) + 7716);  // [[r13] + 7716]
    if (thread == render) return cur;
    for (u32 i = 0; i < 6; ++i) {
        const u32 e = 0x840B3C2Cu + i * 144;
        if (Rd(b, e + 8) == thread) {
            if (Rd(b, Rd(b, e + 44) + 36) == render) return cur;
            break;
        }
    }
    return cur == 0x84095BF0u ? 0x8409F730u : cur;
}
}  // namespace native_flush
bool sr_native_context(PPCContext& ctx, uint8_t* base) {
    using namespace native_flush;
    const int mode = Mode();
    if (mode == 0) return false;
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            const u32 want = CurrentContext(ctx, base);
            __imp__sub_826306A8(ctx, base);
            ++v_ctx;
            if (ctx.r3.u32 != want && v_ctx_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE FLUSH context differs: native {:08X} original {:08X}", want, ctx.r3.u32);
            static thread_local uint32_t t2 = 0;
            if ((++t2 & 1023) == 0)
                REXLOG_INFO("NATIVE FLUSH VERIFY context: {} compared, {} differ", v_ctx.load(), v_ctx_bad.load());
            return true;
        }
    }
    ctx.r3.u64 = CurrentContext(ctx, base);
    return true;
}

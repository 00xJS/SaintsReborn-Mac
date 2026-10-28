// NATIVE SET TEXTURE + VS CONSTANT UPLOAD (2026-10-02, perf round 4). Colour worker helpers, ~8 % + ~2 % of its
// busy samples at the air spot.
//
// 8214BE00(r3 texture handle, r4 slot): per-thread cache [[r13]+7696] + (slot+11)*4; unchanged -> nothing.
//   Otherwise resolves the handle with 82645940 (table [0x832B6024], 20-byte entries, count [0x832B61CC]; entries
//   with more than one frame pick frame (int)(fps * c * time) % frames, c = [0x82089234], time = [0x83710328]) and
//   calls 825CD488(device [[r13]+7688], slot, fetch = [[entry+4]+68], 1 << (63 - (slot + 20))): merges the
//   texture's fetch constant into the device shadow (+1024 + slot*24, 6 words, keeps some bits of the old words,
//   clamps two mip fields with the bytes at device +11694/+11720 + slot), sets the dirty bit (+16), stores the
//   texture at +(slot+3072)*4 and releases the old one (+10780 fence -> old+8, or an 8-byte entry in the queue
//   +13168/+13172 whose top bit comes from an UNINITIALISED stack word of 825CD488's frame - read from the guest
//   stack exactly like the original). All guest stack writes of the original are made too (savegprlr_26 /
//   savegprlr_25 areas, back chains, the 8-byte queue scratch, the byte 82645940 leaves at r1-148).
//   Native only when the handle resolves; unset textures, failed resolves and a full release queue -> original.
// 825D0D80(r3 device, r4 first register, r5 source, r6 count, r7 dirty mask): copies count vec4 (16-byte
//   unaligned loads, aligned stores) to device +1792 + reg*16 and ORs r7 into device +0. Also its two stack stores
//   ([r1-32] = r3, [r1+48] = r7).
// dist\native_tex_off = originals; dist\native_tex_verify = 1 call in 32 runs both on the same memory (snapshot /
// restore of the touched regions) and compares ("NATIVE TEX VERIFY").
#include <climits>
#include <cmath>
#include <emmintrin.h>
#include <vector>
extern "C" void __imp__sub_8214BE00(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_825D0D80(PPCContext& ctx, uint8_t* base);
namespace native_tex {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t; using s32 = int32_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline u32 Rd(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline u64 Rd64(u8* b, u32 a) { u64 v; std::memcpy(&v, G(b, a), 8); return __builtin_bswap64(v); }
inline void Wr(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(G(b, a), &v, 4); }
inline void Wr64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(G(b, a), &v, 8); }
inline u32 Rotl(u32 x, int n) { return (x << n) | (x >> ((32 - n) & 31)); }
int Mode() {
    static const int m = [] {
        int r = 1;
        if (FILE* f = std::fopen("native_tex_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_tex_verify", "rb")) { std::fclose(f2); r = 2; }
        REXLOG_INFO("Native set texture / VS constants (8214BE00, 825D0D80): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
// 82645940: returns the entry address (0 = invalid); *anim_byte = the byte it leaves at r1-4 of its caller's
// frame (only when the entry has more than one frame).
inline u32 Resolve(u8* b, u64 h64, bool& anim, u32& anim_byte) {
    anim = false;
    const s32 h = s32(u32(h64));
    if (h < 0 || h >= s32(Rd(b, 0x832B61CCu))) return 0;
    const u32 tbl = Rd(b, 0x832B6024u);
    u64 idx = h64;
    const u32 e = tbl + u32(h) * 20u;
    const u32 obj = Rd(b, e + 4);
    const u32 frames = Rd(b, obj + 12) >> 16;  // lhz
    if (frames > 1) {
        const u32 fps = G(b, obj + 21)[0];
        const u32 t = Rd(b, 0x83710328u);
        const double f13 = double(float(double(int64_t(fps))));
        const double f12 = double(float(double(int64_t(u64(t)))));
        u32 cbits = Rd(b, 0x82089234u); float c; std::memcpy(&c, &cbits, 4);
        double f0 = double(float(f13 * double(c)));
        f0 = double(float(f0 * f12));
        int64_t conv = std::isnan(f0) ? int64_t(0x80000000U) : (f0 >= double(INT_MAX)) ? INT_MAX
                       : int64_t(_mm_cvttsd_si32(_mm_load_sd(&f0)));
        const u32 r10 = u32(conv);
        const s32 a = s32(r10), d = s32(frames);
        const u32 q = u32((d && !(a == INT32_MIN && d == -1)) ? a / d : 0);
        const u64 prod = u64(int64_t(s32(q)) * int64_t(d));
        const u32 rem = u32(u64(r10) - prod) & 0xFFFFu;
        idx = h64 + rem;
        anim = true;
        anim_byte = fps;
    }
    const u32 a2 = u32(idx) << 2;
    const u64 s = idx + a2;
    return u32(u64(u32(s) << 2) + tbl);
}
struct Plan {
    bool equal = false;
    u32 r1, blk, tbl, r27, dev, slot, fetch, old, r29o, release = 0;  // release: 1 fence, 2 queue
    u64 mask;
    bool anim; u32 anim_byte;
    u32 fence, wp, stackword;
};
// Pure reads; false = the original must run.
inline bool MakePlan(PPCContext& ctx, u8* b, Plan& p) {
    p.r1 = ctx.r1.u32;
    p.blk = Rd(b, ctx.r13.u32);
    p.tbl = Rd(b, p.blk + 7696);
    p.slot = ctx.r4.u32;
    p.r27 = u32((ctx.r4.u64 + 11) << 2) & 0xFFFFFFFCu;
    const u32 cached = Rd(b, p.tbl + p.r27);
    if (s32(cached) == ctx.r3.s32) { p.equal = true; return true; }
    if (ctx.r3.s32 < 0) return false;
    const u32 res = Resolve(b, ctx.r3.u64, p.anim, p.anim_byte);
    if (!res) return false;
    const u32 tex = Rd(b, res + 4);
    p.fetch = Rd(b, tex + 68);
    p.dev = Rd(b, p.blk + 7688);
    const u8 sh = u8(u32(ctx.r4.u64 + 20));
    p.mask = (sh & 0x40) ? 0 : (0x8000000000000000ull >> (sh & 0x7F));
    p.r29o = u32((ctx.r4.u64 + 3072) << 2) & 0xFFFFFFFCu;
    p.old = Rd(b, p.dev + p.r29o);
    if (p.old) {
        p.fence = Rd(b, p.dev + 10780);
        if (p.fence) p.release = 1;
        else if (Rd(b, p.dev + 10784) & Rd(b, p.old)) {
            p.wp = Rd(b, p.dev + 13168);
            if (!(p.wp < Rd(b, p.dev + 13172))) return false;  // queue full: original grows it
            p.release = 2;
            p.stackword = Rd(b, p.r1 - 144 - 160 + 80);
        }
    }
    return true;
}
inline void Apply(PPCContext& ctx, u8* b, const Plan& p) {
    const u32 r1 = p.r1;
    // 8214BE00 prologue: savegprlr_26 + stwu -144.
    Wr64(b, r1 - 56, ctx.r26.u64); Wr64(b, r1 - 48, ctx.r27.u64); Wr64(b, r1 - 40, ctx.r28.u64);
    Wr64(b, r1 - 32, ctx.r29.u64); Wr64(b, r1 - 24, ctx.r30.u64); Wr64(b, r1 - 16, ctx.r31.u64);
    Wr(b, r1 - 8, u32(ctx.lr));
    Wr(b, r1 - 144, r1);
    if (p.equal) return;
    const u32 r1p = r1 - 144;
    if (p.anim) Wr(b, r1p - 4, p.anim_byte);  // what 82645940's std leaves under savegprlr's 4-byte lr store
    // 825CD488 prologue: savegprlr_25 (values as 8214BE00 had them) + stwu -160.
    Wr64(b, r1p - 64, ctx.r25.u64); Wr64(b, r1p - 56, 7696); Wr64(b, r1p - 48, p.r27);
    Wr64(b, r1p - 40, ctx.r3.u64); Wr64(b, r1p - 32, p.blk); Wr64(b, r1p - 24, ctx.r30.u64);
    Wr64(b, r1p - 16, ctx.r31.u64); Wr(b, r1p - 8, 0x8214BE6Cu);
    Wr(b, r1p - 160, r1p);
    const u32 dev = p.dev, f = p.fetch;
    if (f) {
        const u32 w32 = Rd(b, f + 32), w48 = Rd(b, f + 48), w28 = Rd(b, f + 28), w36 = Rd(b, f + 36),
                  w40 = Rd(b, f + 40), w44 = Rd(b, f + 44);
        const u32 fc = dev + 1024 + p.slot * 24;
        const u32 W0 = Rd(b, fc), W1 = Rd(b, fc + 4), W3 = Rd(b, fc + 12), W4 = Rd(b, fc + 16), W5 = Rd(b, fc + 20);
        u32 r3v = ((((w32 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (w32 & 0x1FFFFFFFu);
        u32 r7v = ((((w48 >> 20) & 0xFFFu) + 512u) & 0x1000u) + (w48 & 0x1FFFFE00u);
        r7v = (r7v & ~0x1FFu) | (W5 & 0x1FFu);
        const u32 r27v = (w28 & ~0x003FFC00u) | (W0 & 0x003FFC00u);
        const u32 r26v = (w40 & ~0x7FF80000u) | (W3 & 0x7FF80000u);
        r3v = (r3v & ~0x800u) | (W1 & 0x800u);
        u32 r10v = (w44 & ~0xFFFFFC03u) | (W4 & 0xFFFFFC03u);
        const u32 r30v = (w44 >> 2) & 0xFu;
        u32 r9 = G(b, dev + p.slot + 11694)[0];
        if (r30v > r9) r9 = r30v;
        r10v = (r10v & ~0x3Cu) | (Rotl(r9, 2) & 0x3Cu);
        u32 r8 = G(b, dev + p.slot + 11720)[0];
        const u32 r7b = (w44 >> 6) & 0xFu;
        if (r7b < r8) r8 = r7b;
        r10v = (r10v & ~0x3C0u) | (Rotl(r8, 6) & 0x3C0u);
        Wr(b, fc + 8, w36);
        Wr(b, fc + 0, r27v);
        Wr(b, fc + 16, r10v);
        Wr(b, fc + 20, r7v);
        Wr(b, fc + 12, r26v);
        Wr(b, fc + 4, r3v);
        Wr64(b, dev + 16, Rd64(b, dev + 16) | p.mask);
    }
    Wr(b, dev + p.r29o, f);
    if (p.release == 1) {
        Wr(b, p.old + 8, p.fence);
    } else if (p.release == 2) {
        const u32 r1pp = r1p - 160;
        const u32 word0 = (p.stackword & 0x80000000u) | (p.old >> 2);
        Wr(b, r1pp + 84, 0xFFFFFFFFu);
        Wr(b, r1pp + 80, word0);
        Wr64(b, p.wp, (u64(word0) << 32) | 0xFFFFFFFFull);
        Wr(b, dev + 13168, p.wp + 8);
    }
    Wr(b, p.tbl + p.r27, ctx.r3.u32);
}
// ---- verify helpers: snapshot of guest regions ----
struct Region { u32 a, n; std::vector<u8> before, native; };
inline void Snap(u8* b, std::vector<Region>& rs) { for (auto& r : rs) r.before.assign(G(b, r.a), G(b, r.a) + r.n); }
inline void Keep(u8* b, std::vector<Region>& rs) { for (auto& r : rs) r.native.assign(G(b, r.a), G(b, r.a) + r.n); }
inline void Restore(u8* b, std::vector<Region>& rs) { for (auto& r : rs) std::memcpy(G(b, r.a), r.before.data(), r.n); }
inline int Diff(u8* b, std::vector<Region>& rs) {
    for (size_t i = 0; i < rs.size(); ++i)
        if (std::memcmp(G(b, rs[i].a), rs[i].native.data(), rs[i].n) != 0) return int(i);
    return -1;
}
std::atomic<uint64_t> t_calls{0}, t_bad{0}, t_equal{0}, t_native{0}, t_fallback{0}, c_calls{0}, c_bad{0};
void Stats() {
    static std::atomic<int64_t> last{0};
    const int64_t now = int64_t(GetTickCount64());
    int64_t l = last.load();
    if (now - l > 10000 && last.compare_exchange_strong(l, now))
        REXLOG_INFO("NATIVE TEX VERIFY: set texture {} compared, {} differ (fast {} unchanged, {} native, {} original) | "
                    "VS constants {} compared, {} differ", t_calls.load(), t_bad.load(), t_equal.load(),
                    t_native.load(), t_fallback.load(), c_calls.load(), c_bad.load());
}
}  // namespace native_tex

bool sr_native_settex(PPCContext& ctx, uint8_t* base) {
    using namespace native_tex;
    const int mode = Mode();
    if (mode == 0) return false;
    Plan p;
    if (!MakePlan(ctx, base, p)) { if (mode == 2) ++t_fallback; return false; }
    if (mode == 2) {
        if (p.equal) ++t_equal; else ++t_native;
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            std::vector<Region> rs;
            rs.push_back({p.r1 - 512, 512});
            rs.push_back({p.tbl + p.r27, 4});
            if (!p.equal) {
                rs.push_back({p.dev, 13184});
                if (p.release == 1) rs.push_back({p.old + 8, 4});
                if (p.release == 2) rs.push_back({p.wp, 8});
            }
            Snap(base, rs);
            const PPCContext saved = ctx;
            Apply(ctx, base, p);
            Keep(base, rs);
            Restore(base, rs);
            __imp__sub_8214BE00(ctx, base);
            const int d = Diff(base, rs);
            ++t_calls;
            if (d >= 0 && t_bad.fetch_add(1) < 16) {
                const Region& r = rs[size_t(d)];
                u32 off = 0;
                while (off < r.n && G(base, r.a)[off] == r.native[off]) ++off;
                REXLOG_INFO("NATIVE TEX differs: region {} ({:08X}+{}) at +{}: original {:02X} native {:02X} | equal {} "
                            "release {} anim {} slot {} r1 {:08X}", d, r.a, r.n, off, G(base, r.a)[off], r.native[off],
                            p.equal, p.release, p.anim, p.slot, saved.r1.u32);
            }
            Stats();
            return true;
        }
    }
    Apply(ctx, base, p);
    return true;
}

bool sr_native_vsconst(PPCContext& ctx, uint8_t* base) {
    using namespace native_tex;
    const int mode = Mode();
    if (mode == 0) return false;
    const u32 dev = ctx.r3.u32, src = ctx.r5.u32, n = ctx.r6.u32, r1 = ctx.r1.u32;
    if (n > 512) return false;
    const u32 dst = u32(u64(u32((ctx.r4.u64 + 112) << 4) & 0xFFFFFFF0u) + ctx.r3.u64);
    auto run = [&] {
        Wr(base, r1 - 32, dev);
        Wr64(base, r1 + 48, ctx.r7.u64);
        for (u32 i = 0; i < n; ++i) {
            u8 tmp[16];
            for (u32 k = 0; k < 16; k += 4) std::memcpy(tmp + k, G(base, src + i * 16 + k), 4);
            std::memcpy(G(base, (dst + i * 16) & ~15u), tmp, 16);
        }
        Wr64(base, dev, Rd64(base, dev) | ctx.r7.u64);
    };
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            std::vector<Region> rs;
            rs.push_back({r1 - 32, 4});
            rs.push_back({r1 + 48, 8});
            rs.push_back({dev, 8});
            if (n) rs.push_back({dst & ~15u, n * 16});
            Snap(base, rs);
            run();
            Keep(base, rs);
            Restore(base, rs);
            __imp__sub_825D0D80(ctx, base);
            ++c_calls;
            if (Diff(base, rs) >= 0 && c_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE VSCONST differs: dev {:08X} reg {} src {:08X} n {}", dev, ctx.r4.u32, src, n);
            Stats();
            return true;
        }
    }
    run();
    return true;
}

// NATIVE FETCH-CONSTANT FLUSH. 825DB468(r3 device, r4 dirty mask): for every run of set bits (MSB first; bit k =
// fetch constant k, 24 bytes at device +1024 + 24k) writes 0x80000000 at wp+4, aligns wp to 8 (wp += wp & 7), a
// type-0 header ((6n-1) << 16 | 0x4800 + 6k) and the 6n words; then two 8-byte words 0x0002500000000000 and
// wp += 16 (device +40 write pointer, +44 limit). When a run doesn't fit, the original (which grows the buffer
// through 825DAF48 / 825D38C0) runs instead. Also its stack writes (savegprlr_26 + stwu -144). Mask 0 ->
// original. Same switches as the set texture (native_tex_off / native_tex_verify).
extern "C" void __imp__sub_825DB468(PPCContext& ctx, uint8_t* base);
namespace native_tex {
std::atomic<uint64_t> f_calls{0}, f_bad{0}, f_fallback{0};
// dry = true: only checks the fit and returns the final write pointer (0 = doesn't fit).
inline u32 FetchFlush(u8* b, u32 dev, u64 m, bool dry) {
    u32 wp = Rd(b, dev + 40);
    const u32 limit = Rd(b, dev + 44);
    u32 src = dev + 1020, reg = 18432;
    while (m) {
        const int lz = __builtin_clzll(m);
        m <<= lz;
        src += u32(lz) * 24;
        const u32 r27 = reg + u32(lz) * 6;
        const int ones = (~m == 0) ? 64 : __builtin_clzll(~m);
        const u32 r29 = u32(ones) * 6;
        if (!(wp + (r29 + 5) * 4 < limit)) return 0;
        if (!dry) Wr(b, wp + 4, 0x80000000u);
        wp += wp & 7;
        if (!dry) Wr(b, wp + 4, (((r29 - 1) << 16) & 0xFFFF0000u) | r27);
        wp += 4;
        reg = r29 + r27;
        for (int i = 0; i < ones; ++i) {
            if (!dry) {
                u8 tmp[24];
                std::memcpy(tmp, G(b, src + 4), 24);
                std::memcpy(G(b, wp + 4), tmp, 24);
            }
            src += 24;
            wp += 24;
        }
        m = ones >= 64 ? 0 : (m << ones);
    }
    if (!dry) {
        Wr64(b, wp + 4, 0x0002500000000000ull);
        Wr64(b, wp + 12, 0x0002500000000000ull);
        Wr(b, dev + 40, wp + 16);
    }
    return wp + 16;
}
inline void FetchFlushFrame(PPCContext& ctx, u8* b) {
    const u32 r1 = ctx.r1.u32;
    Wr64(b, r1 - 56, ctx.r26.u64); Wr64(b, r1 - 48, ctx.r27.u64); Wr64(b, r1 - 40, ctx.r28.u64);
    Wr64(b, r1 - 32, ctx.r29.u64); Wr64(b, r1 - 24, ctx.r30.u64); Wr64(b, r1 - 16, ctx.r31.u64);
    Wr(b, r1 - 8, u32(ctx.lr));
    Wr(b, r1 - 144, r1);
}
}  // namespace native_tex

PPC_FUNC(sub_825DB468) {
    using namespace native_tex;
    const int mode = Mode();
    const u32 dev = ctx.r3.u32;
    const u64 mask = ctx.r4.u64;
    if (mode == 0 || mask == 0) { __imp__sub_825DB468(ctx, base); return; }
    const u32 end = FetchFlush(base, dev, mask, true);
    if (!end) { if (mode == 2) ++f_fallback; __imp__sub_825DB468(ctx, base); return; }
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            const u32 wp0 = Rd(base, dev + 40);
            std::vector<Region> rs;
            rs.push_back({ctx.r1.u32 - 144, 144});
            rs.push_back({dev + 40, 4});
            rs.push_back({wp0, end - wp0 + 4});
            Snap(base, rs);
            FetchFlushFrame(ctx, base);
            FetchFlush(base, dev, mask, false);
            Keep(base, rs);
            Restore(base, rs);
            __imp__sub_825DB468(ctx, base);
            const uint64_t c = ++f_calls;
            const int d = Diff(base, rs);
            if (d >= 0 && f_bad.fetch_add(1) < 12) {
                const Region& r = rs[size_t(d)];
                u32 off = 0;
                while (off < r.n && G(base, r.a)[off] == r.native[off]) ++off;
                REXLOG_INFO("NATIVE FETCH FLUSH differs: region {} at +{} mask {:016X} wp {:08X}", d, off, mask, wp0);
            }
            if ((c & 16383) == 0)
                REXLOG_INFO("NATIVE FETCH FLUSH VERIFY: {} compared, {} differ, {} did not fit (original)", c, f_bad.load(),
                            f_fallback.load());
            return;
        }
    }
    FetchFlushFrame(ctx, base);
    FetchFlush(base, dev, mask, false);
    ctx.r4.u64 = end - 16;  // the original leaves its write pointer in r4
}

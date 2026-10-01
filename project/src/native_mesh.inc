// 23. NATIVE MESH DRAW (2026-09-30, builds nm1/nm2). Hand translation of 8223E8D8, the per-mesh draw of
//    every render instance (shader lookup per pass, constant dirty ranges, material float constants,
//    sampler states, textures, shader passes, DrawIndexed). Same child calls in the same order with
//    the same arguments and return addresses, same 496-byte guest stack frame; only the body is
//    native (plain loads instead of volatile byte-swapped ones, locals in host registers).
//    dist\native_mesh        = native body live
//    dist\native_mesh_verify = REPLAY CHECK (nm2; never touches live memory): on 1 in 256 calls per
//        thread the ORIGINAL runs for real while every direct child call is recorded (arguments,
//        return values, device/render struct/decl cache/stack block before and after). Then the
//        native body replays against PRIVATE copies of those regions: child calls are compared to the
//        recording instead of running, their recorded effects are applied to the copies, and the state
//        before every child call and at the end is compared. Log "NATIVE MESH VERIFY".
//        (nm1 ran both for real and rolled live memory back: stretched textures + freeze. Never again.)
//    dist\native_mesh_ab     = native on/off every 10 s; "NATIVE MESH A/B" = us per mesh draw.
//    Included from render_fixes.cpp right before the section-19 hook of 8223E8D8.
namespace native_mesh {
using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

// ---- replay state (thread-local; only used by verify) ----
constexpr int kRegions = 4;           // device, render struct, decl cache, stack frame
constexpr u32 kDevSize = 13312, kRenSize = 5760, kDeclSize = 16, kFrameSize = 560;
constexpr u32 kCmpLo = 128, kCmpHi = 336;  // stack bytes compared (child argument blocks; 336 = saved f31)
struct Rec {
    u32 lr; u64 r[8]; u64 f1, f2; u64 ret3, retf1; size_t pre, post;  // offsets into the snapshot arena
};
struct ReplayState {
    bool recording = false; int depth = 0;
    u32 addr[kRegions] = {}, size[kRegions] = {};
    std::vector<u8> arena;     // snapshots: entry, per child pre/post, final
    std::vector<Rec> recs;
    size_t entry = 0, final_snap = 0; u64 final_r3 = 0;
    bool replaying = false, diverged = false, indirect_in_segment = false;
    size_t k = 0;
    u8 priv[kDevSize + kRenSize + kDeclSize + kFrameSize];
    u8 dummy[64];
    uint64_t writes_outside = 0;
    u32 dbgP = 0, dbgPass = 0, dbgSpass = 0;
};
thread_local ReplayState rp;
inline u32 RegionOff(int r) { return r == 0 ? 0 : r == 1 ? kDevSize : r == 2 ? kDevSize + kRenSize : kDevSize + kRenSize + kDeclSize; }
constexpr u32 kTotal = kDevSize + kRenSize + kDeclSize + kFrameSize;
inline u8* Map(u32 a) {
    for (int r = 0; r < kRegions; ++r)
        if (a - rp.addr[r] < rp.size[r]) return rp.priv + RegionOff(r) + (a - rp.addr[r]);
    return nullptr;
}

template <bool R> inline u8* H(u8* b, u32 a) {
    if constexpr (R) { if (u8* p = Map(a)) return p; }
    return b + a + REX_PHYS_HOST_OFFSET(a);
}
template <bool R> inline u8* HW(u8* b, u32 a) {  // write target
    if constexpr (R) {
        if (u8* p = Map(a)) return p;
        ++rp.writes_outside;
        return rp.dummy;  // replay never writes live memory
    }
    return b + a + REX_PHYS_HOST_OFFSET(a);
}
template <bool R> inline u32 L32(u8* b, u32 a) { u32 v; std::memcpy(&v, H<R>(b, a), 4); return __builtin_bswap32(v); }
template <bool R> inline u16 L16(u8* b, u32 a) { u16 v; std::memcpy(&v, H<R>(b, a), 2); return __builtin_bswap16(v); }
template <bool R> inline u8 L8(u8* b, u32 a) { return *H<R>(b, a); }
template <bool R> inline u64 L64(u8* b, u32 a) { u64 v; std::memcpy(&v, H<R>(b, a), 8); return __builtin_bswap64(v); }
template <bool R> inline void S32(u8* b, u32 a, u32 v) { v = __builtin_bswap32(v); std::memcpy(HW<R>(b, a), &v, 4); }
template <bool R> inline void S64(u8* b, u32 a, u64 v) { v = __builtin_bswap64(v); std::memcpy(HW<R>(b, a), &v, 8); }
template <bool R> inline void S8(u8* b, u32 a, u8 v) { *HW<R>(b, a) = v; }
template <bool R> inline float LF(u8* b, u32 a) { const u32 v = L32<R>(b, a); float f; std::memcpy(&f, &v, 4); return f; }
template <bool R> inline void SF(u8* b, u32 a, float f) { u32 v; std::memcpy(&v, &f, 4); S32<R>(b, a, v); }
template <bool R> inline void CopyF(u8* b, u32 dst, u32 src) {  // lfs + stfs
    u32 v; std::memcpy(&v, H<R>(b, src), 4); std::memcpy(HW<R>(b, dst), &v, 4);
}
inline u64 Srad(u64 v, u64 n) { n &= 127; return n > 63 ? u64(int64_t(v) >> 63) : u64(int64_t(v) >> n); }
inline u64 Srd(u64 v, u64 n) { n &= 127; return n > 63 ? 0 : v >> n; }
inline u64 Sx16(u16 v) { return u64(int64_t(int16_t(v))); }
constexpr u64 kTop = 0x8000000000000000ull;
// Dirty bits (one per 4 float registers) for registers start .. start+count-1, as the game builds them.
inline u64 RangeMask(u64 start, u64 count) {
    const u32 first = u32(start) >> 2;
    const u32 last = u32(start + count - 1) >> 2;
    return Srd(Srad(kTop, u32(u64(last) - u64(first))), first);
}

// ---- verify statistics ----
struct VerifyStats {
    std::mutex m;
    uint64_t samples = 0, clean = 0, diverged = 0, arg_bad = 0, seg_bad = 0, seg_bad_indirect = 0, final_bad = 0,
             ret_bad = 0, outside = 0, skipped = 0, children = 0;
    std::map<std::string, uint64_t> by_kind, seg_offs, seg_offs_sampler, final_offs, args_detail;
    std::vector<std::string> examples;
    std::chrono::steady_clock::time_point next{};
} vs;
void Example(const std::string& e) { std::lock_guard l(vs.m); if (vs.examples.size() < 30) vs.examples.push_back(e); }
inline size_t Alloc() { const size_t at = rp.arena.size(); rp.arena.resize(at + kTotal); return at; }
inline void Snap(u8* base, size_t at) {  // live regions -> arena
    for (int r = 0; r < kRegions; ++r)
        std::memcpy(rp.arena.data() + at + RegionOff(r), base + rp.addr[r] + REX_PHYS_HOST_OFFSET(rp.addr[r]), rp.size[r]);
}
const char* RegionName(int r) { return r == 0 ? "device" : r == 1 ? "render struct" : r == 2 ? "decl cache" : "stack"; }
// Private copy vs a snapshot; returns the number of differing 4-byte words, fills the first few.
int CompareTo(size_t at, std::string* detail, std::map<std::string, uint64_t>* offs = nullptr) {
    int bad = 0;
    for (int r = 0; r < kRegions; ++r) {
        const u32 lo = r == 3 ? kCmpLo : 0, hi = r == 3 ? kCmpHi : rp.size[r];
        const u8* a = rp.priv + RegionOff(r);
        const u8* b = rp.arena.data() + at + RegionOff(r);
        if (!std::memcmp(a + lo, b + lo, hi - lo)) continue;
        for (u32 o = lo; o < hi; o += 4) {
            if (std::memcmp(a + o, b + o, 4)) {
                if (detail && bad < 4) {
                    u32 x, y; std::memcpy(&x, a + o, 4); std::memcpy(&y, b + o, 4);
                    *detail += fmt::format(" {}+{} native {:08X} orig {:08X}", RegionName(r), o, __builtin_bswap32(x), __builtin_bswap32(y));
                }
                ++bad;
                if (offs) (*offs)[fmt::format("{}+{}", RegionName(r), o)]++;
            }
        }
    }
    return bad;
}
// Argument registers per call site (bit 0..7 = r3..r10, bit 8 = f1, bit 9 = f2).
u32 ArgMask(u32 lr) {
    switch (lr) {
    case 0x8223E948: case 0x8223E964: case 0x8223F1C0: case 0x8223F350: case 0x8223F35C: case 0x8223F408:
    case 0x8223F624: return 0x3;
    case 0x8223E9E8: case 0x8223EB1C: case 0x8223F1F8: case 0x8223F454: case 0x8223F6B8: return 0x1;
    case 0x8223EA60: case 0x8223EADC: case 0x8223EB04: case 0x8223F6A0: return 0xF;
    case 0x8223EB54: return 0x30F;
    case 0x8223EDA0: case 0x8223EEB8: case 0x8223EF54: case 0x8223F070: case 0x8223F244: case 0x8223F258:
    case 0x8223F3C4: return 0x1F;
    case 0x8223F228: return 0x7;
    case 0x8223F2A0: return 0x101;
    case 0x8223F33C: return 0xFF;
    case 0x8223F368: return 0x100;
    default: return 0;
    }
}
inline PPCRegister& Gpr(PPCContext& c, int i) {
    switch (i) { case 0: return c.r3; case 1: return c.r4; case 2: return c.r5; case 3: return c.r6;
                 case 4: return c.r7; case 5: return c.r8; case 6: return c.r9; default: return c.r10; }
}
// Recording (original run): called by the child wrappers (SR_RT_TIMED, 825D2478 hook).
inline bool RecActive(PPCContext& ctx) {
    return rp.recording && rp.depth == 0 && u32(ctx.lr) - 0x8223E8D8u < 0xE00u;
}
void RecBefore(PPCContext& ctx, u8* base) {
    rp.depth = 1;
    if (rp.recs.size() >= 600) return;  // absurd; replay will report divergence
    Rec r{};
    r.lr = u32(ctx.lr);
    for (int i = 0; i < 8; ++i) r.r[i] = Gpr(ctx, i).u64;
    r.f1 = ctx.f1.u64; r.f2 = ctx.f2.u64;
    r.pre = Alloc(); Snap(base, r.pre);
    rp.recs.push_back(r);
}
void RecAfter(PPCContext& ctx, u8* base) {
    rp.depth = 0;
    if (rp.recs.empty()) return;
    Rec& r = rp.recs.back();
    r.ret3 = ctx.r3.u64; r.retf1 = ctx.f1.u64;
    r.post = Alloc(); Snap(base, r.post);
}
// Replay (native body against the private copies).
void ReplayCall(PPCContext& ctx, u8* base, u32 lr) {
    if (lr == 0x8223E958) {  // 82702900 = memcpy(r3, r4, r5): not recorded, done on the copies
        if (!rp.diverged)
            for (u32 i = 0; i < u32(ctx.r5.u64); ++i) *HW<true>(base, ctx.r3.u32 + i) = *H<true>(base, ctx.r4.u32 + i);
        return;
    }
    if (rp.diverged) { ctx.r3.u64 = 0; ctx.f1.u64 = 0; return; }
    if (rp.k >= rp.recs.size() || rp.recs[rp.k].lr != lr) {
        rp.diverged = true;
        Example(fmt::format("DIVERGED at child #{}: native called from {:08X}, original from {:08X}", rp.k, lr,
                            rp.k < rp.recs.size() ? rp.recs[rp.k].lr : 0));
        ctx.r3.u64 = 0; ctx.f1.u64 = 0;
        return;
    }
    const Rec& r = rp.recs[rp.k++];
    const u32 mask = ArgMask(lr);
    std::string d;
    for (int i = 0; i < 8; ++i)
        if ((mask >> i & 1) && Gpr(ctx, i).u64 != r.r[i]) d += fmt::format(" r{} native {:X} orig {:X}", i + 3, Gpr(ctx, i).u64, r.r[i]);
    if ((mask >> 8 & 1) && ctx.f1.u64 != r.f1) d += fmt::format(" f1 native {:X} orig {:X}", ctx.f1.u64, r.f1);
    if ((mask >> 9 & 1) && ctx.f2.u64 != r.f2) d += fmt::format(" f2 native {:X} orig {:X}", ctx.f2.u64, r.f2);
    if (!d.empty()) {
        { std::lock_guard l(vs.m); ++vs.arg_bad; vs.by_kind[fmt::format("args@{:08X}", lr)]++;
          std::string dk = fmt::format("{:08X}", lr);
          for (int i = 0; i < 8; ++i) if ((mask >> i & 1) && Gpr(ctx, i).u64 != r.r[i]) dk += fmt::format(" r{}", i + 3);
          vs.args_detail[dk]++; }
        Example(fmt::format("ARGS at {:08X}:{} (P {:08X} pass {} spass {})", lr, d, rp.dbgP, rp.dbgPass, rp.dbgSpass));
    }
    std::string sd;
    std::map<std::string, uint64_t> offs;
    if (CompareTo(r.pre, &sd, &offs)) {
        if (!offs.empty()) {
            std::lock_guard l(vs.m);
            for (auto& [k, v] : offs) {
                if (!rp.indirect_in_segment) { vs.seg_offs[fmt::format("{:08X} {}", lr, k)] += v; continue; }
                // After sampler calls: device+16 (dirty bits) and the fetch constants (+1024..+1791) are expected.
                int off = -1;
                if (k.rfind("device+", 0) == 0) off = std::atoi(k.c_str() + 7);
                if (off == 16 || off == 20 || (off >= 1024 && off < 1792)) continue;
                vs.seg_offs_sampler[fmt::format("{:08X} {}", lr, k)] += v;
            }
        }
        {
            std::lock_guard l(vs.m);
            rp.indirect_in_segment ? ++vs.seg_bad_indirect : ++vs.seg_bad;
            vs.by_kind[fmt::format("state before {:08X}{}", lr, rp.indirect_in_segment ? " (after sampler calls)" : "")]++;
        }
        if (!rp.indirect_in_segment) Example(fmt::format("STATE before the call at {:08X}:{}", lr, sd));
    }
    std::memcpy(rp.priv, rp.arena.data() + r.post, kTotal);  // the child's recorded effects
    rp.indirect_in_segment = false;
    ctx.r3.u64 = r.ret3; ctx.f1.u64 = r.retf1;
}
#define NM_CALL(fn, ret) do { if constexpr (R) ReplayCall(ctx, base, (ret)); else { ctx.lr = (ret); fn(ctx, base); } } while (0)
template <bool R> inline void CallIndirect(PPCContext& ctx, u8* base, u32 target, u32 ret) {
    if constexpr (R) { rp.indirect_in_segment = true; (void)target; (void)ret; return; }
    else { ctx.lr = ret; REX_CALL_INDIRECT_FUNC(target); }
}

template <bool R>
void RunT(PPCContext& ctx, u8* base) {
    const u64 in_lr = ctx.lr, in_r3 = ctx.r3.u64, a7 = ctx.r7.u64;
    const u32 P = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, pass = ctx.r6.u32;
    const u32 r1 = ctx.r1.u32, sp = r1 - 496;
    S32<R>(base, sp, r1);  // stwu back chain
    ctx.r1.u32 = sp;
    const u32 spass = pass == 9 ? 0 : pass == 10 ? 1 : pass;  // shader pass (sp+540)
    S32<R>(base, sp + 540, spass); S32<R>(base, sp + 548, u32(a7)); S32<R>(base, sp + 92, pass);
    auto finish = [&](u64 ret) { ctx.r1.u32 = r1; ctx.r3.u64 = ret; ctx.lr = in_lr; };

    const u32 mesh = L32<R>(base, P + 24);
    NM_CALL(sub_826306A8, 0x8223E93C);
    ctx.r4.u64 = ctx.r3.u64; ctx.r3.u64 = sp + 256;
    NM_CALL(sub_82129CA0, 0x8223E948);
    ctx.r4.u64 = ctx.r3.u64; ctx.r3.u64 = sp + 176; ctx.r5.u64 = 68;
    NM_CALL(sub_82702900, 0x8223E958);
    ctx.r4.u64 = sp + 176; ctx.r3.u64 = sp + 256;
    NM_CALL(sub_82129CA0, 0x8223E964);
    {
        const u32 vi = L32<R>(base, mesh + 8);
        if (L32<R>(base, vi + 4) == 0 || L16<R>(base, vi + 14) == 0) { finish(0); return; }
    }
    const u32 tls = L32<R>(base, ctx.r13.u32 + 0);
    u32 S = L32<R>(base, tls + 7692);
    if (L8<R>(base, S + 4248) != 0 && (L16<R>(base, mesh + 0) & 0x10)) { finish(0); return; }
    if (spass != 0 && u32(a7) != 0 && spass != 8) {
        if ((L16<R>(base, mesh + 0) & 0x300 & L32<R>(base, u32(a7) + 8)) == 0) { finish(0); return; }
    }
    {
        const u32 id = L32<R>(base, P + 40);
        if (id != 0xFFFFFFFFu) { ctx.r3.u64 = id; NM_CALL(sub_8219A128, 0x8223E9E8); }
    }
    {
        const u32 pos = L32<R>(base, P + 8);
        ctx.fpscr.disableFlushMode();
        SF<R>(base, sp + 128, LF<R>(base, pos + 0) + LF<R>(base, a5 + 0));
        SF<R>(base, sp + 132, LF<R>(base, pos + 4) + LF<R>(base, a5 + 4));
        SF<R>(base, sp + 136, LF<R>(base, pos + 8) + LF<R>(base, a5 + 8));
        SF<R>(base, sp + 144, LF<R>(base, a4 + 0) + LF<R>(base, pos + 0));
        SF<R>(base, sp + 148, LF<R>(base, pos + 4) + LF<R>(base, a4 + 4));
        SF<R>(base, sp + 152, LF<R>(base, pos + 8) + LF<R>(base, a4 + 8));
        ctx.r3.u64 = sp + 160; ctx.r4.u64 = sp + 144; ctx.r5.u64 = sp + 128; ctx.r6.u64 = sp + 256;
        NM_CALL(sub_826446F8, 0x8223EA60);
    }
    ctx.fpscr.disableFlushMode();
    const double f31 = double(float(std::sqrt(ctx.f1.f64)));
    {
        const u32 vi = L32<R>(base, mesh + 8);
        const u32 X = L32<R>(base, tls + 7696);
        const u32 idx = u32(Sx16(L16<R>(base, vi + 20))) * 5u + L16<R>(base, vi + 18);
        const u32 decl = L32<R>(base, L32<R>(base, S + 3948) + (idx << 2));
        if (decl != L32<R>(base, X + 4)) {
            const u32 D = L32<R>(base, tls + 7688);
            S32<R>(base, D + 11552, decl);
            S64<R>(base, D + 16, L64<R>(base, D + 16) | (1ull << 51));
            S32<R>(base, X + 4, decl);
        }
        ctx.r6.u64 = 0; ctx.r3.u64 = 0; ctx.r5.u64 = L16<R>(base, vi + 16); ctx.r4.u64 = L32<R>(base, vi + 24);
        NM_CALL(sub_82182610, 0x8223EADC);
    }
    S = L32<R>(base, tls + 7692);
    u8 wt = 1;  // sp+80: world transform set up (render struct +5450 == 1)
    if (L8<R>(base, S + 5450) == 1) {
        ctx.r6.u64 = 0; ctx.r4.u64 = L32<R>(base, P + 12); ctx.r5.u64 = 0; ctx.r3.u64 = L32<R>(base, P + 8);
        NM_CALL(sub_82631950, 0x8223EB04);
    } else {
        wt = 0;
    }
    S8<R>(base, sp + 80, wt);
    ctx.r3.u64 = L32<R>(base, L32<R>(base, mesh + 8) + 28);
    NM_CALL(sub_821825C8, 0x8223EB1C);
    S = L32<R>(base, tls + 7692);
    constexpr u32 kFadeRef = 0x827AC214;  // r31 (sp+100)
    if (L8<R>(base, S + 5450) == 1) {
        ctx.r6.u64 = 0; ctx.f1.f64 = double(LF<R>(base, P + 44)); ctx.r5.u64 = a7; ctx.r4.u64 = L32<R>(base, P + 12);
        ctx.f2.f64 = double(LF<R>(base, kFadeRef)); ctx.r3.u64 = L32<R>(base, P + 8);
        NM_CALL(sub_8223A720, 0x8223EB54);
    }
    constexpr u32 kFlags = 0x8370DC0F;       // r23: +0 texture override flag, +49 / +50 shader remap flags
    constexpr u32 kPrimTable = 0x8203C460;   // r14
    constexpr u32 kConstTable = 0x827D6B08;  // r26: 8 bytes per constant-set entry
    constexpr u32 kRemapA = 0x82EFA0CC, kRemapB = 0x82EFA1E0, kRemapC = 0x82EFA1F4;
    const u32 nsub = L16<R>(base, mesh + 2);
    for (u32 i = 0; i < nsub; ++i) {
        S32<R>(base, sp + 84, i);
        const u32 sm = L32<R>(base, mesh + 4) + i * 8;
        const u32 mt = L32<R>(base, L32<R>(base, mesh + 12) + 4) + u32(L16<R>(base, sm + 6)) * 56;
        bool check_mask;
        if (spass == 3) check_mask = L8<R>(base, P + 56) == 0;
        else check_mask = int32_t(spass) <= 5;
        if (check_mask) {
            const u32 bit = spass < 32 ? (1u << spass) : 0u;  // slw 1, r15
            if ((u32(Sx16(L16<R>(base, mt + 18))) & bit) == 0) continue;
        }
        u32 sid = L32<R>(base, mt + 0);
        {
            u32 remap = 0;
            if (L8<R>(base, kFlags + 49) != 0) remap = kRemapC;
            else if (L8<R>(base, kFlags + 50) != 0) remap = kRemapB;
            else if (L8<R>(base, P + 56) != 0 && spass == 3) remap = kRemapA;
            if (remap) {
                const u32 ent = L32<R>(base, L32<R>(base, tls + 7692) + 4280) + sid * 12;
                const u32 v = L32<R>(base, remap + (L32<R>(base, ent + 4) << 2));
                if (v != 0xFFFFFFFFu) sid = v;
            }
        }
        S = L32<R>(base, tls + 7692);
        const u32 sid12 = sid * 12;  // r18
        {
            const int32_t npasses = int16_t(L16<R>(base, L32<R>(base, L32<R>(base, S + 4280) + sid12 + 8) + 16));
            if (int32_t(spass) >= npasses || int32_t(spass) < 0) continue;
        }
        const u32 P60 = L32<R>(base, P + 60);
        const u32 E = P60 + i * 84;
        // Vertex shader constants: per-instance block or the material's.
        if (wt && P60 && L16<R>(base, E + 8) != 0) {
            const u32 D = L32<R>(base, tls + 7688);
            S64<R>(base, D + 0, L64<R>(base, D + 0) & ~RangeMask(L16<R>(base, E + 10), L16<R>(base, E + 8)));
            ctx.r3.u64 = D; ctx.r4.u64 = 0; ctx.r7.u64 = L16<R>(base, E + 8); ctx.r6.u64 = L32<R>(base, E + 4);
            ctx.r5.u64 = L16<R>(base, E + 10);
            NM_CALL(sub_825DAE10, 0x8223EDA0);
        } else {
            const u32 blk = mt + 28;
            const u16 n = L16<R>(base, blk + 0);
            if (n) {
                const u32 D = L32<R>(base, tls + 7688);
                const u32 arr = L32<R>(base, blk + 4);
                u64 dirty = L64<R>(base, D + 0);
                for (u32 j = 0; j < n; ++j) {
                    const u32 e = arr + j * 24;
                    const u64 reg = Sx16(L16<R>(base, e + 4));
                    const u32 o = D + (u32(reg) << 4);
                    CopyF<R>(base, D + (u32(reg + 112) << 4), e + 8);
                    CopyF<R>(base, o + 1796, e + 12); CopyF<R>(base, o + 1800, e + 16); CopyF<R>(base, o + 1804, e + 20);
                    dirty |= Srd(kTop, u32(reg) >> 2);
                    S64<R>(base, D + 0, dirty);  // the game stores every iteration
                }
            }
            const u16 m = L16<R>(base, blk + 2);
            for (u32 j = 0; j < m; ++j) {
                const u64 k = Sx16(L16<R>(base, L32<R>(base, blk + 8) + j * 2));
                const u32 ent = kConstTable + (u32(k) << 3);
                const u32 v = L32<R>(base, S + (u32(k + 1073) << 2));
                if (L32<R>(base, ent + 4) & 2) {
                    const u64 r4 = Sx16(L16<R>(base, ent + 2)), r6 = Sx16(L16<R>(base, ent + 0));
                    ctx.r3.u64 = L32<R>(base, tls + 7688); ctx.r4.u64 = r4; ctx.r5.u64 = v; ctx.r6.u64 = r6;
                    ctx.r7.u64 = RangeMask(r4, r6);
                    NM_CALL(sub_825D0D80, 0x8223EEB8);
                }
            }
        }
        // Pixel shader constants.
        if (wt && P60 && L16<R>(base, E + 16) != 0) {
            const u32 D = L32<R>(base, tls + 7688);
            S64<R>(base, D + 8, L64<R>(base, D + 8) & ~RangeMask(L16<R>(base, E + 18), L16<R>(base, E + 16)));
            ctx.r3.u64 = D; ctx.r4.u64 = 1; ctx.r7.u64 = L16<R>(base, E + 16); ctx.r6.u64 = L32<R>(base, E + 12);
            ctx.r5.u64 = L16<R>(base, E + 18);
            NM_CALL(sub_825DAE10, 0x8223EF54);
        } else {
            const u32 blk = mt + 40;
            const u16 n = L16<R>(base, blk + 0);
            if (n) {
                const u32 D = L32<R>(base, tls + 7688);
                const u32 arr = L32<R>(base, blk + 4);
                u64 dirty = L64<R>(base, D + 8);
                for (u32 j = 0; j < n; ++j) {
                    const u32 e = arr + j * 24;
                    const u64 reg = Sx16(L16<R>(base, e + 4));
                    const u32 o = D + (u32(reg) << 4);
                    CopyF<R>(base, D + (u32(reg + 368) << 4), e + 8);
                    CopyF<R>(base, o + 5892, e + 12); CopyF<R>(base, o + 5896, e + 16); CopyF<R>(base, o + 5900, e + 20);
                    dirty |= Srd(kTop, u32(reg) >> 2);
                    S64<R>(base, D + 8, dirty);
                }
            }
            const u16 m = L16<R>(base, blk + 2);
            if (m) {
                const u32 S2 = L32<R>(base, tls + 7692);
                for (u32 j = 0; j < m; ++j) {
                    const u64 k = Sx16(L16<R>(base, L32<R>(base, blk + 8) + j * 2));
                    const u32 ent = kConstTable + (u32(k) << 3);
                    const u32 v = L32<R>(base, S2 + (u32(k + 1073) << 2));
                    if (L32<R>(base, ent + 4) & 1) {
                        const u64 r4 = Sx16(L16<R>(base, ent + 2)), r6 = Sx16(L16<R>(base, ent + 0));
                        ctx.r3.u64 = L32<R>(base, tls + 7688); ctx.r4.u64 = r4; ctx.r5.u64 = v; ctx.r6.u64 = r6;
                        ctx.r7.u64 = RangeMask(r4, r6);
                        NM_CALL(sub_825D0E90, 0x8223F070);
                    }
                }
            }
        }
        // Sampler states (cached per sampler/state in the render struct, set via the device vtable).
        {
            const u16 n = L16<R>(base, mt + 8);
            for (u32 j = 0; j < n; ++j) {
                const u32 e = L32<R>(base, mt + 52) + j * 6;
                const u32 Rs = L32<R>(base, tls + 7692);
                const u64 s = Sx16(L16<R>(base, e + 0)), t = Sx16(L16<R>(base, e + 2)), v = Sx16(L16<R>(base, e + 4));
                const u32 slot = Rs + (u32(s * 13 + t + 768) << 2);
                if (L32<R>(base, slot) != u32(v)) {
                    S32<R>(base, slot, u32(v));
                    const u32 D = L32<R>(base, tls + 7688);
                    ctx.r3.u64 = D; ctx.r4.u64 = s; ctx.r5.u64 = v;
                    CallIndirect<R>(ctx, base, L32<R>(base, u32(D + t) + 440), 0x8223F0F4);
                }
            }
        }
        // Textures.
        {
            const u16 n = L16<R>(base, mt + 10);
            for (u32 j = 0; j < n; ++j) {
                const u32 e = L32<R>(base, mt + 12) + j * 12;
                const u64 t8 = Sx16(L16<R>(base, e + 8));
                u64 tex;
                if (L8<R>(base, kFlags + 0) != 0) {
                    tex = L32<R>(base, 0x827AA43C);
                } else {
                    u32 ov = 0xFFFFFFFFu;
                    if (wt && P60) ov = L32<R>(base, P60 + (u32(t8 + i * 21 + 5) << 2));
                    tex = ov != 0xFFFFFFFFu ? ov : L32<R>(base, e + 4);
                }
                ctx.r3.u64 = tex; ctx.r4.u64 = t8;
                NM_CALL(sub_8214BE00, 0x8223F1C0);
            }
        }
        // Extra constant sets (e.g. lighting) bound per material.
        {
            const int32_t n = int16_t(L16<R>(base, mt + 20));
            for (int32_t j = 0; j < n; ++j) {
                const u32 rec = L32<R>(base, mt + 24) + u32(j) * 8;
                ctx.r3.u64 = rec;
                NM_CALL(sub_8223C828, 0x8223F1F8);
                if (wt && L32<R>(base, L32<R>(base, mt + 24) + u32(j) * 8 + 4) == 1) {
                    ctx.r3.u64 = sp + 176; ctx.r4.u64 = L32<R>(base, P + 12);
                    ctx.r5.u64 = L32<R>(base, tls + 7692) + 4212;
                    NM_CALL(sub_8223BF20, 0x8223F228);
                    ctx.r7.u64 = 3ull << 59; ctx.r6.u64 = 3; ctx.r5.u64 = sp + 176; ctx.r4.u64 = 15;
                    ctx.r3.u64 = L32<R>(base, tls + 7688);
                    NM_CALL(sub_825D0D80, 0x8223F244);
                    ctx.r7.u64 = 3ull << 59; ctx.r6.u64 = 3; ctx.r5.u64 = sp + 176; ctx.r4.u64 = 15;  // r3 as left
                    NM_CALL(sub_825D0E90, 0x8223F258);
                }
            }
        }
        // LOD / fade level for the shader.
        u64 lod;
        {
            const u32 g = L32<R>(base, 0x827D0AE4);
            if (int32_t(g) >= 0) lod = g;
            else if (L8<R>(base, P + 33) == 255) {
                ctx.r3.u64 = sid; ctx.f1.f64 = f31;
                NM_CALL(sub_8223AD40, 0x8223F2A0);
                lod = ctx.r3.u64;
            } else lod = L8<R>(base, P + 33);
        }
        // Shader passes + the draw.
        {
            S = L32<R>(base, tls + 7692);
            const u32 sp540 = L32<R>(base, sp + 540);
            const u32 r29 = sp540 << 3;
            const int32_t npass = int32_t(L32<R>(base, L32<R>(base, L32<R>(base, L32<R>(base, S + 4280) + sid12 + 8) + 32) + r29));
            const u32 D0 = L32<R>(base, tls + 7688);
            const u32 r24 = (L32<R>(base, D0 + 10420) >> 2) & 1;
            if (npass > 0) {
                const u64 is10 = L32<R>(base, sp + 92) == 10 ? 1 : 0;
                for (int32_t k = 0; k < npass; ++k) {
                    int64_t r4 = int16_t(L16<R>(base, mt + 16));
                    if (int64_t(int32_t(u32(lod))) > int64_t(int32_t(u32(r4)))) r4 = int64_t(lod);
                    const u32 vi = L32<R>(base, mesh + 8);
                    ctx.r10.u64 = 0; ctx.r9.u64 = is10; ctx.r5.u64 = sp540; ctx.r6.u64 = u64(int64_t(k));
                    ctx.r3.u64 = sid; ctx.r7.u64 = Sx16(L16<R>(base, vi + 20)); ctx.r8.u64 = L16<R>(base, vi + 18);
                    ctx.r4.u64 = u64(r4);
                    NM_CALL(sub_8223AE00, 0x8223F33C);
                    if (k > 0) {
                        ctx.r4.u64 = 0; ctx.r3.u64 = 48; NM_CALL(sub_8219F258, 0x8223F350);
                        ctx.r4.u64 = 1; ctx.r3.u64 = 60; NM_CALL(sub_8219F258, 0x8223F35C);
                        ctx.f1.f64 = double(LF<R>(base, 0x8370DC44));
                        NM_CALL(sub_82631BF0, 0x8223F368);
                    }
                    if (L8<R>(base, 0x827D0AE9) != 0) {
                        const u16 cnt = L16<R>(base, sm + 4);
                        if (cnt) {
                            const u32 Rs = L32<R>(base, tls + 7692);
                            const int32_t div = int32_t(L32<R>(base, Rs + 3968));
                            const u32 add = L32<R>(base, Rs + 3964);
                            const u32 prim = L32<R>(base, Rs + 3960);
                            const int32_t q = div ? int32_t(cnt) / div : 0;
                            const u32 r9 = u32(q) + add;
                            const u32 mul = L32<R>(base, kPrimTable + (prim << 3));
                            const u32 off = L32<R>(base, kPrimTable + 4 + (prim << 3));
                            ctx.r5.u64 = 0; ctx.r6.u64 = L32<R>(base, sm + 0); ctx.r3.u64 = L32<R>(base, tls + 7688);
                            ctx.r4.u64 = prim;
                            ctx.r7.u64 = u64(int64_t(int32_t(r9)) * int64_t(int32_t(mul))) + off;
                            NM_CALL(sub_825D2478, 0x8223F3C4);
                        }
                    }
                    {
                        const u32 Rs = L32<R>(base, tls + 7692);
                        const u32 lst = L32<R>(base, L32<R>(base, L32<R>(base, L32<R>(base, Rs + 4280) + sid12 + 8) + 32) + r29 + 4);
                        if (L16<R>(base, lst + u32(k) * 8 + 2) != 0) NM_CALL(sub_8219F138, 0x8223F3F4);
                    }
                    if (k > 0) {
                        ctx.r4.u64 = r24; ctx.r3.u64 = 48; NM_CALL(sub_8219F258, 0x8223F408);
                        NM_CALL(sub_82631CF0, 0x8223F40C);
                    }
                }
            }
        }
        {
            const int32_t n = int16_t(L16<R>(base, mt + 20));
            if (n > 0) NM_CALL(sub_8223C0A0, 0x8223F430);
            for (int32_t j = 0; j < int32_t(int16_t(L16<R>(base, mt + 20))); ++j) {
                ctx.r3.u64 = L32<R>(base, mt + 24) + u32(j) * 8;
                NM_CALL(sub_8223CA08, 0x8223F454);
            }
        }
        // Sampler states back to 0.
        {
            const u16 n = L16<R>(base, mt + 8);
            for (u32 j = 0; j < n; ++j) {
                const u32 e = L32<R>(base, mt + 52) + j * 6;
                const u32 Rs = L32<R>(base, tls + 7692);
                const u64 s = Sx16(L16<R>(base, e + 0)), t = Sx16(L16<R>(base, e + 2));
                const u32 slot = Rs + (u32(s * 13 + t + 768) << 2);
                if (L32<R>(base, slot) != 0) {
                    const u32 D = L32<R>(base, tls + 7688);
                    S32<R>(base, slot, 0);
                    ctx.r3.u64 = D; ctx.r4.u64 = s; ctx.r5.u64 = 0;
                    CallIndirect<R>(ctx, base, L32<R>(base, u32(D + t) + 440), 0x8223F4D8);
                }
            }
        }
        // Per-instance constant ranges dirty again for the next user.
        if (wt) {
            const u32 p60 = L32<R>(base, P + 60);
            if (p60) {
                const u32 e2 = p60 + L32<R>(base, sp + 84) * 84;
                if (L16<R>(base, e2 + 8) != 0) {
                    const u32 D = L32<R>(base, tls + 7688);
                    S64<R>(base, D + 0, L64<R>(base, D + 0) | RangeMask(L16<R>(base, e2 + 10), L16<R>(base, e2 + 8)));
                }
            }
            const u32 p60b = L32<R>(base, P + 60);
            if (p60b) {
                const u32 e2 = p60b + L32<R>(base, sp + 84) * 84;
                if (L16<R>(base, e2 + 16) != 0) {
                    const u32 D = L32<R>(base, tls + 7688);
                    S64<R>(base, D + 8, L64<R>(base, D + 8) | RangeMask(L16<R>(base, e2 + 18), L16<R>(base, e2 + 16)));
                }
            }
        }
    }
    // After all submeshes: decal / projected extra pass.
    if (wt && L8<R>(base, 0x827D0AE9) != 0 && L32<R>(base, L32<R>(base, mesh + 8) + 24) != 0) {
        ctx.r4.u64 = pass; ctx.r3.u64 = L32<R>(base, P + 36);
        NM_CALL(sub_8223CC08, 0x8223F624);
        if ((ctx.r3.u32 & 0xFF) != 0 && L8<R>(base, kFadeRef + 18229) != 0 && L8<R>(base, 0x8370D725) == 0) {
            const u32 o = L32<R>(base, P + 36);
            const u32 n = L32<R>(base, o + 0);
            if (n != 0 && n <= 144) {
                const u32 r11 = 0x82F9AB90u + n * 28 - 28;
                if (r11 != 0 && L32<R>(base, r11 + 8) != 0) {
                    ctx.r6.u64 = nsub ? u64(u32(a7)) : a7;  // after the loop the game reloads it with lwz
                    ctx.r5.u64 = spass; ctx.r3.u64 = in_r3; ctx.r4.u64 = r11 + 8;
                    NM_CALL(sub_822432F0, 0x8223F6A0);
                }
            }
        }
    }
    if (L8<R>(base, L32<R>(base, tls + 7692) + 5450) == 1) {
        ctx.r3.u64 = 0;
        NM_CALL(sub_82631BB0, 0x8223F6B8);
    }
    finish(1);
}

void Run(PPCContext& ctx, u8* base) { RunT<false>(ctx, base); }

// ---- switches, A/B, verify ----
int Mode() {  // 0 off, 1 live, 2 verify
    static const int m = FileExists("native_mesh_verify") ? 2 : FileExists("native_mesh") ? 1 : 0;
    return m;
}
bool AbOn() { static const bool on = FileExists("native_mesh_ab"); return on; }
std::atomic<int> ab_mode{1};
std::atomic<uint64_t> ab_ticks[2], ab_calls[2];
std::chrono::steady_clock::time_point ab_next{};
std::mutex ab_mutex;
void AbTick() {  // once per frame (frame hook)
    if (!AbOn()) return;
    std::lock_guard lock(ab_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now < ab_next) return;
    const bool first = ab_next.time_since_epoch().count() == 0;
    ab_next = now + std::chrono::seconds(10);
    if (!first) {
        static uint64_t tot_t[2], tot_c[2];
        const double us = mesh_census::TickUs();
        std::string s;
        for (int m = 0; m < 2; ++m) {
            tot_t[m] += ab_ticks[m].exchange(0); tot_c[m] += ab_calls[m].exchange(0);
            s += fmt::format(" | {}: {} mesh draws, {:.3f} us each", m ? "NATIVE" : "original", tot_c[m],
                             tot_c[m] ? tot_t[m] * us / tot_c[m] : 0.0);
        }
        REXLOG_INFO("NATIVE MESH A/B (cumulative, inclusive of child calls){}", s);
    }
    ab_mode.store(ab_mode.load() ^ 1);
}
void MaybeLogLocked() {  // vs.m held
    const auto now = std::chrono::steady_clock::now();
    if (now < vs.next) return;
    const bool first = vs.next.time_since_epoch().count() == 0;
    vs.next = now + std::chrono::seconds(10);
    if (first) return;
    std::string s;
    for (auto& [k, v] : vs.by_kind) s += fmt::format(" [{}: {}]", k, v);
    REXLOG_INFO("NATIVE MESH VERIFY (replay): {} samples, {} CLEAN, {} child calls checked | diverged {}, bad args {}, "
                "bad state {} (+{} right after sampler calls), bad final state {}, bad r3 {}, writes outside {}, "
                "skipped {} |{}", vs.samples, vs.clean, vs.children, vs.diverged, vs.arg_bad, vs.seg_bad,
                vs.seg_bad_indirect, vs.final_bad, vs.ret_bad, vs.outside, vs.skipped, s);
    auto top = [](const std::map<std::string, uint64_t>& m, size_t n) {
        std::vector<std::pair<uint64_t, std::string>> v;
        for (auto& [k, c] : m) v.push_back({c, k});
        std::sort(v.rbegin(), v.rend());
        std::string o;
        for (size_t i = 0; i < v.size() && i < n; ++i) o += fmt::format(" [{} x{}]", v[i].second, v[i].first);
        return o;
    };
    REXLOG_INFO("NATIVE MESH VERIFY words differing before a call (no sampler call in between), top:{}", top(vs.seg_offs, 25));
    REXLOG_INFO("NATIVE MESH VERIFY words differing at the end, top:{}", top(vs.final_offs, 25));
    REXLOG_INFO("NATIVE MESH VERIFY after sampler calls, words OTHER than device+16/20 and fetch constants, top:{}", top(vs.seg_offs_sampler, 25));
    REXLOG_INFO("NATIVE MESH VERIFY argument mismatches:{}", top(vs.args_detail, 10));
    for (auto& e : vs.examples) REXLOG_INFO("NATIVE MESH VERIFY example: {}", e);
    vs.examples.clear();
}
void Verify(PPCContext& ctx, u8* base) {
    const u32 tls = L32<false>(base, ctx.r13.u32 + 0);
    const u32 D = L32<false>(base, tls + 7688), Rn = L32<false>(base, tls + 7692), X = L32<false>(base, tls + 7696);
    const u32 sp = ctx.r1.u32 - 496;
    const u32 a[kRegions] = {D, Rn, X, sp}, z[kRegions] = {kDevSize, kRenSize, kDeclSize, kFrameSize};
    for (int i = 0; i < kRegions; ++i)
        for (int j = i + 1; j < kRegions; ++j)
            if (a[i] < a[j] + z[j] && a[j] < a[i] + z[i]) {
                { std::lock_guard l(vs.m); ++vs.skipped; }
                __imp__sub_8223E8D8(ctx, base);
                return;
            }
    for (int i = 0; i < kRegions; ++i) { rp.addr[i] = a[i]; rp.size[i] = z[i]; }
    rp.arena.clear(); rp.recs.clear();
    rp.entry = Alloc(); Snap(base, rp.entry);
    const PPCContext in = ctx;
    // 1) The original, for real, recording its direct child calls.
    rp.recording = true; rp.depth = 0;
    __imp__sub_8223E8D8(ctx, base);
    rp.recording = false;
    rp.final_snap = Alloc(); Snap(base, rp.final_snap); rp.final_r3 = ctx.r3.u64;
    // 2) The native body against private copies (nothing live is written).
    std::memcpy(rp.priv, rp.arena.data() + rp.entry, kTotal);
    rp.k = 0; rp.diverged = false; rp.indirect_in_segment = false; rp.writes_outside = 0;
    rp.dbgP = in.r3.u32; rp.dbgPass = in.r6.u32; rp.dbgSpass = in.r6.u32 == 9 ? 0 : in.r6.u32 == 10 ? 1 : in.r6.u32;
    {
        thread_local PPCContext rc;
        rc = in;
        RunT<true>(rc, base);
        ctx.fpscr.setcsr(ctx.fpscr.csr);  // host MXCSR back in step with the live context
        std::string fd;
        std::map<std::string, uint64_t> foffs;
        const int fbad = CompareTo(rp.final_snap, &fd, &foffs);
        std::lock_guard l(vs.m);
        ++vs.samples; vs.children += rp.recs.size();
        bool dirty = false;
        if (rp.diverged || rp.k != rp.recs.size()) {
            ++vs.diverged; dirty = true;
            if (!rp.diverged && vs.examples.size() < 30)
                vs.examples.push_back(fmt::format("native made {} child calls, original {}", rp.k, rp.recs.size()));
        }
        if (fbad && !rp.diverged) {
            ++vs.final_bad; dirty = true;
            for (auto& [k, v] : foffs) vs.final_offs[k] += v;
            if (vs.examples.size() < 30) vs.examples.push_back(fmt::format("FINAL STATE:{}", fd));
        }
        if (rc.r3.u64 != rp.final_r3) { ++vs.ret_bad; dirty = true; }
        if (rp.writes_outside) { vs.outside += rp.writes_outside; dirty = true; }
        if (!dirty && vs.arg_bad + vs.seg_bad == 0) {}
        if (!dirty) ++vs.clean;
        MaybeLogLocked();
    }
}
bool ShouldVerify() { thread_local uint32_t n = 0; return (++n & 255) == 0; }
// Entry used by the 8223E8D8 hook: native, verify or original.
inline void Dispatch(PPCContext& ctx, u8* base) {
    const int m = Mode();
    if (m == 2) {
        if (ShouldVerify()) Verify(ctx, base); else __imp__sub_8223E8D8(ctx, base);
        return;
    }
    const bool native = m == 1 && (!AbOn() || ab_mode.load(std::memory_order_relaxed) == 1);
    if (AbOn()) {
        const uint64_t t0 = __rdtsc();
        native ? Run(ctx, base) : __imp__sub_8223E8D8(ctx, base);
        const int k = native ? 1 : 0;
        ab_ticks[k].fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        ab_calls[k].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    native ? Run(ctx, base) : __imp__sub_8223E8D8(ctx, base);
}
}  // namespace native_mesh
#undef NM_CALL

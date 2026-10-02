// NATIVE POLYGON CLIP (2026-10-02). 8264E598(r3 n, r4 in, r5 out, r6 plane) clips a convex polygon of n float3
// vertices (12 bytes each) against plane (a, b, c, d) and writes the kept/cut vertices to out; returns their
// count in r3. Called from 8264E7A0 <- render-list prep 8218FB50 (821903F0) on the job workers: the hottest guest
// function at the air spot (~2.3 % of all busy samples). Same float ops in the same order as the recompiled code
// (products of float-valued doubles are exact, so fmadds = a*b + c in double, rounded to float once);
// classification thresholds are the two floats at 0x82089518 / 0x8208952C (re-read every call).
// dist\native_clip_off = original; dist\native_clip_verify = 1 call in 32 runs both and compares.
extern "C" void __imp__sub_8264E598(PPCContext& ctx, uint8_t* base);
namespace native_clip {
using u8 = uint8_t; using u32 = uint32_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline float Fl(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); v = __builtin_bswap32(v); float f; std::memcpy(&f, &v, 4); return f; }
inline u32 W(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return v; }  // raw (big-endian) word
inline double R(double x) { return double(float(x)); }
int Mode() {
    static const int m = [] {
        int r = 1;
        if (FILE* f = std::fopen("native_clip_off", "rb")) { std::fclose(f); r = 0; }
        else if (FILE* f2 = std::fopen("native_clip_verify", "rb")) { std::fclose(f2); r = 2; }
        REXLOG_INFO("Native polygon clip (8264E598): {}", r == 0 ? "off" : r == 1 ? "on" : "VERIFY");
        return r;
    }();
    return m;
}
// out_words receives raw big-endian words (3 per vertex). Returns the count.
inline u32 Clip(u8* b, u32 n, u32 in, u32 plane, u32* out_words) {
    if (int32_t(n) <= 0) return 0;
    const double hi = double(Fl(b, 0x82089518u)), lo = double(Fl(b, 0x8208952Cu));
    const double px = double(Fl(b, plane)), py = double(Fl(b, plane + 4)), pz = double(Fl(b, plane + 8)),
                 pw = double(Fl(b, plane + 12));
    u32 cnt = 0, k = 1, o = 0;
    for (u32 left = n, cur = in; left != 0; --left, cur += 12) {
        const double xc = double(Fl(b, cur)), yc = double(Fl(b, cur + 4)), zc = double(Fl(b, cur + 8));
        double d0 = R(yc * py);
        d0 = R(xc * px + d0);
        d0 = R(zc * pz + d0);
        d0 = R(d0 + pw);
        const bool in0 = !(d0 > hi);  // class 1 (d > hi) = outside
        const u32 nk = in + k * 12;
        const double xk = double(Fl(b, nk)), yk = double(Fl(b, nk + 4)), zk = double(Fl(b, nk + 8));
        double d1 = R(zk * pz);
        d1 = R(yk * py + d1);
        d1 = R(xk * px + d1);
        d1 = R(d1 + pw);
        const bool in1 = !(d1 > hi);
        (void)lo;  // classes 2/3 only matter for "!= 1"
        bool cut = false;
        if (in0) {
            out_words[o++] = W(b, cur); out_words[o++] = W(b, cur + 4); out_words[o++] = W(b, cur + 8);
            ++cnt;
            cut = !in1;
        } else {
            cut = in1;
        }
        if (cut) {
            ++cnt;
            const double f1 = R(zk * pz);
            const double f11a = R(zc * pz);
            const double f6 = R(zk - zc);
            const double f2 = R(yk * py + f1);
            const double f11b = R(yc * py + f11a);
            const double f7 = R(yk - yc);
            const double f8 = R(xk - xc);
            double f10 = R(px * xk + f2);
            const double f11c = R(-(xc * px + f11b));
            f10 = -f10;
            const double f9 = R(pw - f11c);
            double t = R(f10 - f11c);
            t = R(f9 / t);
            const float ox = float(xc + R(f8 * t)), oy = float(yc + R(f7 * t)), oz = float(zc + R(f6 * t));
            u32 v;
            std::memcpy(&v, &ox, 4); out_words[o++] = __builtin_bswap32(v);
            std::memcpy(&v, &oy, 4); out_words[o++] = __builtin_bswap32(v);
            std::memcpy(&v, &oz, 4); out_words[o++] = __builtin_bswap32(v);
        }
        if (++k == n) k = 0;
    }
    return cnt;
}
std::atomic<uint64_t> v_calls{0}, v_bad{0}, big{0};
}  // namespace native_clip

PPC_FUNC(sub_8264E598) {
    using namespace native_clip;
    const int mode = Mode();
    const u32 n = ctx.r3.u32;
    if (mode == 0 || n > 64) { if (mode == 2 && n > 64) ++big; __imp__sub_8264E598(ctx, base); return; }
    u32 words[64 * 2 * 3];
    ctx.fpscr.disableFlushMode();
    const u32 out = ctx.r5.u32;
    const u32 cnt = Clip(base, n, ctx.r4.u32, ctx.r6.u32, words);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            __imp__sub_8264E598(ctx, base);
            const bool bad = ctx.r3.u32 != cnt || std::memcmp(G(base, out), words, size_t(cnt) * 12) != 0;
            const uint64_t c = ++v_calls;
            if (bad && v_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE CLIP differs: n {} count native {} original {}", n, cnt, ctx.r3.u32);
            if ((c & 4095) == 0)
                REXLOG_INFO("NATIVE CLIP VERIFY: {} compared, {} differ, {} too big (original)", c, v_bad.load(), big.load());
            return;
        }
    }
    std::memcpy(G(base, out), words, size_t(cnt) * 12);
    ctx.r3.u64 = cnt;
}

// NATIVE FRUSTUM TEST (2026-10-02). 82125A98(r3 corners: float3 x 8, r4 frustum: +0 plane count 3..10, +16+16*i
// corner index of plane i, +176+16*i plane (a, b, c, d)): per plane d = ((b*y) + c*z) + a*x + d of the chosen
// corner, fsel(d >= 0 ? [0x82089248] : [0x82089238]) rounded to float, summed from the last plane down (same
// order as the recompiled unrolled code; count 10 starts from plane 9's value, otherwise from the float at
// 0x820875EC); returns (sum > that float) ? 0 : 1. Count outside 3..10 -> 1. Visibility walk on the render
// thread and the cell jobs (~1.7 % of all busy samples at the air spot). Same switches as the clip
// (native_clip_off / native_clip_verify).
extern "C" void __imp__sub_82125A98(PPCContext& ctx, uint8_t* base);
namespace native_clip {
inline double D64(u8* b, u32 a) { uint64_t v; std::memcpy(&v, G(b, a), 8); v = __builtin_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
inline u32 U(u8* b, u32 a) { return __builtin_bswap32(W(b, a)); }
inline u32 Frustum(u8* b, u32 corners, u32 fr) {
    const u32 count = U(b, fr);
    const double f9 = double(Fl(b, 0x820875ECu));
    if (count - 3u > 7u) return 1;
    const double f12 = D64(b, 0x82089238u), f13 = D64(b, 0x82089248u);
    auto val = [&](u32 i) {
        const u32 c = corners + U(b, fr + 16 + 16 * i) * 12u;
        const u32 p = fr + 176 + 16 * i;
        const double a = double(Fl(b, p)), bb = double(Fl(b, p + 4)), cc = double(Fl(b, p + 8)), d = double(Fl(b, p + 12));
        const double x = double(Fl(b, c)), y = double(Fl(b, c + 4)), z = double(Fl(b, c + 8));
        double t = R(bb * y);
        t = R(cc * z + t);
        t = R(a * x + t);
        t = R(t + d);
        return R(t >= 0.0 ? f13 : f12);
    };
    double s;
    int i = int(count) - 1;
    if (count == 10) { s = val(9); i = 8; } else { s = f9; }
    for (; i >= 3; --i) s = R(val(u32(i)) + s);
    s = R(val(2) + s);
    s = R(s + val(1));
    s = R(s + val(0));
    return s > f9 ? 0u : 1u;
}
std::atomic<uint64_t> f_calls{0}, f_bad{0};
}  // namespace native_clip

PPC_FUNC(sub_82125A98) {
    using namespace native_clip;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_82125A98(ctx, base); return; }
    ctx.fpscr.disableFlushMode();
    const u32 r = Frustum(base, ctx.r3.u32, ctx.r4.u32);
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 31) == 0) {
            __imp__sub_82125A98(ctx, base);
            const uint64_t c = ++f_calls;
            if (ctx.r3.u32 != r && f_bad.fetch_add(1) < 12)
                REXLOG_INFO("NATIVE FRUSTUM differs: native {} original {}", r, ctx.r3.u32);
            if ((c & 16383) == 0) REXLOG_INFO("NATIVE FRUSTUM VERIFY: {} compared, {} differ", c, f_bad.load());
            return;
        }
    }
    ctx.r3.u64 = r;
}

// NATIVE 4x4 MATRIX MULTIPLY (2026-10-01). 8219DFC0(r3 out, r4 A, r5 B): out = A * B, row-major,
// 16 dot products (vmsum4fp128) on B transposed; ~2 % of all CPU (render thread + colour worker).
// Same SSE ops in the same lanes as the recompiled code (dp_ps 0xFF, unpack transposes), same
// flush-mode switch, same stack copies (r1-128 = B, r1-64 = A) and the same aligned-down stores.
// Needs a 16-byte aligned r1 (else the original runs). dist\native_math_off = original;
// dist\native_math_verify = 1 call in 64: native into a temp, original for real, compare.
extern "C" void __imp__sub_8219DFC0(PPCContext& ctx, uint8_t* base);
namespace native_math {
using u8 = uint8_t; using u32 = uint32_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
int Mode() {
    static const int m = [] {
        if (FILE* f = std::fopen("native_math_off", "rb")) { std::fclose(f); return 0; }
        if (FILE* f = std::fopen("native_math_verify", "rb")) { std::fclose(f); return 2; }
        return 1;
    }();
    return m;
}
// Computes the 4 output rows (already byte-reversed for big-endian storing) into out[4].
inline void Mul(u8* b, u32 ra, u32 rb, __m128i out[4]) {
    const __m128i rev = _mm_load_si128(reinterpret_cast<const __m128i*>(rex::ppc::VectorMaskL));
    auto L = [&](u32 a) { return _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(G(b, a))), rev); };
    const __m128i B0 = L(rb), B1 = L(rb + 16), B2 = L(rb + 32), B3 = L(rb + 48);
    const __m128 A0 = _mm_castsi128_ps(L(ra)), A1 = _mm_castsi128_ps(L(ra + 16));
    const __m128 A2 = _mm_castsi128_ps(L(ra + 32)), A3 = _mm_castsi128_ps(L(ra + 48));
    // v0 = B2, v11 = B0, v9 = B3, v8 = B1 (as loaded by the original)
    __m128i v13 = _mm_unpackhi_epi32(B2, B0);
    __m128i v11 = _mm_unpacklo_epi32(B2, B0);
    __m128i v12 = _mm_unpackhi_epi32(B3, B1);
    __m128i v7 = _mm_unpacklo_epi32(B3, B1);
    const __m128 c0 = _mm_castsi128_ps(_mm_unpackhi_epi32(v12, v13));
    const __m128 c1 = _mm_castsi128_ps(_mm_unpacklo_epi32(v12, v13));
    const __m128 c2 = _mm_castsi128_ps(_mm_unpackhi_epi32(v7, v11));
    const __m128 c3 = _mm_castsi128_ps(_mm_unpacklo_epi32(v7, v11));
    // v10 = A0, v9 = A1, v8 = A2, v10' = A3
    const __m128i d7 = _mm_castps_si128(_mm_dp_ps(A0, c0, 0xFF)), d3 = _mm_castps_si128(_mm_dp_ps(A0, c2, 0xFF));
    const __m128i d2 = _mm_castps_si128(_mm_dp_ps(A0, c3, 0xFF)), d6 = _mm_castps_si128(_mm_dp_ps(A0, c1, 0xFF));
    const __m128i d5 = _mm_castps_si128(_mm_dp_ps(A1, c0, 0xFF)), d4 = _mm_castps_si128(_mm_dp_ps(A1, c1, 0xFF));
    const __m128i d1 = _mm_castps_si128(_mm_dp_ps(A1, c2, 0xFF)), d9 = _mm_castps_si128(_mm_dp_ps(A1, c3, 0xFF));
    const __m128i d30 = _mm_castps_si128(_mm_dp_ps(A2, c2, 0xFF)), d28 = _mm_castps_si128(_mm_dp_ps(A3, c0, 0xFF));
    const __m128i d12 = _mm_castps_si128(_mm_dp_ps(A3, c2, 0xFF)), d31 = _mm_castps_si128(_mm_dp_ps(A2, c0, 0xFF));
    const __m128i d29 = _mm_castps_si128(_mm_dp_ps(A2, c1, 0xFF)), d27 = _mm_castps_si128(_mm_dp_ps(A3, c1, 0xFF));
    const __m128i d26 = _mm_castps_si128(_mm_dp_ps(A3, c3, 0xFF)), d8 = _mm_castps_si128(_mm_dp_ps(A2, c3, 0xFF));
    __m128i v0 = _mm_unpackhi_epi32(d3, d7);
    __m128i w13 = _mm_unpackhi_epi32(d2, d6);
    __m128i w11 = _mm_unpackhi_epi32(d1, d5);
    __m128i w10 = _mm_unpackhi_epi32(d9, d4);
    v0 = _mm_unpackhi_epi32(w13, v0);
    w13 = _mm_unpackhi_epi32(d12, d28);
    const __m128i w12 = _mm_unpackhi_epi32(w10, w11);
    const __m128i w9 = _mm_unpackhi_epi32(d30, d31);
    w11 = _mm_unpackhi_epi32(d26, d27);
    const __m128i w8 = _mm_unpackhi_epi32(d8, d29);
    out[0] = _mm_shuffle_epi8(v0, rev);                              // -> r3
    out[1] = _mm_shuffle_epi8(w12, rev);                             // -> r3 + 16
    out[3] = _mm_shuffle_epi8(_mm_unpackhi_epi32(w11, w13), rev);    // -> r3 + 48
    out[2] = _mm_shuffle_epi8(_mm_unpackhi_epi32(w8, w9), rev);      // -> r3 + 32
}
std::atomic<uint64_t> v_calls{0}, v_bad{0}, unaligned{0};
std::atomic<int64_t> v_last{0};
}  // namespace native_math

PPC_FUNC(sub_8219DFC0) {
    using namespace native_math;
    const int mode = Mode();
    if (mode == 0 || (ctx.r1.u32 & 15)) {
        if (mode && (ctx.r1.u32 & 15)) unaligned.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_8219DFC0(ctx, base);
        return;
    }
    const u32 r1 = ctx.r1.u32, r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    if (mode == 2) {
        static thread_local uint32_t tick = 0;
        if ((++tick & 63) == 0) {
            __m128i o[4];
            ctx.fpscr.enableFlushMode();
            Mul(base, r4, r5, o);
            __imp__sub_8219DFC0(ctx, base);
            bool bad = false;
            for (int k = 0; k < 4; ++k)
                bad |= std::memcmp(&o[k], G(base, (r3 + 16 * k) & ~15u), 16) != 0;
            ++v_calls;
            if (bad) ++v_bad;
            const int64_t now = int64_t(GetTickCount64());
            int64_t last = v_last.load();
            if (now - last > 10000 && v_last.compare_exchange_strong(last, now))
                REXLOG_INFO("NATIVE MATH VERIFY: {} matrix products compared, {} differ, unaligned stacks {}",
                            v_calls.load(), v_bad.load(), unaligned.load());
            return;
        }
    }
    // The original's stack copies (B at r1-128, A at r1-64), then the product.
    std::memmove(G(base, r1 - 128), G(base, r5), 64);
    std::memmove(G(base, r1 - 64), G(base, r4), 64);
    ctx.fpscr.enableFlushMode();
    __m128i o[4];
    Mul(base, r1 - 64, r1 - 128, o);
    for (int k = 0; k < 4; ++k) _mm_store_si128(reinterpret_cast<__m128i*>(G(base, (r3 + 16 * k) & ~15u)), o[k]);
}

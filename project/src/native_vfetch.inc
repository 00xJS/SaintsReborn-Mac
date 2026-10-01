// VERTEX-FETCH PATCHER MEMO (2026-10-01). 825DB738(r3 shader, r4 microcode out, r5 vertex
// declaration, r6 per-stream byte table) builds the shader's vfetch instructions for a vertex
// declaration (match elements by usage/index, pack format/offset/stride bits, sort + merge) and
// copies each 12-byte result to r4 + (list[k] & 0xFFF) * 12. 800-1500 calls per frame, mostly the
// same shader/declaration pairs -> results are cached per thread, keyed by EVERY input it reads:
// shader +60 count, +64 list, +192 entries (12 bytes each); declaration +24 count, +52 elements
// (12 bytes each); the table bytes [0] and [stream of each element]; and the low 4 bits of the
// never-initialised halfword at its frame + 80 (the first entry carries them into the output).
// The constant tables it reads (0x82047D24, 0x82085B38) are read-only data.
// dist\vfetch_memo_off = original; dist\vfetch_memo_verify = 1 hit in 32 also runs the original
// and compares ("VFETCH MEMO VERIFY").
extern "C" void __imp__sub_825DB738(PPCContext& ctx, uint8_t* base);
namespace vfetch_memo {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
inline u8* G(u8* b, u32 a) { return b + a + REX_PHYS_HOST_OFFSET(a); }
inline u32 L32(u8* b, u32 a) { u32 v; std::memcpy(&v, G(b, a), 4); return __builtin_bswap32(v); }
inline u32 L16(u8* b, u32 a) { uint16_t v; std::memcpy(&v, G(b, a), 2); return __builtin_bswap16(v); }
int Mode() {
    static const int m = [] {
        if (FILE* f = std::fopen("vfetch_memo_off", "rb")) { std::fclose(f); return 0; }
        if (FILE* f = std::fopen("vfetch_memo_verify", "rb")) { std::fclose(f); return 2; }
        return 1;
    }();
    return m;
}
constexpr u32 kSlots = 1024;
struct Entry { u64 hash = 0; std::vector<u8> key; std::vector<u8> out; };  // out: n * 12 bytes
struct Cache { Entry e[kSlots]; std::vector<u8> key; };
thread_local Cache* tc = nullptr;
std::atomic<uint64_t> hits{0}, misses{0}, skipped{0}, v_calls{0}, v_bad{0};
std::atomic<int64_t> last_log{0};
inline u64 Hash(const std::vector<u8>& k) {
    u64 h = 0x9E3779B97F4A7C15ull ^ k.size();
    size_t i = 0;
    for (; i + 8 <= k.size(); i += 8) { u64 w; std::memcpy(&w, k.data() + i, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < k.size(); ++i) { h = (h ^ k[i]) * 0x100000001B3ull; }
    return h ^ (h >> 31);
}
void MaybeLog() {
    const int64_t now = int64_t(GetTickCount64());
    int64_t last = last_log.load();
    if (now - last < 10000 || !last_log.compare_exchange_strong(last, now)) return;
    REXLOG_INFO("VFETCH MEMO: hits {}, misses {}, not cached {} | verify: {} compared, {} differ", hits.load(),
                misses.load(), skipped.load(), v_calls.load(), v_bad.load());
}
}  // namespace vfetch_memo

void sr_vfetch_patch(PPCContext& ctx, uint8_t* base) {
    using namespace vfetch_memo;
    const int mode = Mode();
    if (mode == 0) { __imp__sub_825DB738(ctx, base); return; }
    const u32 S = ctx.r3.u32, out = ctx.r4.u32, D = ctx.r5.u32, T = ctx.r6.u32;
    const u32 n = L32(base, S + 60), cnt = L32(base, D + 24);
    if (n == 0 || n > 96 || cnt > 64) {
        skipped.fetch_add(1, std::memory_order_relaxed);
        __imp__sub_825DB738(ctx, base);
        return;
    }
    if (!tc) tc = new Cache();
    std::vector<u8>& key = tc->key;
    key.clear();
    auto put = [&](const u8* p, size_t len) { key.insert(key.end(), p, p + len); };
    auto put32 = [&](u32 v) { put(reinterpret_cast<const u8*>(&v), 4); };
    put32(n);
    put(G(base, S + 64), n * 4);
    put(G(base, S + 192), n * 12);
    put32(cnt);
    put(G(base, D + 52), cnt * 12);
    key.push_back(*G(base, T));
    for (u32 i = 0; i < cnt; ++i) key.push_back(*G(base, T + L16(base, D + 52 + i * 12)));
    key.push_back(u8(*G(base, ctx.r1.u32 - 592 + 81) & 15));
    const u64 h = Hash(key);
    Entry& e = tc->e[h & (kSlots - 1)];
    const bool hit = e.hash == h && e.key == key;
    auto Dst = [&](u32 k) { return out + (L32(base, S + 64 + k * 4) & 0xFFF) * 12; };
    if (hit && !(mode == 2 && (hits.load(std::memory_order_relaxed) & 31) == 0)) {
        hits.fetch_add(1, std::memory_order_relaxed);
        const u32 frame_tmp = ctx.r1.u32 - 592 + 96;
        for (u32 k = 0; k < n; ++k) {
            const u32 d = Dst(k);
            if (d != frame_tmp + k * 12) std::memcpy(G(base, d), e.out.data() + k * 12, 12);
        }
        if (mode == 2) MaybeLog();
        return;
    }
    __imp__sub_825DB738(ctx, base);
    if (hit) {  // verify: the cached result must equal what the original just wrote
        bool bad = false;
        for (u32 k = 0; k < n; ++k) bad |= std::memcmp(G(base, Dst(k)), e.out.data() + k * 12, 12) != 0;
        hits.fetch_add(1, std::memory_order_relaxed);
        v_calls.fetch_add(1, std::memory_order_relaxed);
        if (bad) v_bad.fetch_add(1, std::memory_order_relaxed);
        MaybeLog();
        return;
    }
    misses.fetch_add(1, std::memory_order_relaxed);
    e.hash = h;
    e.key = key;
    e.out.resize(n * 12);
    for (u32 k = 0; k < n; ++k) std::memcpy(e.out.data() + k * 12, G(base, Dst(k)), 12);
    if (mode == 2) MaybeLog();
}

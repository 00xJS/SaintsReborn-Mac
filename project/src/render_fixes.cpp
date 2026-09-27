// Rendering fixes for the PC port.
//
// Single-pass rendering. The game's render configs (9 x 156 bytes at
// 0x827D6D38: +0 MSAA 0/1/2 = 1x/2x/4x, +12 tile count, +20 tile rects
// x1,y1,x2,y2) include 2x/4x MSAA modes split into 2-4 screen strips
// ("predicated tiling"), because on the Xbox 360 a 4x MSAA 720p frame does not
// fit in the GPU's 10 MB of EDRAM. The shop menus use the 4-strip mode: the
// whole scene is drawn 4 times, and the replayed command buffers also get in
// the way of the queued GPU command streams. Before the game lays out its
// configs (82183008), all of them become 1x with one full-screen tile. The
// internal 2x resolution already smooths edges. A file named "msaa_tiling"
// next to saintsrow.exe keeps the original behaviour.

#include "saintsrow_config.h"
#include "saintsrow_init.h"

#include <rex/ppc/function.h>

#include <cstdint>
#include <cstdio>

namespace {

inline uint32_t Rd32(uint8_t* base, uint32_t addr) {
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + addr));
}
inline void Wr32(uint8_t* base, uint32_t addr, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(base + addr) = __builtin_bswap32(value);
}
bool FileExists(const char* name) {
    if (FILE* f = std::fopen(name, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

constexpr uint32_t kRenderConfigs = 0x827D6D38;
constexpr uint32_t kRenderConfigSize = 156;
constexpr uint32_t kRenderConfigCount = 9;

}  // namespace

extern "C" void __imp__sub_82183008(PPCContext& ctx, uint8_t* base);
PPC_FUNC(sub_82183008) {
    static const bool keep_tiling = FileExists("msaa_tiling");
    if (!keep_tiling) {
        // Config 0 is the normal 1x single-tile one; copy its full-screen rect.
        const uint32_t x2 = Rd32(base, kRenderConfigs + 28);
        const uint32_t y2 = Rd32(base, kRenderConfigs + 32);
        for (uint32_t i = 0; i < kRenderConfigCount; ++i) {
            const uint32_t e = kRenderConfigs + i * kRenderConfigSize;
            if (Rd32(base, e) == 0 && Rd32(base, e + 12) == 1) continue;
            Wr32(base, e, 0);       // no MSAA
            Wr32(base, e + 12, 1);  // one tile
            Wr32(base, e + 20, 0);
            Wr32(base, e + 24, 0);
            Wr32(base, e + 28, x2);
            Wr32(base, e + 32, y2);
        }
    }
    __imp__sub_82183008(ctx, base);
}

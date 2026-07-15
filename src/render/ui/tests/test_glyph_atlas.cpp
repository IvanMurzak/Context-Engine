// render-ui-test_glyph_atlas — the GLYPH-ID-keyed glyph atlas: build (pack distinct glyphs), cache hits
// (no re-raster), glyph-id/size/font keying, blit into the R8 buffer, LRU eviction when full, LRU-touch
// ordering, and the un-renderable / oversize / whitespace edges (M7 a7 DoD, R-QA-013). FreeType-free:
// a synthetic rasterizer stands in for a real face.

#include "context/render/ui/glyph_atlas.h"

#include "render_test.h"

#include <cstdint>
#include <optional>

using namespace context::render::ui;

namespace
{
// A synthetic rasterizer producing a `w`x`h` coverage filled with a glyph-derived value (so a blit is
// verifiable). `left`/`top`/`advance` are plausible non-zero placements.
GlyphRasterizer fixed_raster(int w, int h)
{
    return [w, h](const AtlasKey& k) -> std::optional<GlyphCoverage>
    {
        GlyphCoverage c;
        c.width = w;
        c.height = h;
        c.left = 1;
        c.top = h;
        c.advance = static_cast<float>(w + 1);
        c.pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                        static_cast<std::uint8_t>((k.glyph % 255u) + 1u));
        return c;
    };
}

const void* kFontA = reinterpret_cast<const void*>(0x1000);
const void* kFontB = reinterpret_cast<const void*>(0x2000);
} // namespace

int main()
{
    // --- build: pack distinct glyphs, no eviction --------------------------------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1}); // cols 8 * rows 8 = 64 cells
        CHECK(atlas.capacity() == 64);
        auto raster = fixed_raster(8, 8);
        for (std::uint32_t g = 0; g < 5; ++g)
            CHECK(atlas.acquire(AtlasKey{kFontA, g, 16}, raster) != nullptr);
        CHECK(atlas.count() == 5);
        CHECK(atlas.evictions() == 0);
        CHECK(atlas.rasterizations() == 5);
        CHECK(atlas.contains(AtlasKey{kFontA, 3, 16}));
        CHECK(atlas.peek(AtlasKey{kFontA, 3, 16}) != nullptr);
        CHECK(atlas.peek(AtlasKey{kFontA, 99, 16}) == nullptr);

        // cache hit: re-acquire does NOT rasterize again and returns the same slot
        const AtlasSlot* first = atlas.peek(AtlasKey{kFontA, 2, 16});
        const AtlasSlot* again = atlas.acquire(AtlasKey{kFontA, 2, 16}, raster);
        CHECK(again == first);
        CHECK(atlas.rasterizations() == 5); // unchanged — it was a hit
        CHECK(atlas.count() == 5);

        // the coverage byte was blitted into the R8 buffer at the slot's top-left
        const AtlasSlot* s = atlas.peek(AtlasKey{kFontA, 4, 16});
        const std::size_t idx =
            static_cast<std::size_t>(s->y) * static_cast<std::size_t>(atlas.width())
            + static_cast<std::size_t>(s->x);
        CHECK(atlas.pixels()[idx] == static_cast<std::uint8_t>((4u % 255u) + 1u));
    }

    // --- GLYPH-ID-keyed: glyph id, pixel size, and font all key separately ------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1});
        auto raster = fixed_raster(8, 8);
        const AtlasSlot* g1 = atlas.acquire(AtlasKey{kFontA, 1, 16}, raster);
        const AtlasSlot* g2 = atlas.acquire(AtlasKey{kFontA, 2, 16}, raster);
        const AtlasSlot* g1_big = atlas.acquire(AtlasKey{kFontA, 1, 32}, raster); // same glyph, diff px
        const AtlasSlot* g1_fontB = atlas.acquire(AtlasKey{kFontB, 1, 16}, raster); // diff font
        CHECK(g1 && g2 && g1_big && g1_fontB);
        // all four are distinct cache entries in distinct cells
        CHECK(atlas.count() == 4);
        CHECK((g1->x != g2->x) || (g1->y != g2->y));
        CHECK((g1->x != g1_big->x) || (g1->y != g1_big->y));
        CHECK((g1->x != g1_fontB->x) || (g1->y != g1_fontB->y));
    }

    // --- eviction: full atlas evicts the LRU on the next miss -------------------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1}); // capacity 64
        auto raster = fixed_raster(8, 8);
        for (std::uint32_t g = 0; g < 64; ++g)
            CHECK(atlas.acquire(AtlasKey{kFontA, g, 16}, raster) != nullptr);
        CHECK(atlas.count() == 64);
        CHECK(atlas.evictions() == 0);
        // one more -> evict the LRU (glyph 0, the first inserted, never touched)
        CHECK(atlas.acquire(AtlasKey{kFontA, 64, 16}, raster) != nullptr);
        CHECK(atlas.count() == 64); // stayed at capacity
        CHECK(atlas.evictions() == 1);
        CHECK(!atlas.contains(AtlasKey{kFontA, 0, 16})); // LRU evicted
        CHECK(atlas.contains(AtlasKey{kFontA, 64, 16})); // newcomer present
    }

    // --- LRU touch: a re-used glyph survives eviction ---------------------------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{32, 16, 16, 1}); // cols 2 * rows 1 = 2 cells
        CHECK(atlas.capacity() == 2);
        auto raster = fixed_raster(8, 8);
        atlas.acquire(AtlasKey{kFontA, 0, 16}, raster); // cell 0 (LRU)
        atlas.acquire(AtlasKey{kFontA, 1, 16}, raster); // cell 1
        atlas.acquire(AtlasKey{kFontA, 0, 16}, raster); // HIT -> glyph 0 now MRU
        atlas.acquire(AtlasKey{kFontA, 2, 16}, raster); // miss+full -> evicts LRU == glyph 1
        CHECK(atlas.contains(AtlasKey{kFontA, 0, 16}));  // touched -> survived
        CHECK(!atlas.contains(AtlasKey{kFontA, 1, 16})); // LRU -> evicted
        CHECK(atlas.contains(AtlasKey{kFontA, 2, 16}));
        CHECK(atlas.evictions() == 1);
    }

    // --- edges: un-renderable, oversize, whitespace -----------------------------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{64, 64, 16, 1}); // avail = 15 per cell
        // un-renderable -> nullptr, not cached
        GlyphRasterizer none = [](const AtlasKey&) -> std::optional<GlyphCoverage>
        { return std::nullopt; };
        CHECK(atlas.acquire(AtlasKey{kFontA, 1, 16}, none) == nullptr);
        CHECK(atlas.count() == 0);
        // oversize (20 > 15) -> nullptr, not cached
        CHECK(atlas.acquire(AtlasKey{kFontA, 2, 16}, fixed_raster(20, 20)) == nullptr);
        CHECK(atlas.count() == 0);
        // whitespace (0x0 coverage) -> a valid empty slot occupying a cell
        GlyphRasterizer space = [](const AtlasKey&) -> std::optional<GlyphCoverage>
        {
            GlyphCoverage c;
            c.advance = 5.0f;
            return c; // width == height == 0
        };
        const AtlasSlot* sp = atlas.acquire(AtlasKey{kFontA, 3, 16}, space);
        CHECK(sp != nullptr);
        CHECK(sp->w == 0 && sp->h == 0);
        CHECK(sp->advance == 5.0f);
        CHECK(atlas.count() == 1);
    }

    RENDER_TEST_MAIN_END();
}

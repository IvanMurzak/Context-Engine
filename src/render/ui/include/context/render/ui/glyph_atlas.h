// The runtime UI glyph atlas (M7 a7, R-UI-005; issue #237). A single-channel (R8) glyph cache that
// packs rasterized glyph coverage into a bounded texture with LRU eviction, keyed by GLYPH ID (not
// codepoint) — so a8's shaped output (glyph ids + GPOS offsets) flows through WITHOUT a rewrite; a
// codepoint-keyed atlas would force an a8 rebuild.
//
// FreeType-FREE by design: the atlas never links the text package or FreeType. It rasterizes via a
// caller-supplied callback (from a text::FontFace, or a synthetic rasterizer in tests), so it stays
// GPU-free + web-portable and unit-tests with no font dependency. The R8 pixel buffer is what the draw
// path uploads to the atlas texture; here it is a CPU buffer the fake-RHI tests assert directly.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace context::render::ui
{

// A font-specific glyph index (mirrors text::GlyphId without a link dependency — a plain uint crossing
// the FreeType-decoupled boundary).
using GlyphId = std::uint32_t;

// The atlas cache key: GLYPH-ID-keyed. `font` is an opaque face identity (text::FontFace::identity());
// `px` is the quantized (integer) pixel size. Two faces / sizes / glyph ids each cache separately.
struct AtlasKey
{
    const void* font = nullptr;
    GlyphId glyph = 0;
    std::uint32_t px = 0;

    bool operator==(const AtlasKey&) const noexcept = default;
};

// An 8-bit alpha coverage bitmap for one glyph (the rasterizer's output). Mirrors text::GlyphBitmap,
// duplicated here so the atlas carries no text-package dependency. `pixels` is `width * height`,
// row-major; a whitespace glyph has an empty bitmap (width == height == 0) but a non-zero `advance`.
struct GlyphCoverage
{
    int width = 0;
    int height = 0;
    int left = 0;     // bearingX (px)
    int top = 0;      // bearingY (px, baseline-up)
    float advance = 0.0f;
    std::vector<std::uint8_t> pixels;
};

// A placed glyph: its rect in the atlas texture (texels) + the pen-relative placement metrics the quad
// emitter needs. A whitespace glyph has w == h == 0 (no texels; the emitter skips its quad).
struct AtlasSlot
{
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    int left = 0;
    int top = 0;
    float advance = 0.0f;
};

// Rasterizes a glyph key to coverage on an atlas MISS. nullopt == un-renderable (the glyph is skipped,
// advance-only). The atlas never links FreeType — the caller wires this to text::FontFace::rasterize
// (or a synthetic bitmap in tests).
using GlyphRasterizer = std::function<std::optional<GlyphCoverage>(const AtlasKey&)>;

// A fixed-cell-grid R8 glyph atlas with LRU eviction. On a miss it rasterizes into a free cell; when
// full it evicts the least-recently-used cell and reuses it, so a bounded texture serves an unbounded
// glyph stream (the point of an atlas). Deterministic (ascending cell allocation, strict LRU). A
// tighter skyline packer is a later optimization — the fixed cell keeps eviction correct + testable for
// the v1 substrate.
class GlyphAtlas
{
public:
    struct Config
    {
        int width = 256;   // atlas texture width (texels)
        int height = 256;  // atlas texture height (texels)
        int cell = 64;     // fixed cell size; a glyph larger than (cell - padding) is rejected
        int padding = 1;   // gutter reserved inside each cell (bleed guard)
    };

    explicit GlyphAtlas(Config cfg);

    // Look up `key`. On a HIT, mark it most-recently-used and return its slot. On a MISS, rasterize via
    // `raster`, place it (evicting the LRU cell when full), and return the new slot. Returns nullptr if
    // the glyph is un-renderable (rasterizer nullopt) OR larger than a cell — the caller advances the
    // pen but emits no quad. The returned pointer is valid until the NEXT eviction of that key.
    const AtlasSlot* acquire(const AtlasKey& key, const GlyphRasterizer& raster);

    // The cached slot for `key` WITHOUT inserting/rasterizing/touching LRU (nullptr if absent).
    [[nodiscard]] const AtlasSlot* peek(const AtlasKey& key) const;

    [[nodiscard]] bool contains(const AtlasKey& key) const;
    [[nodiscard]] std::size_t count() const noexcept { return map_.size(); }     // live glyphs
    [[nodiscard]] std::size_t capacity() const noexcept;                          // cell count
    [[nodiscard]] std::uint64_t evictions() const noexcept { return evictions_; }
    [[nodiscard]] std::uint64_t rasterizations() const noexcept { return rasterizations_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }   // texture-upload hint

    [[nodiscard]] int width() const noexcept { return cfg_.width; }
    [[nodiscard]] int height() const noexcept { return cfg_.height; }
    // The R8 coverage buffer (width*height bytes, row-major) the draw path uploads to the atlas texture.
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }

private:
    struct KeyHash
    {
        std::size_t operator()(const AtlasKey& k) const noexcept;
    };
    struct Entry
    {
        AtlasSlot slot;
        std::size_t cell = 0;
        std::list<AtlasKey>::iterator lru_it;
    };

    std::size_t evict_lru();                        // free + return the LRU cell index
    void clear_cell(int px, int py);                // zero a whole cell (bleed guard)
    void blit(const GlyphCoverage& cov, int px, int py);

    Config cfg_;
    int cols_ = 0;
    int rows_ = 0;
    std::vector<std::uint8_t> pixels_;
    std::vector<std::size_t> free_cells_;           // stack of free cell indices (ascending on pop)
    std::list<AtlasKey> lru_;                       // MRU at front, LRU at back
    std::unordered_map<AtlasKey, Entry, KeyHash> map_;
    std::uint64_t evictions_ = 0;
    std::uint64_t rasterizations_ = 0;
    std::uint64_t revision_ = 0;
};

} // namespace context::render::ui

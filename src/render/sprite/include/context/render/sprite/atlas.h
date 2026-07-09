// Texture atlases for the sprite path (R-2D-001) — CPU-side packing + UV lookup.
//
// A texture atlas is a single GPU texture holding many small sprite images side by side; drawing
// many sprites from ONE atlas lets the batcher (batch.h) coalesce them into one draw call. This
// header is the pure-CPU half: registering named sub-rectangles, normalized-UV lookup, and a simple
// shelf packer that lays a set of images out into an atlas. GPU-free, so it is unit-tested locally
// under every toolchain with no GPU.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace context::render::sprite
{

// A sub-rectangle of an atlas texture, in PIXELS (origin top-left, the image convention).
struct AtlasRegion
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// A normalized UV rectangle in [0,1] (u0,v0 = top-left corner, u1,v1 = bottom-right).
struct UVRect
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// A named set of regions within one atlas texture of a fixed pixel size. Lookup is by name; UVs are
// derived from the region and the atlas dimensions. Registration is ordered (insertion order is the
// iteration order) and rejects duplicate names / out-of-bounds regions.
class TextureAtlas
{
public:
    TextureAtlas() = default;
    TextureAtlas(std::uint32_t width, std::uint32_t height) : width_(width), height_(height) {}

    // Register `region` under `name`. Fails (returns false, no change) on a duplicate name or a region
    // that would extend past the atlas bounds.
    bool add(const std::string& name, const AtlasRegion& region);

    // The region for `name`, or nullptr if absent.
    [[nodiscard]] const AtlasRegion* find(const std::string& name) const;

    // Normalized UVs for a pixel region against this atlas's dimensions.
    [[nodiscard]] UVRect uv(const AtlasRegion& region) const;

    // Normalized UVs for a named region, or a default full-texture UVRect{} if the name is absent.
    [[nodiscard]] UVRect uv(const std::string& name) const;

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] std::size_t size() const { return regions_.size(); }
    [[nodiscard]] bool empty() const { return regions_.empty(); }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    // Insertion-ordered name -> region (small N per atlas; a vector keeps ordering + is cache-friendly).
    std::vector<std::pair<std::string, AtlasRegion>> regions_;
};

// One image to pack into an atlas: a name + its pixel dimensions.
struct PackItem
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The outcome of packing: the atlas built so far, plus the names that did NOT fit. `ok` is true only
// when every item was placed (overflow is empty).
struct PackResult
{
    bool ok = false;
    TextureAtlas atlas;
    std::vector<std::string> overflow;
};

// Pack `items` into an atlas of `atlas_width` x `atlas_height` using a simple SHELF (row) packer:
// items are placed left-to-right on a shelf whose height is the tallest item placed on it; a new
// shelf opens below when the current one is full. `padding` pixels are inserted between items and at
// the shelf edges (a bleed guard so bilinear sampling never picks up a neighbour). Items are packed
// in descending-height order (the standard shelf heuristic — minimizes wasted vertical space), but
// each item keeps its original `name`. An item taller/wider than the atlas (accounting for padding)
// goes to `overflow`. Deterministic: the same input always yields the same layout.
[[nodiscard]] PackResult pack_atlas(std::uint32_t atlas_width, std::uint32_t atlas_height,
                                    const std::vector<PackItem>& items, std::uint32_t padding = 0);

} // namespace context::render::sprite

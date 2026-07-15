// The fixed-cell-grid R8 glyph atlas with LRU eviction (see glyph_atlas.h). FreeType-free: rasterizes
// via the caller's GlyphRasterizer callback. Deterministic — cells allocate in ascending order and
// eviction is strict LRU — so the build/eviction ctests are reproducible.

#include "context/render/ui/glyph_atlas.h"

#include <algorithm>
#include <functional>

namespace context::render::ui
{

std::size_t GlyphAtlas::KeyHash::operator()(const AtlasKey& k) const noexcept
{
    std::size_t h = std::hash<const void*>{}(k.font);
    const auto mix = [&h](std::size_t v) noexcept
    { h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2); };
    mix(std::hash<std::uint32_t>{}(k.glyph));
    mix(std::hash<std::uint32_t>{}(k.px));
    return h;
}

GlyphAtlas::GlyphAtlas(Config cfg) : cfg_(cfg)
{
    if (cfg_.cell < 1)
        cfg_.cell = 1;
    if (cfg_.padding < 0)
        cfg_.padding = 0;
    if (cfg_.width < cfg_.cell)
        cfg_.width = cfg_.cell;
    if (cfg_.height < cfg_.cell)
        cfg_.height = cfg_.cell;

    cols_ = cfg_.width / cfg_.cell;
    rows_ = cfg_.height / cfg_.cell;
    pixels_.assign(static_cast<std::size_t>(cfg_.width) * static_cast<std::size_t>(cfg_.height), 0);

    const std::size_t total = static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_);
    free_cells_.reserve(total);
    // Push descending so pop_back() hands out ascending cell indices (deterministic placement).
    for (std::size_t i = total; i-- > 0;)
        free_cells_.push_back(i);
}

std::size_t GlyphAtlas::capacity() const noexcept
{
    return static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_);
}

bool GlyphAtlas::contains(const AtlasKey& key) const
{
    return map_.find(key) != map_.end();
}

const AtlasSlot* GlyphAtlas::peek(const AtlasKey& key) const
{
    const auto it = map_.find(key);
    return (it != map_.end()) ? &it->second.slot : nullptr;
}

std::size_t GlyphAtlas::evict_lru()
{
    const AtlasKey victim = lru_.back();
    lru_.pop_back();
    const auto it = map_.find(victim);
    const std::size_t cell = it->second.cell;
    map_.erase(it);
    ++evictions_;
    return cell;
}

void GlyphAtlas::clear_cell(int px, int py)
{
    for (int y = 0; y < cfg_.cell; ++y)
    {
        const std::size_t row =
            static_cast<std::size_t>(py + y) * static_cast<std::size_t>(cfg_.width)
            + static_cast<std::size_t>(px);
        std::fill(pixels_.begin() + static_cast<std::ptrdiff_t>(row),
                  pixels_.begin() + static_cast<std::ptrdiff_t>(row) + cfg_.cell, std::uint8_t{0});
    }
}

void GlyphAtlas::blit(const GlyphCoverage& cov, int px, int py)
{
    const std::size_t need =
        static_cast<std::size_t>(cov.width) * static_cast<std::size_t>(cov.height);
    if (cov.width <= 0 || cov.height <= 0 || cov.pixels.size() < need)
        return;
    for (int y = 0; y < cov.height; ++y)
    {
        for (int x = 0; x < cov.width; ++x)
        {
            const std::size_t dst =
                static_cast<std::size_t>(py + y) * static_cast<std::size_t>(cfg_.width)
                + static_cast<std::size_t>(px + x);
            const std::size_t src =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(cov.width)
                + static_cast<std::size_t>(x);
            pixels_[dst] = cov.pixels[src];
        }
    }
}

const AtlasSlot* GlyphAtlas::acquire(const AtlasKey& key, const GlyphRasterizer& raster)
{
    if (const auto it = map_.find(key); it != map_.end())
    {
        // Hit: mark most-recently-used (splice keeps the stored iterator valid).
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
        return &it->second.slot;
    }

    ++rasterizations_;
    const std::optional<GlyphCoverage> cov = raster(key);
    if (!cov)
        return nullptr; // un-renderable — caller advances the pen, emits no quad

    const int avail = cfg_.cell - cfg_.padding;
    if (cov->width > avail || cov->height > avail)
        return nullptr; // larger than a cell — skip (a v1 fixed-cell limitation)

    const std::size_t cell = free_cells_.empty() ? evict_lru() : [&]
    {
        const std::size_t c = free_cells_.back();
        free_cells_.pop_back();
        return c;
    }();

    const int cx = static_cast<int>(cell % static_cast<std::size_t>(cols_));
    const int cy = static_cast<int>(cell / static_cast<std::size_t>(cols_));
    const int px = cx * cfg_.cell;
    const int py = cy * cfg_.cell;

    clear_cell(px, py); // guard against bleed from a larger evicted glyph
    blit(*cov, px, py);
    ++revision_;

    AtlasSlot slot;
    slot.x = static_cast<std::uint16_t>(px);
    slot.y = static_cast<std::uint16_t>(py);
    slot.w = static_cast<std::uint16_t>(cov->width);
    slot.h = static_cast<std::uint16_t>(cov->height);
    slot.left = cov->left;
    slot.top = cov->top;
    slot.advance = cov->advance;

    lru_.push_front(key);
    Entry entry;
    entry.slot = slot;
    entry.cell = cell;
    entry.lru_it = lru_.begin();
    const auto [it, ok] = map_.emplace(key, entry);
    (void)ok;
    return &it->second.slot;
}

} // namespace context::render::ui

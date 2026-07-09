// Texture atlas packing + UV lookup — see context/render/sprite/atlas.h.

#include "context/render/sprite/atlas.h"

#include <algorithm>
#include <cstdint>

namespace context::render::sprite
{

bool TextureAtlas::add(const std::string& name, const AtlasRegion& region)
{
    if (find(name) != nullptr)
    {
        return false; // duplicate name
    }
    // Reject a region that extends past the atlas bounds (a zero-size atlas rejects everything).
    // Compare via subtraction (width_ - region.width, guarded by region.width <= width_) rather than
    // addition so a pathological region.x/width near UINT32_MAX cannot wrap past the check.
    if (region.width == 0 || region.height == 0 || region.width > width_ ||
        region.x > width_ - region.width || region.height > height_ ||
        region.y > height_ - region.height)
    {
        return false;
    }
    regions_.emplace_back(name, region);
    return true;
}

const AtlasRegion* TextureAtlas::find(const std::string& name) const
{
    for (const auto& entry : regions_)
    {
        if (entry.first == name)
        {
            return &entry.second;
        }
    }
    return nullptr;
}

UVRect TextureAtlas::uv(const AtlasRegion& region) const
{
    if (width_ == 0 || height_ == 0)
    {
        return UVRect{};
    }
    const float fw = static_cast<float>(width_);
    const float fh = static_cast<float>(height_);
    UVRect r;
    r.u0 = static_cast<float>(region.x) / fw;
    r.v0 = static_cast<float>(region.y) / fh;
    r.u1 = static_cast<float>(region.x + region.width) / fw;
    r.v1 = static_cast<float>(region.y + region.height) / fh;
    return r;
}

UVRect TextureAtlas::uv(const std::string& name) const
{
    const AtlasRegion* region = find(name);
    return region != nullptr ? uv(*region) : UVRect{};
}

PackResult pack_atlas(std::uint32_t atlas_width, std::uint32_t atlas_height,
                      const std::vector<PackItem>& items, std::uint32_t padding)
{
    PackResult result;
    result.atlas = TextureAtlas(atlas_width, atlas_height);

    // Shelf packer: place items in descending-height order (the classic heuristic that minimizes
    // wasted vertical space). Stable secondary key (original index) keeps the layout deterministic.
    std::vector<std::size_t> order(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&items](std::size_t a, std::size_t b)
                     { return items[a].height > items[b].height; });

    std::uint32_t shelf_x = padding;      // next free x on the current shelf (after left padding)
    std::uint32_t shelf_y = padding;      // top of the current shelf
    std::uint32_t shelf_height = 0;       // tallest item on the current shelf
    bool all_placed = true;

    for (std::size_t idx : order)
    {
        const PackItem& item = items[idx];
        // An item that cannot fit the atlas even on an empty shelf (padding on both sides) overflows.
        // Guard 2*padding first (padding > dim/2 <=> 2*padding > dim), then compare via subtraction so
        // a pathological item.width/height near UINT32_MAX cannot wrap the `+ 2 * padding` past atlas.
        if (item.width == 0 || item.height == 0 || padding > atlas_width / 2 ||
            padding > atlas_height / 2 || item.width > atlas_width - 2 * padding ||
            item.height > atlas_height - 2 * padding)
        {
            result.overflow.push_back(item.name);
            all_placed = false;
            continue;
        }
        // Wrap to a new shelf when the item would run past the right edge (reserve right padding).
        // Subtraction form is safe: the fit check above guarantees item.width + 2*padding <= atlas_width,
        // so atlas_width - padding - item.width >= padding >= 0.
        if (shelf_x > atlas_width - padding - item.width)
        {
            shelf_y += shelf_height + padding;
            shelf_x = padding;
            shelf_height = 0;
        }
        // Not enough vertical room left for a new shelf holding this item -> overflow.
        if (shelf_y > atlas_height - padding - item.height)
        {
            result.overflow.push_back(item.name);
            all_placed = false;
            continue;
        }
        AtlasRegion region{shelf_x, shelf_y, item.width, item.height};
        if (!result.atlas.add(item.name, region))
        {
            // A duplicate name (or a bounds edge) — surface it as overflow rather than silently drop.
            result.overflow.push_back(item.name);
            all_placed = false;
            continue;
        }
        shelf_x += item.width + padding;
        shelf_height = std::max(shelf_height, item.height);
    }

    result.ok = all_placed;
    return result;
}

} // namespace context::render::sprite

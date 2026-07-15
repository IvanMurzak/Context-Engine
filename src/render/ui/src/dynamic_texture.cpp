// The first dynamic-texture registry — see context/render/ui/dynamic_texture.h.

#include "context/render/ui/dynamic_texture.h"

#include <utility>

namespace context::render::ui
{

DynamicTextureRegistry::DynamicTextureRegistry(IDevice& device) : device_(device) {}

DynamicTextureId DynamicTextureRegistry::create_panel_target(Extent2D size)
{
    TextureDesc desc;
    desc.size = size;
    desc.format = TextureFormat::RGBA8Unorm;
    desc.render_attachment = true; // the panel UI is repainted into it each frame (RTT)
    desc.copy_src = true;          // read back for the golden dump / CI proof
    desc.texture_binding = true;   // sampled onto the world quad by the composite pass

    Entry entry;
    entry.texture = device_.create_texture(desc);
    entry.size = size;
    entries_.push_back(std::move(entry));
    // handle == index + 1, so the first allocation returns 1 and 0 stays the invalid handle.
    return static_cast<DynamicTextureId>(entries_.size());
}

ITexture* DynamicTextureRegistry::get(DynamicTextureId id) const
{
    if (id == kInvalidDynamicTexture || id > entries_.size())
    {
        return nullptr;
    }
    return entries_[id - 1].texture.get();
}

Extent2D DynamicTextureRegistry::size_of(DynamicTextureId id) const
{
    if (id == kInvalidDynamicTexture || id > entries_.size())
    {
        return Extent2D{0, 0};
    }
    return entries_[id - 1].size;
}

bool DynamicTextureRegistry::contains(DynamicTextureId id) const noexcept
{
    return id != kInvalidDynamicTexture && id <= entries_.size();
}

} // namespace context::render::ui

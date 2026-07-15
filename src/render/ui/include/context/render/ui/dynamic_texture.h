// The FIRST dynamic-texture registry (M7 a9, R-UI-003; lock D4). Owns persistent per-panel offscreen
// render targets (RTT): a world-space UI panel renders its tree into a registered texture each frame,
// and the world quad bound to it (a render_world.h UiPanel, placed by the L-39 extract) samples that
// texture. This is the "later wave" the render_world.h texture handle fields reserved — a UiPanel's
// `texture` field is a handle into THIS registry.
//
// Deliberately MINIMAL (panel targets only): the general asset-texture registry (uploaded images,
// streamed textures) is a later milestone (the T8 risk note — "don't accrete the M8 asset-texture
// registry into a9"). Each entry is allocated ONCE on the device and reused across frames (persistent),
// so a panel's RTT never reallocates mid-run — the same persistent-target discipline offscreen_scene.h
// established, generalized from a single fixed-size proof target to a keyed set of per-panel targets.

#pragma once

#include "context/render/rhi.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace context::render::ui
{

// A handle into a DynamicTextureRegistry. 0 is the reserved "unbound" handle (matching the
// render_world.h "0 = slot unused" texture-handle convention); valid handles start at 1.
using DynamicTextureId = std::uint32_t;
inline constexpr DynamicTextureId kInvalidDynamicTexture = 0;

class DynamicTextureRegistry
{
public:
    // `device` must outlive the registry (the textures are created on it and never own it).
    explicit DynamicTextureRegistry(IDevice& device);

    // Allocate a persistent RGBA8 offscreen panel target of `size` and return its handle (>= 1). The
    // texture carries render_attachment (the panel UI is drawn into it each frame), copy_src (read back
    // for the golden dump / CI proof), and texture_binding (sampled onto the world quad). Never returns
    // kInvalidDynamicTexture.
    DynamicTextureId create_panel_target(Extent2D size);

    // The texture for `id`, or nullptr for kInvalidDynamicTexture / an out-of-range handle.
    [[nodiscard]] ITexture* get(DynamicTextureId id) const;

    // The pixel size a handle was allocated at ({0,0} for an unknown handle).
    [[nodiscard]] Extent2D size_of(DynamicTextureId id) const;

    // Whether `id` names a live entry (false for kInvalidDynamicTexture / out of range).
    [[nodiscard]] bool contains(DynamicTextureId id) const noexcept;

    // How many panel targets have been allocated.
    [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        std::unique_ptr<ITexture> texture;
        Extent2D size;
    };

    IDevice& device_;
    std::vector<Entry> entries_; // handle == index + 1 (append-only; 0 stays the invalid handle)
};

} // namespace context::render::ui

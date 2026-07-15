// The engine-integrated GPU UI backend (M7 a6, R-UI-005; locks D1/D3/L-39). Implements a1's backend-
// agnostic UiProvider contract (context/packages/ui/provider.h) over the T1 RHI (context/render/rhi.h)
// — the GPU-driven presentation D3 named "the GPU backend lives in src/render/ui/".
//
// Capabilities advertised true: gpu_driver (GPU-driven, no CPU readback in the frame loop),
// damage_repaint (repaints only dirty regions), composited_transforms (transform/opacity fold at
// composite time, no relayout).
//
// ARCHITECTURE (the web reality, gpuweb#1424): damage repaint targets a PERSISTENT offscreen UI-layer
// texture with LoadOp::Load + a reduced (scissored) redraw of only the damaged quads — NEVER a partial
// repaint of the swapchain backbuffer, whose texture is a fresh, non-preserved surface every frame on
// the web. A full repaint (or the first frame) clears the layer; a damage repaint LOADs it and redraws
// only the quads overlapping the plan's dirty regions. The persistent layer is then composited full-
// screen over the 3D pass each frame — that composite-onto-the-fresh-backbuffer step is the windowed/
// web PRESENT path, which rhi.h itself defers to the ISurface/ISwapchain wave; at this offscreen tier
// the layer is the presentable result (read back for the golden + the CI proof).

#pragma once

#include "context/packages/ui/provider.h"
#include "context/render/rhi.h"
#include "context/render/ui/snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace context::render::ui
{

// The GPU-driven runtime UI provider. Holds a device reference (never owns it) and the persistent
// UI-layer texture it repaints across frames. Presentation-only w.r.t. the tree (D6 — never mutates it).
class GpuUiProvider final : public packages::ui::UiProvider
{
public:
    // `device` must outlive the provider. `surface` is the UI-layer texture size (pixels). `clear` is
    // the surface background the layer is cleared to on a full repaint — at this offscreen tier it also
    // stands in for the composited 3D-pass output behind the UI. This form allocates + owns its own
    // persistent layer texture (the screen-space HUD path, M7 a6).
    GpuUiProvider(IDevice& device, Extent2D surface, Color clear);

    // World-space panel form (M7 a9, D4): render into an EXTERNALLY-owned target instead of a self-
    // allocated layer — e.g. a dynamic-texture registry panel target (dynamic_texture.h) the world quad
    // then samples. `target` must outlive the provider and be sized `surface` with render_attachment +
    // copy_src; the provider repaints into it with the SAME persistent-layer discipline (Clear on a full
    // repaint / first frame, Load + reduced redraw on a damage repaint) but never owns or reallocates it.
    GpuUiProvider(IDevice& device, ITexture& target, Extent2D surface, Color clear);

    // R-UI-005: GPU-driven + damage repaint + composited transforms. Text features are false at T1.
    [[nodiscard]] packages::ui::Capabilities capabilities() const override;

    // Extract the tree into the L-39 double buffer, select the draw set from the plan (full ⇒ all,
    // damage ⇒ only quads overlapping the dirty regions), and repaint the persistent layer: Clear on a
    // full repaint / the first frame, Load (preserve + reduced redraw) on a damage repaint.
    void present(const packages::ui::UiTree& tree, const packages::ui::RepaintPlan& plan) override;

    // Copy the persistent UI-layer texture to a CPU buffer (tight RGBA8, rows top-first). Used by the
    // ui-hud golden dump + the CI readback proof; NOT part of the steady-state GPU frame loop. Returns
    // false when nothing has been presented yet or the readback map fails.
    [[nodiscard]] bool read_layer(std::vector<std::uint8_t>& out);

    // --- introspection (for the fake-backend structural tests) ------------------------------------
    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_presented_; }
    // Quads redrawn on the LAST present (full ⇒ every visible quad; damage ⇒ the minimal overlapping set).
    [[nodiscard]] std::size_t last_draw_count() const noexcept { return last_draw_count_; }
    // Whether the LAST present cleared the layer (full/first) vs LOADed it (a damage repaint).
    [[nodiscard]] bool last_was_clear() const noexcept { return last_was_clear_; }
    // Whether the persistent layer is live: allocated (once) and reused across presents in the self-
    // owned form, or the external target in the world-space form (persistent from construction).
    [[nodiscard]] bool layer_persistent() const noexcept { return active_layer() != nullptr; }
    [[nodiscard]] Extent2D surface() const noexcept { return surface_; }
    [[nodiscard]] const UiRenderSnapshot& snapshot() const noexcept { return buffers_.front(); }

private:
    void ensure_layer();
    // The live target the provider paints into + reads back: the external target when one was supplied
    // (world-space panel form), else the self-owned layer (nullptr until the first present allocates it).
    [[nodiscard]] ITexture* active_layer() const noexcept
    {
        return external_target_ != nullptr ? external_target_ : layer_.get();
    }
    // Repaint `draw_set` (indices into the front snapshot) into the layer; `clear` selects the first
    // pass's LoadOp (Clear ⇒ wipe to clear_ then draw; Load ⇒ preserve then draw on top).
    void repaint(const std::vector<std::uint32_t>& draw_set, bool clear);

    IDevice& device_;
    Extent2D surface_;
    Color clear_;
    std::unique_ptr<ITexture> layer_;         // self-owned persistent UI-layer texture (a6 HUD path)
    ITexture* external_target_ = nullptr;      // borrowed registry panel target (a9 world-space path)
    UiRenderDoubleBuffer buffers_;

    std::uint64_t frames_presented_ = 0;
    std::size_t last_draw_count_ = 0;
    bool last_was_clear_ = false;
};

} // namespace context::render::ui

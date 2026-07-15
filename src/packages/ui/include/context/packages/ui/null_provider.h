// The null / headless UI provider (M7 T1, R-UI-006). Logic-only, ZERO render cost: its present() does no
// rendering work at all (it never walks the tree or the plan), so UI state/logic runs headless and
// CI-assertable with no GPU. This is the R-UI-006 headless guarantee made concrete and the reference
// "does the contract, renders nothing" provider. It advertises text_shaping + bidi TRUE (a8): shaping
// lives in the HEADLESS text package (context_ui_text::measure), so the null provider computes the SAME
// glyph rects / hit-testing the GPU provider draws — the a8 placement cliff. Every RENDER capability
// (gpu_driver / damage_repaint / composited_transforms) and ime stay false.

#pragma once

#include "context/packages/ui/provider.h"

#include <cstdint>

namespace context::packages::ui
{

class NullProvider final : public UiProvider
{
public:
    // Render capabilities false (renders nothing); text_shaping + bidi TRUE — the headless shaping the
    // GPU provider also reports (a8). IME false (no OS text entry in M7 scope).
    [[nodiscard]] Capabilities capabilities() const override
    {
        Capabilities caps;
        caps.text_shaping = true;
        caps.bidi = true;
        return caps;
    }

    // Zero-cost: accept the frame and do NOTHING with the tree or the plan — no draw work, no
    // allocation, no per-node traversal. Only a debug frame counter advances (proof present was called).
    void present(const UiTree&, const RepaintPlan&) override { ++frames_presented_; }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_presented_; }

private:
    std::uint64_t frames_presented_ = 0;
};

} // namespace context::packages::ui

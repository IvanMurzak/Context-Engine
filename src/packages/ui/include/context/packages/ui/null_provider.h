// The null / headless UI provider (M7 T1, R-UI-006). Logic-only, ZERO render cost: it reports all-false
// capabilities and its present() does no rendering work at all (it never walks the tree or the plan), so
// UI state/logic runs headless and CI-assertable with no GPU. This is the R-UI-006 headless guarantee
// made concrete and the reference "does the contract, renders nothing" provider.

#pragma once

#include "context/packages/ui/provider.h"

#include <cstdint>

namespace context::packages::ui
{

class NullProvider final : public UiProvider
{
public:
    // All capabilities false: no GPU, no damage repaint, no composited transforms, no text features.
    [[nodiscard]] Capabilities capabilities() const override { return Capabilities{}; }

    // Zero-cost: accept the frame and do NOTHING with the tree or the plan — no draw work, no
    // allocation, no per-node traversal. Only a debug frame counter advances (proof present was called).
    void present(const UiTree&, const RepaintPlan&) override { ++frames_presented_; }

    [[nodiscard]] std::uint64_t frames_presented() const noexcept { return frames_presented_; }

private:
    std::uint64_t frames_presented_ = 0;
};

} // namespace context::packages::ui

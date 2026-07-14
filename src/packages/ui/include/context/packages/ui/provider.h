// The backend-agnostic UI-Provider contract (M7 T1, R-UI-002/005, locks D1/D3). A provider consumes the
// retained tree + a repaint plan and reports a Capabilities struct; the engine negotiates against those
// capabilities and falls back (a provider with NO damage support ⇒ full repaint). This header is the
// contract seam (like the JsEngine/TsToolchain precedent): the GPU backend lives in src/render/ui/ (a
// later task) and implements this interface; the null/headless provider (null_provider.h) also does.

#pragma once

#include "context/packages/ui/damage.h"
#include "context/packages/ui/ui_node.h"

#include <vector>

namespace context::packages::ui
{

class UiTree; // consumed by-reference; no definition needed in the contract header

// The capabilities a UI backend may implement — the exact R-UI-005 set. The engine reads these to
// decide how to present (and, per L-53, they are the columns of the published capability matrix).
// text_shaping / bidi / ime are declared capabilities (owner ruling O-2) — false here at the contract
// level; each concrete provider reports its own truth.
struct Capabilities
{
    bool gpu_driver = false;             // GPU-driven presentation, no CPU readback
    bool damage_repaint = false;         // repaints dirty regions only (else full repaint)
    bool composited_transforms = false;  // GPU-composited transforms/opacity, no relayout
    bool text_shaping = false;           // complex-text shaping (HarfBuzz-class)
    bool bidi = false;                   // bidirectional text
    bool ime = false;                    // OS input-method editor integration
};

// What the engine asks a provider to repaint this frame. `full_repaint` ⇒ repaint the whole surface
// (regions holds the viewport as a convenience); otherwise `regions` are the coalesced dirty rects.
struct RepaintPlan
{
    bool full_repaint = false;
    std::vector<Rect> regions;
};

// The UI backend seam. A provider reports its capabilities and presents the retained tree under a
// repaint plan. Stateless-by-contract w.r.t. the tree (it never mutates it — presentation only, D6).
class UiProvider
{
public:
    virtual ~UiProvider() = default;

    [[nodiscard]] virtual Capabilities capabilities() const = 0;
    virtual void present(const UiTree& tree, const RepaintPlan& plan) = 0;
};

// The negotiation / fallback table (D1). Given a provider's capabilities and the tree's damage, produce
// the repaint plan:
//   * no damage_repaint support  ⇒ FULL repaint (the R-UI-005 fallback — repaint everything);
//   * a `full` damage request    ⇒ FULL repaint;
//   * otherwise                  ⇒ incremental repaint over the COALESCED dirty regions (possibly none).
[[nodiscard]] RepaintPlan negotiate_repaint(const Capabilities& caps, const DamageList& damage,
                                            const Rect& viewport);

} // namespace context::packages::ui

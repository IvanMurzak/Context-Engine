// The UI extract step + its double buffer (M7 a6, R-UI-005, locks L-39/D1/D3/D6). The GPU-driven UI
// backend rides the SAME sim->render extract discipline the 3D path uses (extract.h): a READ-ONLY
// observer walks the retained UiTree into a UiRenderSnapshot of flat, backend-agnostic draw quads, and
// the snapshot rides a double buffer so the presenting frame reads a stable copy while the next frame
// is extracted — never a torn tree (L-39). UI is presentation (D6): the extract never mutates the tree.
//
// KERNEL-FREE by design (like sprite/): the snapshot is pure geometry + color over context_ui's public
// UiTree/UiNode surface, so the whole extract compiles + unit-tests with no GPU AND compiles into the
// Emscripten web target (the ui-hud golden's web leg).

#pragma once

#include "context/packages/ui/ui_node.h"

#include <cstdint>
#include <vector>

namespace context::packages::ui
{
class UiTree; // consumed by-reference; the .cpp uses the full definition
}

namespace context::render::ui
{

// One drawable UI primitive: a solid-colored axis-aligned rectangle in SURFACE space (pixels, y-down,
// top-left origin — the a1 Rect convention), already carrying its composite-time transform (D-transform)
// baked into `rect` and its effective opacity (composited down the tree) in `opacity`. `color` is the
// RGBA the backend rasterizes; at the T1 RHI tier (no blend state) the backend draws OPAQUE quads, so a
// translucent node's opacity is folded against a known backdrop at composite time (composite.h) BEFORE
// it reaches a quad — `opacity` is carried for that fold + future alpha-blended presentation.
struct UiQuad
{
    packages::ui::Rect rect;             // composited surface-space bounds (transform already applied)
    packages::ui::Color color;          // solid fill the backend rasterizes
    float opacity = 1.0f;               // effective (tree-composited) opacity — metadata for the fold
    packages::ui::NodeId node = packages::ui::kInvalidNode; // source node (introspection / a11y hook)
    std::uint32_t order = 0;            // painter/draw order (pre-order DFS index) — the sort key
};

// The immutable-per-frame draw set a UI backend presents: the drawable quads in painter order plus the
// surface the layout resolved against. Produced by extract_ui; consumed by the batch/select layer
// (batch.h) and the provider (provider.h). Backend-agnostic (the D1 seam).
struct UiRenderSnapshot
{
    std::vector<UiQuad> quads;          // visible drawables, PRE-ORDER (painter's algorithm)
    packages::ui::Rect surface;         // the viewport the tree laid out into
    std::uint64_t generation = 0;       // monotonically bumped each extract — the double-buffer stamp

    void clear() noexcept
    {
        quads.clear();
        surface = packages::ui::Rect{};
    }
};

// Extract every VISIBLE drawable node of `tree` into `out` (cleared first), in pre-order (painter's
// algorithm: a parent's quad precedes its children's, earlier siblings precede later ones — so a later
// quad overdraws an earlier one, matching the tree's paint order). A node is a drawable iff it is
// visible (style.visible AND effective opacity > 0), its computed `bounds` is non-empty, and its
// background is not fully transparent (a container with a transparent background contributes no quad but
// its children still do). Effective opacity is the product down the tree; the composite transform
// (translate + scale) is applied to the node's bounds at extract time WITHOUT a relayout (R-UI-005
// composited_transforms). An invisible node's whole subtree is skipped. `out.surface` is stamped with
// `viewport`; `out.generation` is bumped. READ-ONLY over the tree (D6).
void extract_ui(const packages::ui::UiTree& tree, const packages::ui::Rect& viewport,
                UiRenderSnapshot& out);

// The L-39 render double buffer: the extract writes the BACK snapshot; swap() publishes it as the FRONT
// (the stable copy a present reads) and rotates the old front to the back for the next extract. This is
// the UI analog of the render module's double buffer — the presenting frame never reads a
// mid-extraction snapshot.
class UiRenderDoubleBuffer
{
public:
    // Extract into the back buffer (the write target), then publish it as the new front. The back
    // buffer's generation continues from the last published frame so the stamp is a monotonic frame
    // counter across the two rotating buffers (each buffer's own counter alone would only count ITS
    // extracts), then extract_ui bumps it.
    void extract(const packages::ui::UiTree& tree, const packages::ui::Rect& viewport)
    {
        back_.generation = front_.generation;
        extract_ui(tree, viewport, back_);
        swap();
    }

    // Publish the back buffer as the front (and rotate the old front to the back). No allocation — the
    // two buffers are swapped, so the old front's capacity is reused by the next extract.
    void swap() noexcept { std::swap(front_, back_); }

    // The stable snapshot a present reads.
    [[nodiscard]] const UiRenderSnapshot& front() const noexcept { return front_; }
    // The extract's write target (exposed for tests / manual extract).
    [[nodiscard]] UiRenderSnapshot& back() noexcept { return back_; }

private:
    UiRenderSnapshot front_;
    UiRenderSnapshot back_;
};

} // namespace context::render::ui

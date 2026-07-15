// UI quad draw-order + batching + damage draw-set selection (M7 a6, R-UI-005 damage_repaint). The
// CPU draw-scheduling layer, reusing the sprite path's sort-then-coalesce discipline (sprite/batch.h,
// L-55): NEVER reorder across paint order — sort by paint order FIRST, then coalesce only ADJACENT
// same-key runs, so overdraw/transparency is preserved and batching only removes redundant state
// changes between neighbours. Plus the damage selector: the MINIMAL set of quads a damage repaint must
// redraw (the ones overlapping the plan's dirty regions), which is the whole point of damage_repaint.
//
// Pure CPU over the snapshot (snapshot.h) — GPU-free, unit-tested locally with no adapter, and the
// "damage -> minimal draw set" structural draw-count assertions live here.

#pragma once

#include "context/packages/ui/provider.h" // RepaintPlan
#include "context/render/ui/snapshot.h"

#include <cstdint>
#include <vector>

namespace context::render::ui
{

// The batch key at T1: opaque quads (the fast, blend-free path) vs translucent (the future blended
// path — a distinct GPU pipeline state once an RHI blend state lands, so a real batch boundary). At T1
// there is one solid pipeline, so a HUD of opaque panels coalesces into a single batch.
[[nodiscard]] inline bool quad_is_opaque(const UiQuad& q) noexcept
{
    return q.opacity >= 1.0f && q.color.a == 255;
}

// A coalesced run of consecutive quads (in draw order) that share the batch key and would issue as one
// draw call once the instanced/vertex-buffer path lands. `quad_indices` index into the snapshot's
// quads, in draw order.
struct UiBatch
{
    bool opaque = true;
    std::vector<std::uint32_t> quad_indices;
};

// The draw-order permutation of the snapshot's quads: indices sorted by paint order (`UiQuad::order`),
// stable. The extract already emits pre-order, so this is the identity in practice — but it is a real
// stable sort so the schedule is correct even if a caller appends quads out of order.
[[nodiscard]] std::vector<std::uint32_t> sort_ui_draw_order(const UiRenderSnapshot& snapshot);

// Coalesce the draw order into batches: walk the sorted quads and open a new batch whenever the batch
// key (opaque/translucent) changes from the previous quad, otherwise extend the current batch.
// Concatenating the batches' `quad_indices` reproduces the full draw order. Empty input ⇒ no batches.
[[nodiscard]] std::vector<UiBatch> build_ui_batches(const UiRenderSnapshot& snapshot);

// The MINIMAL draw set for a repaint plan, in draw order:
//   * full_repaint  ⇒ every quad (the fallback / first-frame path);
//   * otherwise     ⇒ only the quads whose composited rect intersects at least one dirty region
//                     (the damage_repaint win — a mutation of one widget redraws only what it touched).
// An empty, non-full plan (no damage) ⇒ an empty set (nothing to redraw). Half-open intersection
// (Rect::intersects), so a quad merely touching a region edge is NOT included.
[[nodiscard]] std::vector<std::uint32_t>
select_draw_set(const UiRenderSnapshot& snapshot, const packages::ui::RepaintPlan& plan);

} // namespace context::render::ui

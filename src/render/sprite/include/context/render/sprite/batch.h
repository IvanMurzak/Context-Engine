// Sprite batching + sort-layer ordering (R-2D-001, L-55) — the CPU draw-order + draw-call coalescing.
//
// Two steps, both pure CPU (unit-tested locally, no GPU):
//   1. sort_draw_order  — order sprites by (sort_layer, order_in_layer, input index). This is the
//      2D draw order (painter's algorithm): back-to-front, so later sprites overdraw earlier ones.
//   2. build_batches    — coalesce CONSECUTIVE same-atlas sprites (in draw order) into one batch, so
//      a run of sprites sharing a texture atlas becomes a single draw call.
//
// KEY design constraint (L-55, correctness over cleverness): batching must NEVER reorder sprites
// across the draw order. A global "group every sprite by atlas" batcher would minimize draw calls but
// break transparency/overdraw (a background sprite could draw on top of a foreground one). So we sort
// FIRST, then only merge ADJACENT same-atlas runs — draw order is preserved exactly; batching only
// removes redundant state changes between neighbours that already happen to share an atlas.

#pragma once

#include "context/render/sprite/sprite.h"

#include <cstdint>
#include <vector>

namespace context::render::sprite
{

// A batch: a contiguous run (in final draw order) of sprites that share an atlas and are drawn with
// one draw call. `sprite_indices` are indices into the caller's original sprite vector, in draw order.
struct SpriteBatch
{
    std::uint32_t atlas_id = 0;
    std::vector<std::uint32_t> sprite_indices;
};

// The draw-order permutation of `sprites`: a list of indices into `sprites`, ordered by sort_layer
// ascending, then order_in_layer ascending, then original index (a stable sort — authoring order is
// the final tie-break). Back-to-front painter's order.
[[nodiscard]] std::vector<std::uint32_t> sort_draw_order(const std::vector<Sprite2D>& sprites);

// Build batches over the draw order (calls sort_draw_order internally): walk the sorted sprites and
// open a new batch whenever the atlas_id changes from the previous sprite, otherwise extend the
// current batch. Returns batches in draw order; concatenating their sprite_indices reproduces the
// full draw order. An empty input yields no batches.
[[nodiscard]] std::vector<SpriteBatch> build_batches(const std::vector<Sprite2D>& sprites);

} // namespace context::render::sprite

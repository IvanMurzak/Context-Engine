// Broad-phase-pruned panel raycaster (M7 a10; L-16; R-SIM-007). Consumes the existing spatial
// broad-phase index (src/packages/spatial/) for CANDIDATE PRUNING ONLY — no kernel changes, no new
// acceleration structure: each panel-mesh triangle is inserted as a leaf keyed by a synthetic
// entity, and a pick ray is pruned to the triangles whose world AABB overlaps the ray segment's
// AABB before the exact ray-vs-triangle test (curved_panel.h) confirms the nearest hit.
//
// WHY A SIBLING LIB (not the context_ui foundation): SpatialIndex's public API is keyed by
// context::kernel::Entity, so context_spatial PUBLIC-links context_kernel. Pulling that into the
// pure-stdlib context_ui foundation would break its "no sim link" charter (its README) — so, exactly
// like context_ui_input (which links context_input for the a3 session seam), the spatial-backed
// raycaster is a SEPARATE static lib. The pure ray/UV math + the linear-scan oracle stay in
// context_ui (curved_panel.h); only the acceleration wrapper lives here.

#pragma once

#include "context/packages/spatial/spatial_index.h"
#include "context/packages/ui/curved_panel.h"

#include <cstddef>
#include <cstdint>

namespace context::packages::ui
{

// A panel mesh wrapped in a broad-phase spatial index for accelerated picking. Build once per mesh
// (the mesh is copied in), then raycast many pointer rays against it: each raycast prunes candidate
// triangles via the index, then runs the exact ray-vs-triangle test only on the survivors. The
// result is IDENTICAL to curved_panel.h's raycast_panel_mesh (the pruning changes cost, not the
// answer — asserted in the tests).
class PanelMeshRaycaster
{
public:
    // Build the index over `mesh`'s triangles. Each triangle becomes a leaf whose bounds are its
    // tight world AABB, keyed by a synthetic entity carrying the triangle index. `mesh` is copied.
    explicit PanelMeshRaycaster(PanelMesh mesh);

    // Broad-phase-pruned nearest-hit raycast. `max_dist` bounds the pick segment (origin ->
    // origin + max_dist * normalized-ish dir) used to build the query AABB — large enough to reach
    // the panel; it does not affect which triangle wins, only the pruning box. Returns the same hit
    // curved_panel.h's raycast_panel_mesh would, computed over the pruned candidate set.
    [[nodiscard]] PanelRayHit raycast(const Ray& ray, float max_dist = 1.0e4f) const;

    // The full pointer pipeline (hit -> UV -> panel coords), pruned. nullopt on a miss.
    [[nodiscard]] std::optional<Vec2> raycast_pointer(const Ray& ray, float panel_w, float panel_h,
                                                      UvWrap wrap = UvWrap::Clamp,
                                                      float max_dist = 1.0e4f) const;

    // Broad-phase nodes visited by the MOST RECENT raycast (spatial's query cost metric) — a
    // sub-linear-scaling proof asserts this stays far below the triangle count as the mesh grows.
    [[nodiscard]] std::size_t last_query_visited_nodes() const noexcept;

    // Candidate triangles the MOST RECENT raycast's broad phase returned (before the exact test).
    [[nodiscard]] std::size_t last_query_candidate_count() const noexcept;

    [[nodiscard]] std::size_t triangle_count() const noexcept { return mesh_.triangles.size(); }

private:
    PanelMesh mesh_;
    spatial::SpatialIndex index_;
    mutable std::size_t last_visited_ = 0;
    mutable std::size_t last_candidates_ = 0;
};

} // namespace context::packages::ui

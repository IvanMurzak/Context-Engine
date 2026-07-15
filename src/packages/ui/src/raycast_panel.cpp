// Broad-phase-pruned panel raycaster (M7 a10; L-16; R-SIM-007). Uses spatial::SpatialIndex for
// candidate pruning only; curved_panel.h owns the exact ray-vs-triangle test. See raycast_panel.h.

#include "context/packages/ui/raycast_panel.h"

#include <algorithm>
#include <cmath>

namespace context::packages::ui
{
namespace
{

// A synthetic entity for triangle index `i` (generation 1 so it is valid; index carries the triangle
// slot). The index is single-panel-scoped, so a plain index->entity map is exact.
[[nodiscard]] kernel::Entity tri_entity(std::uint32_t i) noexcept
{
    return kernel::Entity{i, 1u};
}

// The tight world AABB of one triangle.
[[nodiscard]] spatial::Aabb tri_aabb(const PanelMesh& mesh, const std::array<std::uint32_t, 3>& tri)
{
    const Vec3& a = mesh.vertices[tri[0]].pos;
    const Vec3& b = mesh.vertices[tri[1]].pos;
    const Vec3& c = mesh.vertices[tri[2]].pos;
    spatial::Aabb box;
    box.min = spatial::Vec3{std::min({a.x, b.x, c.x}), std::min({a.y, b.y, c.y}),
                            std::min({a.z, b.z, c.z})};
    box.max = spatial::Vec3{std::max({a.x, b.x, c.x}), std::max({a.y, b.y, c.y}),
                            std::max({a.z, b.z, c.z})};
    return box;
}

} // namespace

PanelMeshRaycaster::PanelMeshRaycaster(PanelMesh mesh) : mesh_(std::move(mesh))
{
    for (std::uint32_t i = 0; i < mesh_.triangles.size(); ++i)
    {
        const std::array<std::uint32_t, 3>& tri = mesh_.triangles[i];
        if (tri[0] >= mesh_.vertices.size() || tri[1] >= mesh_.vertices.size() ||
            tri[2] >= mesh_.vertices.size())
        {
            continue; // a malformed index is never indexed (mirrors the linear scan's skip)
        }
        index_.insert(tri_entity(i), tri_aabb(mesh_, tri));
    }
}

PanelRayHit PanelMeshRaycaster::raycast(const Ray& ray, float max_dist) const
{
    // Broad phase: build the AABB of the pick segment [origin, origin + max_dist * dir] (a tiny pad
    // keeps a zero-width axis from degenerating the overlap test), query the index for candidates.
    // (`seg_far`, not `far` — `far` is a <windows.h> macro on the MSVC CI leg.)
    const Vec3 seg_far{ray.origin.x + ray.dir.x * max_dist, ray.origin.y + ray.dir.y * max_dist,
                       ray.origin.z + ray.dir.z * max_dist};
    constexpr float kPad = 1.0e-4f;
    spatial::Aabb seg;
    seg.min = spatial::Vec3{std::min(ray.origin.x, seg_far.x) - kPad,
                            std::min(ray.origin.y, seg_far.y) - kPad,
                            std::min(ray.origin.z, seg_far.z) - kPad};
    seg.max = spatial::Vec3{std::max(ray.origin.x, seg_far.x) + kPad,
                            std::max(ray.origin.y, seg_far.y) + kPad,
                            std::max(ray.origin.z, seg_far.z) + kPad};

    std::vector<kernel::Entity> candidates;
    index_.query_aabb(seg, candidates);
    last_visited_ = index_.last_query_visited_nodes();
    last_candidates_ = candidates.size();

    // Narrow phase: the exact ray-vs-triangle test on the survivors, keeping the nearest hit.
    PanelRayHit best;
    float best_t = 0.0f;
    for (const kernel::Entity& e : candidates)
    {
        const std::uint32_t i = e.index;
        if (i >= mesh_.triangles.size())
        {
            continue;
        }
        const std::array<std::uint32_t, 3>& tri = mesh_.triangles[i];
        const PanelVertex& va = mesh_.vertices[tri[0]];
        const PanelVertex& vb = mesh_.vertices[tri[1]];
        const PanelVertex& vc = mesh_.vertices[tri[2]];
        float t = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (!ray_triangle_intersect(ray, va.pos, vb.pos, vc.pos, t, u, v))
        {
            continue;
        }
        if (best.hit && t >= best_t)
        {
            continue;
        }
        const float w = 1.0f - u - v;
        best.hit = true;
        best.triangle = i;
        best.t = t;
        best.bary = {w, u, v};
        best.uv = Vec2{w * va.uv.x + u * vb.uv.x + v * vc.uv.x,
                       w * va.uv.y + u * vb.uv.y + v * vc.uv.y};
        best_t = t;
    }
    return best;
}

std::optional<Vec2> PanelMeshRaycaster::raycast_pointer(const Ray& ray, float panel_w, float panel_h,
                                                        UvWrap wrap, float max_dist) const
{
    const PanelRayHit hit = raycast(ray, max_dist);
    if (!hit.hit)
    {
        return std::nullopt;
    }
    return uv_to_panel_coords(hit.uv, panel_w, panel_h, wrap);
}

std::size_t PanelMeshRaycaster::last_query_visited_nodes() const noexcept
{
    return last_visited_;
}

std::size_t PanelMeshRaycaster::last_query_candidate_count() const noexcept
{
    return last_candidates_;
}

} // namespace context::packages::ui

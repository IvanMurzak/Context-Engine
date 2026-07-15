// Curved-surface world-space UI ray math + panel-space mapping (M7 a10; L-16). Pure stdlib — no
// kernel, no spatial (the broad-phase-pruned raycaster is the sibling raycast_panel.cpp). See
// curved_panel.h for the UV convention + design notes.

#include "context/packages/ui/curved_panel.h"

#include <cmath>

namespace context::packages::ui
{
namespace
{

[[nodiscard]] Vec3 sub(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b) noexcept
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] float dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

} // namespace

bool ray_triangle_intersect(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c, float& t,
                            float& u, float& v) noexcept
{
    // Moeller-Trumbore, double-sided. det carries the winding sign; we do NOT cull on it, so a panel
    // is picked from either face (a UI panel has no meaningful back-face-cull semantics).
    constexpr float kEps = 1e-8f;
    const Vec3 e1 = sub(b, a);
    const Vec3 e2 = sub(c, a);
    const Vec3 p = cross(ray.dir, e2);
    const float det = dot(e1, p);
    if (det > -kEps && det < kEps)
    {
        return false; // ray parallel to the triangle plane
    }
    const float inv_det = 1.0f / det;
    const Vec3 tv = sub(ray.origin, a);
    u = dot(tv, p) * inv_det;
    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }
    const Vec3 q = cross(tv, e1);
    v = dot(ray.dir, q) * inv_det;
    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }
    t = dot(e2, q) * inv_det;
    return t > kEps; // strictly in front of the origin
}

PanelRayHit raycast_panel_mesh(const Ray& ray, const PanelMesh& mesh)
{
    PanelRayHit best;
    float best_t = 0.0f;
    for (std::uint32_t i = 0; i < mesh.triangles.size(); ++i)
    {
        const std::array<std::uint32_t, 3>& tri = mesh.triangles[i];
        if (tri[0] >= mesh.vertices.size() || tri[1] >= mesh.vertices.size() ||
            tri[2] >= mesh.vertices.size())
        {
            continue; // a malformed index never faults the scan
        }
        const PanelVertex& va = mesh.vertices[tri[0]];
        const PanelVertex& vb = mesh.vertices[tri[1]];
        const PanelVertex& vc = mesh.vertices[tri[2]];
        float t = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (!ray_triangle_intersect(ray, va.pos, vb.pos, vc.pos, t, u, v))
        {
            continue;
        }
        if (best.hit && t >= best_t)
        {
            continue; // keep the nearest hit
        }
        const float w = 1.0f - u - v; // barycentric weight of vertex a
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

Vec2 uv_to_panel_coords(const Vec2& uv, float panel_w, float panel_h, UvWrap wrap) noexcept
{
    auto resolve = [wrap](float c) noexcept -> float
    {
        if (wrap == UvWrap::Repeat)
        {
            return c - std::floor(c); // fractional part in [0,1)
        }
        if (c < 0.0f)
        {
            return 0.0f;
        }
        if (c > 1.0f)
        {
            return 1.0f;
        }
        return c;
    };
    return Vec2{resolve(uv.x) * panel_w, resolve(uv.y) * panel_h};
}

std::optional<Vec2> raycast_panel_pointer(const Ray& ray, const PanelMesh& mesh, float panel_w,
                                          float panel_h, UvWrap wrap)
{
    const PanelRayHit hit = raycast_panel_mesh(ray, mesh);
    if (!hit.hit)
    {
        return std::nullopt;
    }
    return uv_to_panel_coords(hit.uv, panel_w, panel_h, wrap);
}

PanelMesh build_cylinder_panel_mesh(std::uint32_t segments, float radius, float height,
                                    float arc_radians, const Vec3& center)
{
    PanelMesh mesh;
    if (segments == 0)
    {
        segments = 1;
    }
    const float half_h = height * 0.5f;
    mesh.vertices.reserve(static_cast<std::size_t>(segments + 1) * 2u);
    // Two verts per column boundary (top, bottom), left -> right across the arc.
    for (std::uint32_t i = 0; i <= segments; ++i)
    {
        const float frac = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = -arc_radians * 0.5f + arc_radians * frac;
        const float x = center.x + radius * std::sin(angle);
        const float z = center.z + radius * std::cos(angle);
        PanelVertex top;
        top.pos = Vec3{x, center.y + half_h, z};
        top.uv = Vec2{frac, 0.0f};
        PanelVertex bottom;
        bottom.pos = Vec3{x, center.y - half_h, z};
        bottom.uv = Vec2{frac, 1.0f};
        mesh.vertices.push_back(top);
        mesh.vertices.push_back(bottom);
    }
    // Two triangles per column quad: (top_i, bottom_i, bottom_{i+1}) + (top_i, bottom_{i+1}, top_{i+1}).
    mesh.triangles.reserve(static_cast<std::size_t>(segments) * 2u);
    for (std::uint32_t i = 0; i < segments; ++i)
    {
        const std::uint32_t top_i = i * 2u;
        const std::uint32_t bot_i = i * 2u + 1u;
        const std::uint32_t top_n = (i + 1u) * 2u;
        const std::uint32_t bot_n = (i + 1u) * 2u + 1u;
        mesh.triangles.push_back({top_i, bot_i, bot_n});
        mesh.triangles.push_back({top_i, bot_n, top_n});
    }
    return mesh;
}

} // namespace context::packages::ui

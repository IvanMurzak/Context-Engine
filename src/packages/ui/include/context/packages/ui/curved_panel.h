// Curved-surface world-space UI: the headless, provider-agnostic ray math + panel-space mapping (M7
// a10, R-UI-003; L-16 raycast->UV->events, non-XR half; owner ruling d). This is the "hit -> UV ->
// panel coords" pipeline a9's flat-quad binding is extended to arbitrary UV-mapped meshes with.
//
// GREENFIELD (verified against the repo): no raycast API existed anywhere (spatial exposes only
// broad-phase query_aabb/query_radius; physics3d has sphere/box colliders only) and there was no
// runtime mesh/UV seam. This header BUILDS the exact ray-vs-triangle intersection + barycentric UV
// interpolation + the UV->panel-space mapping; the broad-phase candidate pruning that rides
// spatial's SpatialIndex lives in the SIBLING lib raycast_panel.h (which needs the kernel-backed
// index), so this foundation piece stays PURE STDLIB (no kernel / no spatial link) — the same
// charter the context_ui foundation keeps (its README) and the a2 hit-test / a3 routing surface
// this feeds into keep. A PanelMesh is deliberately PANEL-SCOPED (positions + one UV set + triangle
// indices): it is NOT the M8 asset-mesh registry (T8 risk note).
//
// UV convention: (0,0) is the top-left texel of the panel's dynamic texture, matching the a9
// worldpanel_scene shader and the a1 surface space (Rect, y-down, top-left origin, snapshot.h). So
// UV -> panel coords is a straight (u*w, v*h) with NO y-flip — the panel content tree lays out into
// Rect{0,0,w,h} and a hit's UV addresses directly into it (the a2 hit_test surface).

#pragma once

#include "context/packages/ui/ui_node.h" // Vec2 (UV / panel-space point), Rect

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace context::packages::ui
{

// A 3D point / vector in world space (presentation floats, D6 — never hashed). The ui foundation has
// only Vec2 (ui_node.h); ray math needs a 3D companion. Component-wise semantics only.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// A picking ray in world space. `dir` need NOT be unit length — `t` is parametric along `dir`
// (hit point == origin + t*dir), so the caller keeps whatever scale is convenient (an ortho picker
// passes a straight -Z direction; a perspective picker an unprojected direction).
struct Ray
{
    Vec3 origin;
    Vec3 dir{0.0f, 0.0f, -1.0f};
};

// One mesh vertex: a world-space position plus its UV coordinate into the panel's texture (0..1).
struct PanelVertex
{
    Vec3 pos;
    Vec2 uv;
};

// A panel-scoped UV-mapped mesh: world-space vertices (position + UV) and triangle indices into
// them. Minimal by design — one UV set, no materials/normals/tangents — because a panel binds ONE
// dynamic texture to ONE UV set (raycast_panel.h / render/ui panel_mesh.h). NOT the M8 asset-mesh
// registry: keep it panel-scoped.
struct PanelMesh
{
    std::vector<PanelVertex> vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;

    [[nodiscard]] bool empty() const noexcept { return triangles.empty(); }
};

// How a UV coordinate outside [0,1] is resolved to a panel-space point.
enum class UvWrap
{
    Clamp, // clamp to [0,1] — the edge texel extends (the default; a panel has finite content)
    Repeat // wrap by fractional part — u - floor(u) (a tiling panel)
};

// The result of raycasting a PanelMesh: which triangle was hit, the parametric distance, the
// barycentric weights (of the triangle's a/b/c vertices), and the interpolated UV. `hit == false`
// leaves the other fields at their defaults (the ray missed every triangle).
struct PanelRayHit
{
    bool hit = false;
    std::uint32_t triangle = 0;      // index into PanelMesh::triangles of the nearest hit
    float t = 0.0f;                  // hit point == ray.origin + t*ray.dir (t > 0, in front)
    std::array<float, 3> bary{};     // barycentric weights of the triangle's [a,b,c] verts (sum 1)
    Vec2 uv;                         // barycentric-interpolated UV at the hit
};

// --- ray-vs-triangle (Moeller-Trumbore) ----------------------------------------------------------
// Intersect `ray` with the triangle (a,b,c). On a hit in FRONT of the origin, writes the parametric
// distance `t` (hit == origin + t*dir) and the barycentric weights `u`/`v` of b/c (the weight of a
// is 1-u-v) and returns true; otherwise returns false. Double-sided (winding-independent): a panel
// is picked from either face. A ray parallel to the triangle plane, or a hit behind the origin,
// misses.
[[nodiscard]] bool ray_triangle_intersect(const Ray& ray, const Vec3& a, const Vec3& b, const Vec3& c,
                                          float& t, float& u, float& v) noexcept;

// Raycast the whole mesh: the NEAREST triangle the ray hits in front of the origin (smallest t),
// with its barycentric-interpolated UV. Linear scan over every triangle — O(triangles). The
// broad-phase-pruned form (spatial candidate pruning) is raycast_panel.h; this exact scan is both
// the correctness oracle that form is validated against AND the fallback for a tiny mesh.
[[nodiscard]] PanelRayHit raycast_panel_mesh(const Ray& ray, const PanelMesh& mesh);

// --- UV -> panel-space mapping -------------------------------------------------------------------
// Map a UV coordinate to a panel-space point (pixels, y-down, top-left origin — the a1 surface / a2
// hit_test space). `panel_w`/`panel_h` are the panel content surface size (the Rect the content tree
// laid out into). Out-of-[0,1] UV is resolved per `wrap` (Clamp = edge extend, Repeat = tile).
[[nodiscard]] Vec2 uv_to_panel_coords(const Vec2& uv, float panel_w, float panel_h,
                                      UvWrap wrap = UvWrap::Clamp) noexcept;

// The full pointer pipeline: raycast the mesh, then (on a hit) map the hit UV to panel-space coords.
// Returns nullopt when the ray misses the mesh (the caller treats a miss exactly as a flat panel's
// off-quad pointer — swallowed when modal, fall-through when an overlay: a3's route_pointer path).
[[nodiscard]] std::optional<Vec2> raycast_panel_pointer(const Ray& ray, const PanelMesh& mesh,
                                                        float panel_w, float panel_h,
                                                        UvWrap wrap = UvWrap::Clamp);

// --- a cylinder-class curved panel mesh ----------------------------------------------------------
// Build a vertical cylinder-SECTION panel mesh (the "cylinder-class curved mesh" the DoD names): a
// single-row strip of `segments` quads (2 triangles each) wrapping an arc of `arc_radians` about the
// Y axis, symmetric about +Z (angle 0 faces the camera; a convex panel bulging toward the viewer).
// A vertex at column i (i in [0,segments]) sits at angle = -arc/2 + arc*i/segments:
//   x = center.x + radius*sin(angle), z = center.z + radius*cos(angle), y = center.y +/- height/2.
// UVs run u = i/segments left->right (0..1) and v = 0 (top) .. 1 (bottom). `segments` >= 1,
// `arc_radians` in (0, 2*pi). Straight-on the projected columns foreshorten toward the edges — the
// visible curvature. Pure geometry (no GPU): the golden scene rasterizes it, the raycaster picks it.
[[nodiscard]] PanelMesh build_cylinder_panel_mesh(std::uint32_t segments, float radius, float height,
                                                  float arc_radians, const Vec3& center);

} // namespace context::packages::ui

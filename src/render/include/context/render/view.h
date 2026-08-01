// The render-side Camera/View abstraction (M9 e11a; design m9-editor D5) -- one viewport's camera,
// and the projection math a viewport needs in BOTH directions.
//
// D5: N simultaneous viewports, two types (Scene = the editor camera, Game = the runtime camera),
// any viewport in any window. A View is the render-side value type that describes ONE of them:
// where the camera is, how it projects, whether it frames the world in 2D or 3D, which kind of
// viewport it belongs to, and which viewport that is. It is a plain value -- copied into the L-39
// render snapshot by a later child, never a handle into the sim World.
//
// The piece that did not exist anywhere in the tree before this header is the INVERSE direction.
// The M4 paths only ever needed world -> clip (sprite::project_point, lit::transform_point through
// a view-projection); a scene viewport needs clip -> world, because that is what turns a click at a
// pixel into a ray you can intersect the world with. unproject() and pick_ray() are that direction;
// e11g consumes pick_ray to drive `editor select`.
//
// SCOPE (e11a): value type + math only. No render target, no render pass, no extract change, no
// shell wiring, no spatial index -- those are later children of the e11 viewport task.
//
// Conventions:
//   * World / view / clip conventions are context/render/math.h's (right-handed, y-up, view -Z
//     forward, clip x,y in [-1,1] y-UP and z in [0,1]).
//   * REGION PIXELS are the OTHER convention and the two are easy to conflate: physical pixels,
//     region-relative, TOP-LEFT origin, y-DOWN -- exactly what an OS pointer reports and what the
//     Shell's PointerDispatch::region_position already carries. pick_ray() owns that flip; nothing
//     else in this header speaks pixels.
//   * The framed ASPECT RATIO is never stored on a View. It belongs to the target the view is
//     rendered into, so every entry point below takes that target's extent and derives it. Storing
//     it too would be the classic pair that drifts (the reasoning dpi.h spells out for DPI vs its
//     scale factor), and a View whose aspect disagreed with the region it was picked in would put
//     the pick ray somewhere the user did not click.

#pragma once

#include "context/render/math.h"
#include "context/render/render_world.h"
#include "context/render/rhi.h"

#include <cstdint>

namespace context::render
{

// How a view frames the world. 2D is an orthographic camera (the R-2D-001 / L-55 first-class 2D
// path); 3D is a perspective camera.
enum class ViewMode
{
    two_d,
    three_d,
};

// Which kind of viewport this view belongs to (D5). Scene = the editor's own camera, with edit-time
// overlays; Game = the runtime camera of the play session, with none.
enum class ViewType
{
    scene,
    game,
};

// The projection parameters. Which fields matter depends on ViewMode: three_d reads
// `fov_y_radians`, two_d reads `ortho_half_height`; both read the depth range. Keeping one struct
// (rather than a variant) is deliberate -- toggling a viewport between 2D and 3D must not discard
// the framing the other mode had.
struct Projection
{
    // three_d: the FULL vertical field of view, in RADIANS. Default ~60 degrees.
    float fov_y_radians = 1.0471976f;
    // two_d: HALF the world-space height the view frames; the framed width follows the target's
    // aspect ratio. Mirrors sprite::Camera2D's half_height.
    float ortho_half_height = 1.0f;
    // The depth range, measured along view -Z (forward). A 2D view typically sets near_z = 0 so the
    // z = 0 sprite plane lands at clip depth 0, as sprite::Camera2D's defaults do.
    float near_z = 0.1f;
    float far_z = 1000.0f;
};

// One viewport's camera. `transform` is the camera's WORLD placement, reusing the render snapshot's
// own Transform (position + rotation quaternion + scale) so a later child can carry a View in
// RenderSnapshot with no conversion. A camera's SCALE is deliberately ignored by view_matrix() --
// the projection owns framing, so scaling a camera means nothing and honouring it would only make
// the view matrix non-orthonormal.
struct View
{
    Transform transform{};
    Projection projection{};
    ViewMode mode = ViewMode::three_d;
    ViewType type = ViewType::scene;
    // Which viewport this view renders. 0 = unassigned, the same "0 = slot unused" convention the
    // snapshot's mesh/texture handles use. The Shell's string region id maps onto it at the binding
    // seam, so nothing here needs to allocate a string per frame.
    //
    // WARNING -- this is a render-side SLOT, session-local and never persisted. It is NOT the daemon's
    // viewport id, which is a std::string (EditorSessionState::set_camera / the "viewportId" JSON
    // field), nor the Shell's string region id. Same word, different identifier: the binding layer
    // owns the map between them, and nothing may derive one from the other.
    std::uint32_t viewport_id = 0;
};

// A world-space ray. `direction` is normalized (zero-length only when the view was degenerate).
struct Ray
{
    Vec3 origin{};
    Vec3 direction{};
};

// A pixel inside a viewport region: PHYSICAL, REGION-relative, top-left origin, y-DOWN. Signed
// because a captured drag legitimately leaves the region it started in -- the same reason
// shell::PointI is signed, which is the type this mirrors on the render side (render must not
// depend on the Shell).
struct RegionPoint
{
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// width / height of `target`, or 1.0 for a degenerate (zero-extent) target.
[[nodiscard]] float aspect_ratio(Extent2D target);

// The VIEW matrix: world -> view. The inverse of the camera's world placement, built directly
// (transpose the rotation, negate the rotated translation) rather than through inverse(), because
// a rigid transform's inverse is exact and needs no cofactor expansion.
[[nodiscard]] Mat4 view_matrix(const View& view);

// The PROJECTION matrix: view -> clip, for a view rendered into a `target`-sized surface.
// ViewMode::three_d builds a perspective frustum; two_d an orthographic box centred on the camera,
// `ortho_half_height` tall and aspect-times-that wide.
[[nodiscard]] Mat4 projection_matrix(const View& view, Extent2D target);

// projection_matrix(view, target) * view_matrix(view): world -> clip in one matrix.
[[nodiscard]] Mat4 view_proj(const View& view, Extent2D target);

// Project a WORLD point to NDC (x,y in [-1,1] y-up, z in [0,1] inside the frustum). The perspective
// divide is applied.
[[nodiscard]] Vec3 project(const View& view, Extent2D target, Vec3 world_point);

// Unproject an NDC point back to WORLD -- the inverse of project(), and the direction nothing in
// the tree could do before e11a. A degenerate view (whose view-projection is singular) yields a
// finite result rather than NaNs, because inverse() falls back to the identity.
[[nodiscard]] Vec3 unproject(const View& view, Extent2D target, Vec3 ndc_point);

// The world-space ray through `region_pixel` of a `region_size` viewport region: origin on the near
// plane, direction pointing into the scene. This is what turns a click into something the world can
// be intersected with (e11g). Handles the region-pixel -> NDC flip described in the header note, and
// samples the pixel CENTRE. For a perspective view the origin is (nearly) the eye and the direction
// varies per pixel; for an orthographic one the direction is constant and the ORIGIN varies -- both
// fall out of unprojecting the same pixel at two depths, so neither mode is special-cased.
[[nodiscard]] Ray pick_ray(const View& view, RegionPoint region_pixel, Extent2D region_size);

} // namespace context::render

// CPU picking (M9 editor-UX e4; D8) -- turning a world-space ray into the ENTITY it hits, over the
// same drawables the viewport pass actually draws.
//
// WHY CPU, not a GPU id-buffer (restated so it is not re-litigated -- taskflow 06 §1 / the task
// spec): this must be assertable on all three `build` legs with no GPU, this repo's headless-first
// rule. A GPU id-buffer is pixel-exact but assertable only on the single Linux render leg, and
// picking CI cannot defend is not taken. Accepted cost: worse accuracy at geometry silhouettes.
//
// WHAT IS PICKED. `RenderSnapshot::items` carry a `Transform` and a `Renderable` -- no mesh geometry
// (`Renderable::mesh_id` is an opaque handle with NO registry anywhere, viewport_pass.h's own header
// note). So "what a click hits" can only mean the same thing render_viewport_view() actually DRAWS
// for a renderable: the box PROXY (viewport_pass.h's `proxy_model` -- a unit box `[-0.5,0.5]^3`
// scaled by the item's transform and the pass's `proxy_size`, in the SAME orientation). Raycasting
// against that box, rather than a second bounding-volume convention invented here, is what keeps
// "you can only pick what you can see" true by construction rather than by two definitions agreeing.
//
// THE MATH. Each item's box is tested in OBJECT space via the box's own inverse model matrix, so
// non-uniform scale and rotation are both honoured exactly (a naive world-space sphere/AABB test
// would not be). Because different items carry different scales, an object-space parametric `t` is
// NOT comparable across items -- the object-space hit point is mapped back to WORLD space and
// "nearest" is decided by real world-space distance from the ray's own origin.
//
// SCOPE (e4): the raycast core (this header) is a pure function over snapshot data -- no GPU, no
// readback, no daemon, no Shell. Wiring a live pointer press into this and then into `editor.select`
// is context::editor::shell::panels::ViewportFeed::pick() (viewport_feed.h).

#pragma once

#include "context/kernel/entity.h"
#include "context/render/render_world.h"
#include "context/render/view.h" // render::Ray (world-space, view.h's own convention)

namespace context::render
{

// What one pick_nearest() call found. `hit == false` leaves `entity` at the invalid handle
// (kernel::Entity's own default) and `distance` at 0 -- the ray missed every drawable (an empty
// snapshot included, trivially).
struct PickHit
{
    bool hit = false;
    kernel::Entity entity{};
    // World-space distance from `ray.origin` to the hit point, along the ray's OWN (possibly
    // non-unit) direction -- see the header note on why this, and not an object-space `t`, is what
    // "nearest" must compare.
    float distance = 0.0f;
};

// The CPU raycast (D8): the nearest drawable in `snapshot` whose box proxy `ray` intersects, at the
// SAME box size the viewport pass would draw (`proxy_size`, default matching
// `ViewportPassConfig::proxy_size`'s own default). An item whose Transform is not finite is skipped,
// exactly as the render pass skips drawing it (transform_is_finite, viewport_pass.h) -- you cannot
// pick what would not have been drawn. Pure, headless, no GPU: a function over snapshot data only.
[[nodiscard]] PickHit pick_nearest(const Ray& ray, const RenderSnapshot& snapshot,
                                    float proxy_size = 1.0f);

} // namespace context::render

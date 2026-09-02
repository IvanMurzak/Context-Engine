// CPU picking -- see context/render/picking.h for the design and the "why CPU" restatement.

#include "context/render/picking.h"

#include "context/render/math.h"
#include "context/render/viewport_pass.h" // proxy_model / transform_is_finite -- the SAME box the pass draws

#include <cmath>
#include <cstdio>
#include <limits>

namespace context::render
{
namespace
{

// The determinant floor below which a proxy's model matrix is treated as SINGULAR for picking
// purposes -- deliberately generous (well above float epsilon) because what matters here is not
// "is this literally zero" but "would inverse() take its identity fallback and hand back a matrix
// meaning something else entirely". A determinant this small already puts inverse()'s own float
// division into subnormal/overflow territory (math.h's inverse() documents exactly that case).
constexpr float kMinPickableDeterminant = 1.0e-6f;

// Ray-vs-UNIT-BOX (centred on the origin, spanning [-0.5, 0.5] on every axis) in the box's OWN
// space, via the classic slab method. `origin`/`dir` are already in that object space (the caller
// transformed the world ray there through the box's inverse model matrix). Returns false when the
// ray misses the box entirely; otherwise `t_enter`/`t_exit` bound the intersection interval along
// `dir` (t_enter may be negative -- the ray's origin is inside the box).
//
// A near-zero `dir` component means the ray runs PARALLEL to that pair of faces: the box constrains
// nothing along that axis unless `origin` already sits outside the slab, in which case the ray can
// never reach it and the whole test misses.
[[nodiscard]] bool ray_vs_unit_box(Vec3 origin, Vec3 dir, float& t_enter, float& t_exit)
{
    constexpr float kHalf = 0.5f;
    constexpr float kEpsilon = 1.0e-8f;
    float t_min = -std::numeric_limits<float>::infinity();
    float t_max = std::numeric_limits<float>::infinity();

    const float axis_origin[3] = {origin.x, origin.y, origin.z};
    const float axis_dir[3] = {dir.x, dir.y, dir.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        const float o = axis_origin[axis];
        const float d = axis_dir[axis];
        if (!(d > kEpsilon || d < -kEpsilon)) // effectively parallel to this pair of faces
        {
            if (o < -kHalf || o > kHalf)
            {
                return false; // outside the slab and never converging
            }
            continue; // this axis constrains nothing
        }
        const float inv_d = 1.0f / d;
        float t1 = (-kHalf - o) * inv_d;
        float t2 = (kHalf - o) * inv_d;
        if (t1 > t2)
        {
            const float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > t_min)
        {
            t_min = t1;
        }
        if (t2 < t_max)
        {
            t_max = t2;
        }
        if (t_min > t_max)
        {
            return false;
        }
    }
    t_enter = t_min;
    t_exit = t_max;
    return true;
}

} // namespace

PickHit pick_nearest(const Ray& ray, const RenderSnapshot& snapshot, float proxy_size)
{
    PickHit best;
    float best_distance = std::numeric_limits<float>::infinity();

    for (const RenderItem& item : snapshot.items)
    {
        if (!transform_is_finite(item.transform))
        {
            continue; // the render pass would have skipped drawing this item too
        }

        const Mat4 model = proxy_model(item.transform, proxy_size);

        // A SINGULAR (or near-singular) model -- a zero/degenerate authored scale on any axis, most
        // commonly -- has no real inverse: math.h's inverse() falls back to the IDENTITY rather than
        // an inf/NaN matrix. Testing the ray against THAT would raycast a phantom unit box at the
        // WORLD ORIGIN instead of the item's actual (drawn, if invisible) location -- a click far
        // from the item could then spuriously "hit" it, or a click ON it could miss. Skip instead,
        // exactly like the transform_is_finite() skip above: you cannot pick a box that has no real
        // shape in world space, the same "pick what is drawn" rule this file's header states.
        if (!(std::fabs(determinant(model)) > kMinPickableDeterminant))
        {
            continue;
        }
        const Mat4 inv = inverse(model);

        // Map the world ray into the box's OWN space. Transforming the origin and a SECOND point
        // (origin + direction) through the same affine matrix and subtracting cancels the
        // translation, leaving exactly the LINEAR (rotation + scale) part applied to `direction` --
        // the direction does not need its own, separately-extracted transform.
        const Vec3 obj_origin = transform_point(inv, ray.origin);
        const Vec3 obj_target = transform_point(inv, add(ray.origin, ray.direction));
        const Vec3 obj_dir = sub(obj_target, obj_origin);

        float t_enter = 0.0f;
        float t_exit = 0.0f;
        if (!ray_vs_unit_box(obj_origin, obj_dir, t_enter, t_exit))
        {
            continue;
        }
        if (t_exit < 0.0f)
        {
            continue; // the box is entirely BEHIND the ray's origin
        }
        const float t_use = t_enter > 0.0f ? t_enter : 0.0f;

        // Back to WORLD space for the one thing that must be compared across items with different
        // scales: the real distance from the ray's own origin (see the header note).
        const Vec3 local_hit = add(obj_origin, scale(obj_dir, t_use));
        const Vec3 world_hit = transform_point(model, local_hit);
        const float distance = length(sub(world_hit, ray.origin));

        if (distance < best_distance)
        {
            best_distance = distance;
            best.hit = true;
            best.entity = item.entity;
            best.distance = distance;
        }
    }

    return best;
}

std::string pick_selection_id(kernel::Entity entity)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "entity:%u:%u", static_cast<unsigned>(entity.index),
                  static_cast<unsigned>(entity.generation));
    return std::string(buf);
}

} // namespace context::render

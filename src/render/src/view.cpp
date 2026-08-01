// The render-side Camera/View abstraction -- see context/render/view.h.

#include "context/render/view.h"

#include <cmath>
#include <cstddef>

namespace context::render
{

float aspect_ratio(Extent2D target)
{
    if (is_empty(target)) // rhi.h owns this predicate precisely so every layer stops open-coding it
    {
        return 1.0f; // a zero-extent target (mid-resize, minimized) frames a square, never a NaN
    }
    return static_cast<float>(target.width) / static_cast<float>(target.height);
}

Mat4 view_matrix(const View& view)
{
    // The camera's WORLD matrix is R (rotation) followed by T (translation to `position`); the VIEW
    // matrix is its inverse, R^T * T(-position). A rotation matrix is orthonormal, so its inverse is
    // its transpose -- exact, and no cofactor expansion involved. Scale is ignored on purpose (see
    // the header): a camera does not scale the world it frames.
    const Transform& t = view.transform;
    const Mat4 r =
        rotation_from_quaternion(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]);

    Mat4 out; // identity: the 4th row/column start out correct for an affine transform
    for (int col = 0; col < 3; ++col)
    {
        for (int row = 0; row < 3; ++row)
        {
            // transpose: out(col,row) = r(row,col)
            out.m[static_cast<std::size_t>(col) * 4 + static_cast<std::size_t>(row)] =
                r.at(row, col);
        }
    }

    // Translation column = -R^T * position. `out` currently holds R^T with a zero translation, so
    // transforming the eye through it is exactly that rotation.
    const Vec3 eye{t.position[0], t.position[1], t.position[2]};
    const Vec3 rotated_eye = transform_point(out, eye);
    out.m[12] = -rotated_eye.x;
    out.m[13] = -rotated_eye.y;
    out.m[14] = -rotated_eye.z;
    return out;
}

Mat4 projection_matrix(const View& view, Extent2D target)
{
    const float aspect = aspect_ratio(target);
    const Projection& p = view.projection;
    if (view.mode == ViewMode::three_d)
    {
        // A perspective frustum needs its near plane STRICTLY in front of the eye. At near_z <= 0 the
        // depth row and the projective row coincide, so the view-projection is SINGULAR: inverse()
        // takes its identity fallback and unproject() / pick_ray() hand back the NDC point dressed
        // up as world space -- finite, so a finiteness check cannot see it, and completely wrong.
        //
        // This is reachable with no authoring error at all. Projection deliberately keeps BOTH
        // framings so a viewport can toggle between modes without discarding either, and a 2D view
        // legitimately sits at near_z = 0 (the z = 0 sprite plane at clip depth 0). Toggling that
        // same view to 3D is exactly the path. projection_matrix is where that policy belongs:
        // perspective() is the math, and math.h says so.
        const float near_z =
            (std::isfinite(p.near_z) && p.near_z > 0.0f) ? p.near_z : Projection{}.near_z;
        return perspective(p.fov_y_radians, aspect, near_z, p.far_z);
    }
    // 2D: an orthographic box centred on the camera, half_height tall, aspect-times-that wide.
    const float half_h = p.ortho_half_height;
    const float half_w = half_h * aspect;
    return ortho(-half_w, half_w, -half_h, half_h, p.near_z, p.far_z);
}

Mat4 view_proj(const View& view, Extent2D target)
{
    return mul(projection_matrix(view, target), view_matrix(view));
}

Vec3 project(const View& view, Extent2D target, Vec3 world_point)
{
    return transform_point(view_proj(view, target), world_point);
}

Vec3 unproject(const View& view, Extent2D target, Vec3 ndc_point)
{
    return transform_point(inverse(view_proj(view, target)), ndc_point);
}

Ray pick_ray(const View& view, RegionPoint region_pixel, Extent2D region_size)
{
    // Region pixels are top-left origin and y-DOWN; NDC is y-UP. The +0.5 samples the pixel CENTRE
    // rather than its top-left corner, so the centre pixel of an odd-sized region maps to NDC 0.
    const float w = region_size.width != 0u ? static_cast<float>(region_size.width) : 1.0f;
    const float h = region_size.height != 0u ? static_cast<float>(region_size.height) : 1.0f;
    const float ndc_x = 2.0f * ((static_cast<float>(region_pixel.x) + 0.5f) / w) - 1.0f;
    const float ndc_y = 1.0f - 2.0f * ((static_cast<float>(region_pixel.y) + 0.5f) / h);

    // ONE inverse, used at both depths: the near-plane point is the ray origin, and the far-plane
    // point gives its direction. This is what makes the perspective and orthographic cases the same
    // code -- perspective varies the direction, ortho varies the origin.
    const Mat4 inv = inverse(view_proj(view, region_size));
    const Vec3 near_world = transform_point(inv, Vec3{ndc_x, ndc_y, 0.0f});
    const Vec3 far_world = transform_point(inv, Vec3{ndc_x, ndc_y, 1.0f});

    Ray ray;
    ray.origin = near_world;
    ray.direction = normalize(sub(far_world, near_world));
    return ray;
}

} // namespace context::render

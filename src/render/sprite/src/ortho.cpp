// Orthographic 2D projection math — see context/render/sprite/ortho.h.

#include "context/render/sprite/ortho.h"

namespace context::render::sprite
{

Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
    // Column-major, WebGPU clip cube (x,y in [-1,1], z in [0,1]). Guard against a degenerate box so a
    // zero-extent camera yields a well-defined (identity-scale) matrix rather than a divide-by-zero.
    const float rl = (right - left) != 0.0f ? (right - left) : 1.0f;
    const float tb = (top - bottom) != 0.0f ? (top - bottom) : 1.0f;
    const float fn = (far_z - near_z) != 0.0f ? (far_z - near_z) : 1.0f;

    Mat4 out;
    out.m.fill(0.0f);
    // Diagonal scales.
    out.m[0] = 2.0f / rl;  // col 0, row 0
    out.m[5] = 2.0f / tb;  // col 1, row 1
    out.m[10] = 1.0f / fn; // col 2, row 2 (z: [near,far] -> [0,1])
    // Translation (column 3).
    out.m[12] = -(right + left) / rl;
    out.m[13] = -(top + bottom) / tb;
    out.m[14] = -near_z / fn;
    out.m[15] = 1.0f;
    return out;
}

Mat4 Camera2D::projection() const
{
    return ortho(center.x - half_width, center.x + half_width, center.y - half_height,
                 center.y + half_height, near_z, far_z);
}

Vec2 project_point(const Mat4& proj, Vec2 world)
{
    // p_clip = M * vec4(world.x, world.y, 0, 1). Only x,y are returned (the sprite plane is z=0).
    const float x = proj.at(0, 0) * world.x + proj.at(1, 0) * world.y + proj.at(3, 0);
    const float y = proj.at(0, 1) * world.x + proj.at(1, 1) * world.y + proj.at(3, 1);
    return Vec2{x, y};
}

std::array<Vec2, 4> quad_clip_corners(const Mat4& proj, Vec2 center, Vec2 size)
{
    const float hw = size.x * 0.5f;
    const float hh = size.y * 0.5f;
    // World corners: bottom-left, bottom-right, top-right, top-left (CCW, y-up).
    const Vec2 bl{center.x - hw, center.y - hh};
    const Vec2 br{center.x + hw, center.y - hh};
    const Vec2 tr{center.x + hw, center.y + hh};
    const Vec2 tl{center.x - hw, center.y + hh};
    return {project_point(proj, bl), project_point(proj, br), project_point(proj, tr),
            project_point(proj, tl)};
}

} // namespace context::render::sprite

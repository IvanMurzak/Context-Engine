// 3D math for the lit/PBR path — see lit_math.h.

#include "context/render/lit/lit_math.h"

namespace context::render::lit
{

Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a.at(k, row) * b.at(col, k);
            }
            out.m[static_cast<std::size_t>(col) * 4 + static_cast<std::size_t>(row)] = sum;
        }
    }
    return out;
}

Vec3 transform_point(const Mat4& m, Vec3 p)
{
    const float x = m.at(0, 0) * p.x + m.at(1, 0) * p.y + m.at(2, 0) * p.z + m.at(3, 0);
    const float y = m.at(0, 1) * p.x + m.at(1, 1) * p.y + m.at(2, 1) * p.z + m.at(3, 1);
    const float z = m.at(0, 2) * p.x + m.at(1, 2) * p.y + m.at(2, 2) * p.z + m.at(3, 2);
    const float w = m.at(0, 3) * p.x + m.at(1, 3) * p.y + m.at(2, 3) * p.z + m.at(3, 3);
    if (w != 0.0f && w != 1.0f)
    {
        return {x / w, y / w, z / w};
    }
    return {x, y, z};
}

Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up)
{
    const Vec3 f = normalize(sub(center, eye)); // forward (view -Z)
    const Vec3 r = normalize(cross(f, up));     // right   (view +X)
    const Vec3 u = cross(r, f);                 // true up (view +Y)

    Mat4 out;
    out.m = {r.x,           u.x,           -f.x,         0.0f, //
             r.y,           u.y,           -f.y,         0.0f, //
             r.z,           u.z,           -f.z,         0.0f, //
             -dot(r, eye),  -dot(u, eye),  dot(f, eye),  1.0f};
    return out;
}

Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
    // Maps view space onto the WebGPU clip cube: x,y in [-1,1] (y-up), z in [0,1] — the orthoZO
    // convention the sprite path's 2D ortho pins down (L-11: WebGPU depth range, not GL's [-1,1]).
    // View-space forward is -Z (look_at above), so a point `near_z` in front of the camera sits at
    // view z = -near_z and must land at clip z = 0; `far_z` at clip z = 1.
    Mat4 out;
    out.m = {2.0f / (right - left), 0.0f, 0.0f, 0.0f,                             //
             0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,                             //
             0.0f, 0.0f, -1.0f / (far_z - near_z), 0.0f,                          //
             -(right + left) / (right - left), -(top + bottom) / (top - bottom),  //
             -near_z / (far_z - near_z), 1.0f};
    return out;
}

} // namespace context::render::lit

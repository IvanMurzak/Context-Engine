// Shared CPU 3D math for the render side -- see context/render/math.h.

#include "context/render/math.h"

#include <cstddef>

namespace context::render
{
namespace
{

// The ADJUGATE of `mat` (the transposed cofactor matrix) in the same column-major layout, plus the
// determinant read off the first column's cofactors. determinant() and inverse() both go through
// this, so the two can never disagree about which matrices are singular.
struct Adjugate
{
    std::array<float, 16> adj{};
    float det = 0.0f;
};

[[nodiscard]] Adjugate adjugate_of(const Mat4& mat)
{
    const std::array<float, 16>& m = mat.m;
    std::array<float, 16> a{};

    a[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    a[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    a[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    a[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    a[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    a[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    a[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    a[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    a[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    a[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    a[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    a[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    a[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    a[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    a[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    a[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    Adjugate out;
    out.adj = a;
    out.det = m[0] * a[0] + m[1] * a[4] + m[2] * a[8] + m[3] * a[12];
    return out;
}

} // namespace

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
    out.m = {r.x,          u.x,          -f.x,        0.0f, //
             r.y,          u.y,          -f.y,        0.0f, //
             r.z,          u.z,          -f.z,        0.0f, //
             -dot(r, eye), -dot(u, eye), dot(f, eye), 1.0f};
    return out;
}

Mat4 ortho(float left, float right, float bottom, float top, float near_z, float far_z)
{
    // Maps view space onto the WebGPU clip cube: x,y in [-1,1] (y-up), z in [0,1] -- the orthoZO
    // convention the sprite path's 2D ortho pins down (L-11: WebGPU depth range, not GL's [-1,1]).
    // View-space forward is -Z (look_at above), so a point `near_z` in front of the camera sits at
    // view z = -near_z and must land at clip z = 0; `far_z` at clip z = 1.
    //
    // Each denominator is guarded exactly as sprite::ortho guards its own, so a zero-extent box
    // (a viewport mid-resize, a minimized window, a 2D camera authored with zero half-height)
    // yields a finite identity-scale matrix on the collapsed axis instead of inf/NaN.
    const float rl = (right - left) != 0.0f ? (right - left) : 1.0f;
    const float tb = (top - bottom) != 0.0f ? (top - bottom) : 1.0f;
    const float fn = (far_z - near_z) != 0.0f ? (far_z - near_z) : 1.0f;

    Mat4 out;
    out.m = {2.0f / rl, 0.0f, 0.0f, 0.0f,                             //
             0.0f, 2.0f / tb, 0.0f, 0.0f,                             //
             0.0f, 0.0f, -1.0f / fn, 0.0f,                            //
             -(right + left) / rl, -(top + bottom) / tb, -near_z / fn, 1.0f};
    return out;
}

Mat4 perspective(float fov_y_radians, float aspect, float near_z, float far_z)
{
    // perspectiveRH_ZO: right-handed, view -Z forward, clip z in [0,1]. The projective row is
    // m.at(2,3) = -1, so w' = -view_z and transform_point's divide is what produces NDC.
    //
    //     x' = (f/aspect) * view_x         y' = f * view_y            f = 1 / tan(fov_y / 2)
    //     z' = far/(near-far) * view_z + (far*near)/(near-far)        w' = -view_z
    //
    // so view_z = -near lands at NDC z 0 and view_z = -far at NDC z 1 (checked by test_math).
    const float tan_half = std::tan(0.5f * fov_y_radians);
    const float t = (std::isfinite(tan_half) && tan_half > 0.0f) ? tan_half : 1.0f;
    const float a = (std::isfinite(aspect) && aspect > 0.0f) ? aspect : 1.0f;
    const float nf = (near_z - far_z) != 0.0f ? (near_z - far_z) : -1.0f;
    const float f = 1.0f / t;

    Mat4 out;
    out.m.fill(0.0f);
    out.m[0] = f / a;                    // col 0, row 0
    out.m[5] = f;                        // col 1, row 1
    out.m[10] = far_z / nf;              // col 2, row 2
    out.m[11] = -1.0f;                   // col 2, row 3 -- w' = -view_z
    out.m[14] = (far_z * near_z) / nf;   // col 3, row 2
    out.m[15] = 0.0f;                    // col 3, row 3 -- NOT affine
    return out;
}

float determinant(const Mat4& m)
{
    return adjugate_of(m).det;
}

Mat4 inverse(const Mat4& m)
{
    const Adjugate a = adjugate_of(m);

    // A non-finite determinant (an overflowed one, or a NaN out of a poisoned matrix) must be
    // rejected HERE and not by the finiteness sweep below: 1/inf is 0, which would scale the whole
    // adjugate to a matrix of ZEROS -- finite, and silently wrong.
    if (!std::isfinite(a.det))
    {
        return Mat4{};
    }

    const float inv_det = 1.0f / a.det;
    Mat4 out;
    for (std::size_t i = 0; i < 16; ++i)
    {
        out.m[i] = a.adj[i] * inv_det;
    }

    // Everything else -- a SINGULAR matrix (det 0, so 1/det is an infinity) and a determinant that
    // is non-zero but subnormal (where 1/det overflows) -- lands as an inf or a NaN in the RESULT,
    // so one sweep of the result covers both. Checking the determinant against zero as well would
    // be a branch no input can reach on its own, i.e. one no plant could ever prove load-bearing.
    for (const float v : out.m)
    {
        if (!std::isfinite(v))
        {
            return Mat4{}; // no inverse -- identity, never inf/NaN (the header's house rule)
        }
    }
    return out;
}

Mat4 rotation_from_quaternion(float x, float y, float z, float w)
{
    const float len_sq = x * x + y * y + z * z + w * w;
    if (!std::isfinite(len_sq) || !(len_sq > 0.0f))
    {
        return Mat4{}; // a zero / non-finite quaternion is no rotation at all -- identity
    }
    const float s = 1.0f / std::sqrt(len_sq);
    const float qx = x * s;
    const float qy = y * s;
    const float qz = z * s;
    const float qw = w * s;

    Mat4 out;
    // Column c is the image of basis vector e_c (column-major: m[c * 4 + r]).
    out.m[0] = 1.0f - 2.0f * (qy * qy + qz * qz);
    out.m[1] = 2.0f * (qx * qy + qz * qw);
    out.m[2] = 2.0f * (qx * qz - qy * qw);
    out.m[3] = 0.0f;
    out.m[4] = 2.0f * (qx * qy - qz * qw);
    out.m[5] = 1.0f - 2.0f * (qx * qx + qz * qz);
    out.m[6] = 2.0f * (qy * qz + qx * qw);
    out.m[7] = 0.0f;
    out.m[8] = 2.0f * (qx * qz + qy * qw);
    out.m[9] = 2.0f * (qy * qz - qx * qw);
    out.m[10] = 1.0f - 2.0f * (qx * qx + qy * qy);
    out.m[11] = 0.0f;
    out.m[12] = 0.0f;
    out.m[13] = 0.0f;
    out.m[14] = 0.0f;
    out.m[15] = 1.0f;
    return out;
}

} // namespace context::render

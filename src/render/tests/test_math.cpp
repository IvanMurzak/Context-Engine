// CPU unit test: the shared render math (context/render/math.h) — the M9 e11a additions
// (perspective, determinant, inverse, rotation_from_quaternion) and the degenerate-input
// finiteness contract. No GPU — runs on the local dev gate under every toolchain.
//
// Fixture discipline (this milestone's most expensive recurring defect is an assertion that cannot
// fail, and camera math is unusually prone to it):
//   * The field of view is 50 degrees, NOT 90. At 90 degrees tan(fov/2) is exactly 1, so a
//     perspective() that forgot its 1/tan(fov/2) factor entirely would produce identical numbers.
//   * The aspect ratio is 16:9, NOT 1:1. At 1:1 a dropped aspect divide, and an aspect applied
//     upside-down, are both invisible.
//   * near/far are 0.25/120, NOT 0/1 or 1/2, so a swapped near/far or an off-by-one depth mapping
//     cannot coincidentally agree.
//   * The orthographic box is ASYMMETRIC ([-3,11] x [-2,9]). A symmetric box has zero translation
//     terms, so it cannot distinguish a projection with a correct principal point from one with no
//     principal-point handling at all.
//   * The matrix inverted is a perspective composed with an off-axis look_at, so it is neither
//     orthonormal nor affine — a transposed or unscaled "inverse" disagrees with the real one, and
//     test_inverse_is_not_merely_a_transpose asserts that the fixture actually has that property
//     rather than assuming it.

#include "context/render/math.h"

#include "render_test.h"

#include <cmath>
#include <limits>

using namespace context::render;

namespace
{

// Tolerances. Everything here is float32 (~7 significant decimal digits). A single projected
// coordinate costs ~8 multiply-adds, so ~1e-6 relative; the composed-matrix identity below costs a
// 4x4 multiply on top of a cofactor inverse whose terms span far/near = 480, which is where the
// error actually accumulates. MEASURED worst cases over exactly the fixtures below, on this host:
// 1.19e-7 for the direct projection asserts, 2.27e-5 for mul(M, inverse(M)) vs the identity. The
// constants sit ~17x and ~9x above those — loose enough to survive another toolchain's rounding,
// and one to four orders of magnitude BELOW the error any wrong matrix here produces (the smallest
// defect planted against these assertions moves a projected coordinate by ~0.3).
constexpr float kEps = 2.0e-6f;
constexpr float kInverseEps = 2.0e-4f;

constexpr float kFovY = 0.87266463f;  // 50 degrees
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.25f;
constexpr float kFar = 120.0f;

bool near_f(float a, float b, float eps = kEps)
{
    return std::fabs(a - b) <= eps;
}

bool all_finite(const Mat4& m)
{
    for (const float v : m.m)
    {
        if (!std::isfinite(v))
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// perspective()
// ---------------------------------------------------------------------------------------------

void test_perspective_maps_near_and_far_planes()
{
    const Mat4 proj = perspective(kFovY, kAspect, kNear, kFar);
    // View forward is -Z: the near plane sits at view z = -near and must land at NDC z 0, the far
    // plane at NDC z 1. Both are on the view axis, so x and y must be 0 as well.
    const Vec3 on_near = transform_point(proj, Vec3{0.0f, 0.0f, -kNear});
    CHECK(near_f(on_near.x, 0.0f) && near_f(on_near.y, 0.0f));
    CHECK(near_f(on_near.z, 0.0f)); // PERSPECTIVE near plane -> NDC z 0
    const Vec3 on_far = transform_point(proj, Vec3{0.0f, 0.0f, -kFar});
    CHECK(near_f(on_far.z, 1.0f)); // PERSPECTIVE far plane -> NDC z 1
}

void test_perspective_depth_is_projective_not_linear()
{
    // The hallmark of a perspective depth mapping: NDC z reaches 0.5 at the HARMONIC mean of near
    // and far (0.499 here), not at their arithmetic mean (60.1). An affine z mapping — the shape an
    // ortho-style depth row would give — puts 0.5 at the arithmetic mean, so this single assertion
    // separates the two by two orders of magnitude.
    const Mat4 proj = perspective(kFovY, kAspect, kNear, kFar);
    const float harmonic_mid = 2.0f * kNear * kFar / (kNear + kFar);
    const Vec3 mid = transform_point(proj, Vec3{0.0f, 0.0f, -harmonic_mid});
    CHECK(near_f(mid.z, 0.5f)); // PERSPECTIVE harmonic midpoint -> NDC z 0.5

    const float arithmetic_mid = 0.5f * (kNear + kFar);
    const Vec3 linear_mid = transform_point(proj, Vec3{0.0f, 0.0f, -arithmetic_mid});
    CHECK(linear_mid.z > 0.99f); // an AFFINE depth row would put this at 0.5, not near 1
}

void test_perspective_frustum_edges_and_aspect()
{
    const Mat4 proj = perspective(kFovY, kAspect, kNear, kFar);
    const float tan_half = std::tan(0.5f * kFovY);
    const float depth = 3.0f;

    // The vertical half-extent at `depth` is depth * tan(fov/2): that point is the TOP edge, NDC
    // y = +1. This pins the 1/tan(fov/2) factor — drop it and y lands at tan(fov/2) instead.
    const Vec3 top = transform_point(proj, Vec3{0.0f, depth * tan_half, -depth});
    CHECK(near_f(top.y, 1.0f)); // PERSPECTIVE vertical half-extent -> NDC y +1

    // The horizontal half-extent is that times the aspect ratio: NDC x = +1.
    const Vec3 right = transform_point(proj, Vec3{depth * tan_half * kAspect, 0.0f, -depth});
    CHECK(near_f(right.x, 1.0f)); // PERSPECTIVE horizontal half-extent -> NDC x +1

    // ...and feeding the VERTICAL half-extent horizontally must land at 1/aspect (0.5625), never at
    // 1 (no aspect handling) and never at aspect (an upside-down aspect divide).
    const Vec3 narrow = transform_point(proj, Vec3{depth * tan_half, 0.0f, -depth});
    CHECK(near_f(narrow.x, 1.0f / kAspect)); // PERSPECTIVE aspect applied to x, right way up
}

void test_perspective_degenerate_inputs_are_finite()
{
    // A viewport mid-resize, an authored zero fov, a collapsed depth range. Each must yield a
    // finite matrix (the normalize() house rule), never inf/NaN poisoning the frame.
    CHECK(all_finite(perspective(0.0f, kAspect, kNear, kFar)));      // zero fov
    CHECK(all_finite(perspective(kFovY, 0.0f, kNear, kFar)));        // zero aspect
    CHECK(all_finite(perspective(kFovY, kAspect, 5.0f, 5.0f)));      // zero depth range
    CHECK(all_finite(perspective(0.0f, 0.0f, 0.0f, 0.0f)));          // everything at once

    // ...and the fallback VALUES, not merely finiteness. A guard that substituted some other
    // constant would keep every all_finite() above green while silently reframing the scene, so
    // finiteness alone cannot tell a working guard from a broken one -- the same reason the
    // non-finite-aspect case below asserts a value rather than a finiteness sweep.
    CHECK(near_f(perspective(0.0f, kAspect, kNear, kFar).at(1, 1), 1.0f)); // zero fov -> tan_half 1
    CHECK(near_f(perspective(kFovY, kAspect, 5.0f, 5.0f).at(2, 2), -5.0f)); // zero range -> nf -1
}

void test_perspective_non_finite_aspect_falls_back_to_square()
{
    // The header promises a NON-FINITE aspect falls back, and finiteness is the wrong instrument to
    // prove it with: drop the isfinite() half of that guard and an infinite aspect still yields a
    // perfectly FINITE matrix, because f/inf is 0. It is silently a degenerate projection that
    // frames nothing. So assert the fallback's actual observable -- the same matrix an aspect of 1
    // gives -- and pin that column absolutely first, so the comparison cannot be satisfied by two
    // matrices that are both zero.
    const float f = 1.0f / std::tan(0.5f * kFovY);
    const Mat4 square = perspective(kFovY, 1.0f, kNear, kFar);
    CHECK(near_f(square.at(0, 0), f)); // an aspect of 1 leaves the x scale at 1/tan(fov/2)

    const float infinite = std::numeric_limits<float>::infinity();
    const Mat4 fallback = perspective(kFovY, infinite, kNear, kFar);
    CHECK(all_finite(fallback));
    CHECK(near_f(fallback.at(0, 0), f)); // NON-FINITE aspect falls back to 1, not to a zero scale
    CHECK(near_f(fallback.at(1, 1), square.at(1, 1)));
}

// ---------------------------------------------------------------------------------------------
// ortho() — the asymmetric (non-centred principal point) case and the degenerate guard
// ---------------------------------------------------------------------------------------------

void test_ortho_asymmetric_box_pins_principal_point()
{
    // [-3,11] x [-2,9], near 1, far 17 — deliberately NOT centred on the view axis, so the
    // translation terms are non-zero and a projection that ignored them fails here while passing
    // every symmetric-box assertion.
    const Mat4 proj = ortho(-3.0f, 11.0f, -2.0f, 9.0f, 1.0f, 17.0f);
    const Vec3 min_corner = transform_point(proj, Vec3{-3.0f, -2.0f, -1.0f});
    CHECK(near_f(min_corner.x, -1.0f) && near_f(min_corner.y, -1.0f));
    CHECK(near_f(min_corner.z, 0.0f)); // ORTHO asymmetric near corner -> NDC z 0
    const Vec3 max_corner = transform_point(proj, Vec3{11.0f, 9.0f, -17.0f});
    CHECK(near_f(max_corner.x, 1.0f) && near_f(max_corner.y, 1.0f));
    CHECK(near_f(max_corner.z, 1.0f)); // ORTHO asymmetric far corner -> NDC z 1

    // The BOX centre — (4, 3.5), not the origin — is what maps to NDC (0,0).
    const Vec3 box_centre = transform_point(proj, Vec3{4.0f, 3.5f, -1.0f});
    CHECK(near_f(box_centre.x, 0.0f) && near_f(box_centre.y, 0.0f));
    // ...and the view AXIS does not, precisely because the box is off-centre.
    const Vec3 on_axis = transform_point(proj, Vec3{0.0f, 0.0f, -1.0f});
    CHECK(std::fabs(on_axis.x) > 0.5f); // ORTHO off-centre box does NOT put the axis at NDC 0
}

void test_ortho_degenerate_box_is_finite()
{
    // The lit path's ortho() had no such guard before e11a; a 2D view authored with a zero
    // half-height, or a viewport mid-resize, reaches it now.
    CHECK(all_finite(ortho(5.0f, 5.0f, 5.0f, 5.0f, 0.0f, 0.0f)));
    CHECK(all_finite(ortho(-1.0f, 1.0f, 4.0f, 4.0f, 0.0f, 1.0f)));
}

// ---------------------------------------------------------------------------------------------
// determinant() / inverse()
// ---------------------------------------------------------------------------------------------

// The fixture matrix: an off-axis perspective view-projection. Neither orthonormal (so its inverse
// is not its transpose) nor affine (so its last row is not 0,0,0,1).
Mat4 fixture_view_proj()
{
    const Mat4 view = look_at(Vec3{7.5f, 4.25f, -3.75f}, Vec3{-1.5f, 0.75f, 2.0f},
                              Vec3{0.1f, 1.0f, 0.2f});
    return mul(perspective(kFovY, kAspect, kNear, kFar), view);
}

void test_inverse_round_trips_to_identity()
{
    const Mat4 m = fixture_view_proj();
    const Mat4 inv = inverse(m);
    const Mat4 lhs = mul(m, inv);
    const Mat4 rhs = mul(inv, m);
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            const float expected = (col == row) ? 1.0f : 0.0f;
            CHECK(near_f(lhs.at(col, row), expected, kInverseEps)); // M * inverse(M) == I
            CHECK(near_f(rhs.at(col, row), expected, kInverseEps)); // inverse(M) * M == I
        }
    }
}

void test_inverse_is_not_merely_a_transpose()
{
    // Proves the fixture can DISCRIMINATE: if this matrix were orthonormal, the assertion above
    // would hold for a transpose too and would be testing nothing. It is not — by a wide margin.
    const Mat4 m = fixture_view_proj();
    const Mat4 inv = inverse(m);
    float worst = 0.0f;
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            worst = std::fmax(worst, std::fabs(inv.at(col, row) - m.at(row, col)));
        }
    }
    CHECK(worst > 0.1f); // FIXTURE DISCRIMINATES: inverse differs from transpose
}

void test_determinant_agrees_with_invertibility()
{
    Mat4 identity;
    CHECK(near_f(determinant(identity), 1.0f)); // DETERMINANT of the identity is 1

    const float det = determinant(fixture_view_proj());
    CHECK(std::isfinite(det) && std::fabs(det) > 0.0f); // DETERMINANT of the fixture is non-zero

    Mat4 singular;
    singular.m[5] = 0.0f; // collapse one axis: rank 3
    CHECK(near_f(determinant(singular), 0.0f)); // DETERMINANT of a collapsed axis is 0

    // Every fixture above is DIAGONAL, and for a diagonal matrix the determinant is just the product
    // of the diagonal -- so all three pass against an implementation that only ever multiplies those
    // four entries, and the cofactor expansion goes unpinned. A proper rotation has determinant
    // exactly 1 AND full off-diagonal structure; the quarter turn's own diagonal product is 0.
    const float s = std::sqrt(0.5f);
    CHECK(near_f(determinant(rotation_from_quaternion(0.0f, 0.0f, s, s)), 1.0f));

    // The composed fixture is the only matrix here with a non-zero fourth column, which is the one
    // cofactor term the cases above never reach. look_at is rigid (determinant 1), so the product's
    // determinant is the projection's own.
    CHECK(near_f(determinant(fixture_view_proj()),
                 determinant(perspective(kFovY, kAspect, kNear, kFar)), 1.0e-4f));
}

void test_inverse_of_a_singular_matrix_is_the_identity()
{
    Mat4 singular;
    singular.m[5] = 0.0f;
    const Mat4 inv = inverse(singular);
    CHECK(all_finite(inv)); // SINGULAR inverse stays finite
    Mat4 identity;
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            CHECK(near_f(inv.at(col, row), identity.at(col, row))); // SINGULAR inverse is identity
        }
    }
}

void test_inverse_that_would_overflow_stays_finite()
{
    // A determinant that is non-zero but subnormal: 1/det overflows to infinity, so a guard on the
    // determinant alone is not enough — the RESULT has to be swept too.
    Mat4 tiny;
    tiny.m[0] = 1.0e-39f;
    const Mat4 subnormal_inverse = inverse(tiny);
    CHECK(all_finite(subnormal_inverse)); // SUBNORMAL determinant still yields a finite inverse

    // The identity is the strictly stronger claim, and it is available here for the same reason the
    // overflow case below asserts it: finiteness alone cannot separate the fallback from a matrix of
    // plausible-looking garbage.
    Mat4 identity;
    for (int i = 0; i < 16; ++i)
    {
        CHECK(near_f(subnormal_inverse.m[static_cast<std::size_t>(i)],
                     identity.m[static_cast<std::size_t>(i)])); // SUBNORMAL inverse is the identity
    }
}

void test_inverse_of_an_overflowing_determinant_is_the_identity()
{
    // The mirror case, and the one a result-finiteness sweep CANNOT catch: a determinant that
    // overflows to infinity makes 1/det exactly ZERO, which scales the adjugate to a matrix of
    // zeros — perfectly finite, and silently wrong. Only the determinant check sees it, which is
    // why asserting the identity here (not merely finiteness) is what makes that branch provable.
    //
    // 1e12 on the diagonal is chosen so the ADJUGATE stays finite (each entry is a product of three
    // diagonal terms, 1e36, inside float range) while the determinant — a product of four, 1e48 —
    // overflows. A larger diagonal overflows the adjugate too, and then the products are NaN rather
    // than zero, which the result sweep catches and the determinant branch is never observed. Found
    // by planting: at 1e30 the plant against that branch came back GREEN.
    Mat4 enormous;
    enormous.m[0] = 1.0e12f;
    enormous.m[5] = 1.0e12f;
    enormous.m[10] = 1.0e12f;
    enormous.m[15] = 1.0e12f;
    CHECK(!std::isfinite(determinant(enormous))); // the fixture really does overflow the determinant
    const Mat4 overflowed = inverse(enormous);
    Mat4 identity;
    for (int i = 0; i < 16; ++i)
    {
        CHECK(near_f(overflowed.m[static_cast<std::size_t>(i)],
                     identity.m[static_cast<std::size_t>(i)])); // OVERFLOWED determinant -> identity
    }
}

// ---------------------------------------------------------------------------------------------
// rotation_from_quaternion()
// ---------------------------------------------------------------------------------------------

void test_rotation_from_quaternion_quarter_turn()
{
    // +90 degrees about +Z, right-handed: +X -> +Y, +Y -> -X, +Z unchanged.
    const float s = std::sqrt(0.5f);
    const Mat4 r = rotation_from_quaternion(0.0f, 0.0f, s, s);
    const Vec3 x_axis = transform_point(r, Vec3{1.0f, 0.0f, 0.0f});
    CHECK(near_f(x_axis.x, 0.0f) && near_f(x_axis.y, 1.0f) && near_f(x_axis.z, 0.0f));
    const Vec3 y_axis = transform_point(r, Vec3{0.0f, 1.0f, 0.0f});
    CHECK(near_f(y_axis.x, -1.0f) && near_f(y_axis.y, 0.0f)); // QUARTER TURN sends +Y to -X
    const Vec3 z_axis = transform_point(r, Vec3{0.0f, 0.0f, 1.0f});
    CHECK(near_f(z_axis.z, 1.0f)); // QUARTER TURN leaves the rotation axis alone
}

void test_rotation_from_quaternion_normalizes_and_degrades()
{
    // An unnormalized authored quaternion still yields a pure rotation: the same one.
    const float s = std::sqrt(0.5f);
    const Mat4 unit = rotation_from_quaternion(0.0f, 0.0f, s, s);
    const Mat4 scaled = rotation_from_quaternion(0.0f, 0.0f, 3.0f * s, 3.0f * s);
    for (int i = 0; i < 16; ++i)
    {
        CHECK(near_f(scaled.m[static_cast<std::size_t>(i)], unit.m[static_cast<std::size_t>(i)]));
    }

    // A zero quaternion is no rotation at all: identity, never NaN.
    const Mat4 zero = rotation_from_quaternion(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(all_finite(zero)); // ZERO QUATERNION stays finite
    Mat4 identity;
    for (int i = 0; i < 16; ++i)
    {
        CHECK(near_f(zero.m[static_cast<std::size_t>(i)],
                     identity.m[static_cast<std::size_t>(i)])); // ZERO QUATERNION is the identity
    }
}

} // namespace

int main()
{
    test_perspective_maps_near_and_far_planes();
    test_perspective_depth_is_projective_not_linear();
    test_perspective_frustum_edges_and_aspect();
    test_perspective_degenerate_inputs_are_finite();
    test_perspective_non_finite_aspect_falls_back_to_square();
    test_ortho_asymmetric_box_pins_principal_point();
    test_ortho_degenerate_box_is_finite();
    test_inverse_round_trips_to_identity();
    test_inverse_is_not_merely_a_transpose();
    test_determinant_agrees_with_invertibility();
    test_inverse_of_a_singular_matrix_is_the_identity();
    test_inverse_that_would_overflow_stays_finite();
    test_inverse_of_an_overflowing_determinant_is_the_identity();
    test_rotation_from_quaternion_quarter_turn();
    test_rotation_from_quaternion_normalizes_and_degrades();
    RENDER_TEST_MAIN_END();
}

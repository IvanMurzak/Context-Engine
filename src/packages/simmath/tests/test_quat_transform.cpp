// Fixed-point quaternion + TRS transform: rotation algebra + point mapping + composition (R-QA-013).

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/quat.h"
#include "context/packages/simmath/transform.h"
#include "context/packages/simmath/trig.h"
#include "context/packages/simmath/vec.h"
#include "simmath_test.h"

using namespace context::packages::simmath;

namespace
{
Vec3 v3(int x, int y, int z)
{
    return {Fixed::from_int(x), Fixed::from_int(y), Fixed::from_int(z)};
}

bool vclose(Vec3 v, double x, double y, double z, double tol)
{
    return simmathtest::close(v.x, x, tol) && simmathtest::close(v.y, y, tol) &&
           simmathtest::close(v.z, z, tol);
}
} // namespace

int main()
{
    // --- quaternion identity + Hamilton product ----------------------------------------------------
    {
        const Quat id = quat_identity();
        CHECK(id == (Quat{kZero, kZero, kZero, kOne}));
        const Quat q{Fixed::from_ratio(1, 2), Fixed::from_ratio(1, 3), Fixed::from_ratio(1, 4),
                     Fixed::from_ratio(2, 3)};
        CHECK((q * id) == q);
        CHECK((id * q) == q);
    }

    // --- conjugate ---------------------------------------------------------------------------------
    {
        const Quat q{Fixed::from_int(1), Fixed::from_int(2), Fixed::from_int(3), Fixed::from_int(4)};
        CHECK(conjugate(q) ==
              (Quat{Fixed::from_int(-1), Fixed::from_int(-2), Fixed::from_int(-3), Fixed::from_int(4)}));
    }

    // --- rotate: identity leaves a vector unchanged ------------------------------------------------
    {
        const Vec3 v = v3(2, 5, -3);
        CHECK(rotate(quat_identity(), v) == v);
    }

    // --- rotate: 90 degrees about +Z maps +X -> +Y -------------------------------------------------
    {
        const Quat rz90 = quat_from_axis_angle(v3(0, 0, 1), kHalfPi);
        CHECK(vclose(rotate(rz90, v3(1, 0, 0)), 0.0, 1.0, 0.0, 5e-3));
        CHECK(vclose(rotate(rz90, v3(0, 1, 0)), -1.0, 0.0, 0.0, 5e-3));
        CHECK(vclose(rotate(rz90, v3(0, 0, 1)), 0.0, 0.0, 1.0, 5e-3)); // axis is invariant
    }

    // --- rotate: 180 degrees about +Y maps +X -> -X ------------------------------------------------
    {
        const Quat ry180 = quat_from_axis_angle(v3(0, 1, 0), kPi);
        CHECK(vclose(rotate(ry180, v3(1, 0, 0)), -1.0, 0.0, 0.0, 6e-3));
        CHECK(vclose(rotate(ry180, v3(0, 0, 1)), 0.0, 0.0, -1.0, 6e-3));
    }

    // --- normalized quaternion is unit length ------------------------------------------------------
    {
        const Quat q{Fixed::from_int(1), Fixed::from_int(2), Fixed::from_int(2), Fixed::from_int(4)};
        CHECK(close(length(normalized(q)), 1.0, 2e-3));
        CHECK(normalized(Quat{kZero, kZero, kZero, kZero}) == quat_identity());
    }

    // --- transform: identity is a no-op ------------------------------------------------------------
    {
        const Vec3 p = v3(3, -2, 5);
        CHECK(transform_point(transform_identity(), p) == p);
    }

    // --- transform: scale, then rotate, then translate ---------------------------------------------
    {
        Transform t;
        t.translation = v3(10, 0, 0);
        t.rotation = quat_from_axis_angle(v3(0, 0, 1), kHalfPi); // +X -> +Y
        t.scale = v3(2, 2, 2);
        // local (1,0,0) -> scale (2,0,0) -> rotate (0,2,0) -> translate (10,2,0)
        CHECK(vclose(transform_point(t, v3(1, 0, 0)), 10.0, 2.0, 0.0, 6e-3));
        // direction ignores translation + (foundation) scale: (1,0,0) -> (0,1,0)
        CHECK(vclose(transform_direction(t, v3(1, 0, 0)), 0.0, 1.0, 0.0, 6e-3));
    }

    // --- transform: compose(parent, child) == parent applied to child-mapped point -----------------
    {
        Transform parent;
        parent.translation = v3(5, 0, 0);
        parent.rotation = quat_from_axis_angle(v3(0, 0, 1), kHalfPi);
        Transform child;
        child.translation = v3(0, 3, 0);
        child.rotation = quat_identity();

        const Transform c = compose(parent, child);
        const Vec3 p = v3(1, 1, 0);
        const Vec3 via_compose = transform_point(c, p);
        const Vec3 via_nested = transform_point(parent, transform_point(child, p));
        CHECK(vclose(via_compose, to_double(via_nested.x), to_double(via_nested.y),
                     to_double(via_nested.z), 6e-3));
    }

    // --- determinism -------------------------------------------------------------------------------
    {
        Quat a = quat_from_axis_angle(v3(0, 1, 0), Fixed::from_ratio(3, 5));
        Quat b = quat_from_axis_angle(v3(0, 1, 0), Fixed::from_ratio(3, 5));
        CHECK(a == b);
        Vec3 va = rotate(a, v3(1, 2, 3));
        Vec3 vb = rotate(b, v3(1, 2, 3));
        CHECK(va.x.raw == vb.x.raw && va.y.raw == vb.y.raw && va.z.raw == vb.z.raw);
    }

    SIMMATH_TEST_MAIN_END();
}

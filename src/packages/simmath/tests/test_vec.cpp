// Fixed-point vectors: arithmetic, dot/cross, length/normalize (R-QA-013).

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/vec.h"
#include "simmath_test.h"

using namespace context::packages::simmath;

namespace
{
Vec3 v3(int x, int y, int z)
{
    return {Fixed::from_int(x), Fixed::from_int(y), Fixed::from_int(z)};
}
} // namespace

int main()
{
    // --- Vec2 --------------------------------------------------------------------------------------
    {
        const Vec2 a{Fixed::from_int(3), Fixed::from_int(4)};
        const Vec2 b{Fixed::from_int(1), Fixed::from_int(2)};
        CHECK((a + b) == (Vec2{Fixed::from_int(4), Fixed::from_int(6)}));
        CHECK((a - b) == (Vec2{Fixed::from_int(2), Fixed::from_int(2)}));
        CHECK((-a) == (Vec2{Fixed::from_int(-3), Fixed::from_int(-4)}));
        CHECK((a * Fixed::from_int(2)) == (Vec2{Fixed::from_int(6), Fixed::from_int(8)}));
        CHECK(dot(a, b) == Fixed::from_int(11)); // 3*1 + 4*2
        CHECK(length_squared(a) == Fixed::from_int(25));
        CHECK(length(a) == Fixed::from_int(5)); // 3-4-5
        const Vec2 n = normalized(a);
        CHECK(close(length(n), 1.0, 2e-3));
        CHECK(normalized(Vec2{kZero, kZero}) == (Vec2{kZero, kZero})); // zero stays zero
    }

    // --- Vec3 arithmetic + dot ---------------------------------------------------------------------
    {
        const Vec3 a = v3(1, 2, 3);
        const Vec3 b = v3(4, 5, 6);
        CHECK((a + b) == v3(5, 7, 9));
        CHECK((b - a) == v3(3, 3, 3));
        CHECK((-a) == v3(-1, -2, -3));
        CHECK((a * Fixed::from_int(3)) == v3(3, 6, 9));
        CHECK(dot(a, b) == Fixed::from_int(32)); // 4 + 10 + 18
        CHECK(hadamard(a, b) == v3(4, 10, 18));
    }

    // --- Vec3 cross (right-handed) -----------------------------------------------------------------
    {
        const Vec3 x = v3(1, 0, 0);
        const Vec3 y = v3(0, 1, 0);
        const Vec3 z = v3(0, 0, 1);
        CHECK(cross(x, y) == z);
        CHECK(cross(y, z) == x);
        CHECK(cross(z, x) == y);
        CHECK(cross(y, x) == v3(0, 0, -1)); // anti-commutative
        CHECK(cross(x, x) == v3(0, 0, 0));
    }

    // --- Vec3 length / normalize -------------------------------------------------------------------
    {
        const Vec3 a = v3(2, 3, 6); // |a| == 7
        CHECK(length_squared(a) == Fixed::from_int(49));
        CHECK(length(a) == Fixed::from_int(7));
        const Vec3 n = normalized(a);
        CHECK(close(length(n), 1.0, 2e-3));
        CHECK(normalized(v3(0, 0, 0)) == v3(0, 0, 0));
    }

    // --- determinism -------------------------------------------------------------------------------
    {
        Vec3 a = v3(1, 2, 3);
        Vec3 b = v3(1, 2, 3);
        for (int i = 0; i < 30; ++i)
        {
            a = normalized(a + v3(0, 1, 0)) * Fixed::from_ratio(9, 8);
            b = normalized(b + v3(0, 1, 0)) * Fixed::from_ratio(9, 8);
        }
        CHECK(a.x.raw == b.x.raw && a.y.raw == b.y.raw && a.z.raw == b.z.raw);
    }

    SIMMATH_TEST_MAIN_END();
}

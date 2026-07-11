// Fixed-point scalar arithmetic + determinism (R-QA-013).

#include "context/packages/simmath/fixed.h"
#include "simmath_test.h"

using namespace context::packages::simmath;

int main()
{
    // --- construction ------------------------------------------------------------------------------
    CHECK(Fixed::from_int(3).raw == (std::int64_t{3} << kFractionBits));
    CHECK(Fixed::from_int(0) == kZero);
    CHECK(Fixed::from_int(1) == kOne);
    CHECK(Fixed::from_ratio(1, 2).raw == kFixedOneRaw / 2);
    CHECK(Fixed::from_ratio(3, 4) == Fixed::from_raw(3 * kFixedOneRaw / 4));

    // --- add / sub / negate ------------------------------------------------------------------------
    CHECK(Fixed::from_int(2) + Fixed::from_int(3) == Fixed::from_int(5));
    CHECK(Fixed::from_int(2) - Fixed::from_int(5) == Fixed::from_int(-3));
    CHECK(-Fixed::from_int(7) == Fixed::from_int(-7));

    // --- multiply (Q16, floor rounding) ------------------------------------------------------------
    CHECK(Fixed::from_int(3) * Fixed::from_int(4) == Fixed::from_int(12));
    CHECK(Fixed::from_ratio(1, 2) * Fixed::from_ratio(1, 2) == Fixed::from_ratio(1, 4));
    CHECK(Fixed::from_int(-3) * Fixed::from_int(4) == Fixed::from_int(-12));
    // scalar-by-integer overloads are exact
    CHECK(Fixed::from_ratio(1, 2) * std::int64_t{6} == Fixed::from_int(3));
    CHECK(Fixed::from_int(9) / std::int64_t{3} == Fixed::from_int(3));

    // --- divide ------------------------------------------------------------------------------------
    CHECK(Fixed::from_int(12) / Fixed::from_int(4) == Fixed::from_int(3));
    CHECK(Fixed::from_int(1) / Fixed::from_int(2) == Fixed::from_ratio(1, 2));
    CHECK(close(Fixed::from_int(1) / Fixed::from_int(3), 1.0 / 3.0, 1e-4));

    // --- compound assignment -----------------------------------------------------------------------
    {
        Fixed a = Fixed::from_int(10);
        a += Fixed::from_int(5);
        CHECK(a == Fixed::from_int(15));
        a -= Fixed::from_int(3);
        CHECK(a == Fixed::from_int(12));
        a *= Fixed::from_int(2);
        CHECK(a == Fixed::from_int(24));
        a /= Fixed::from_int(4);
        CHECK(a == Fixed::from_int(6));
    }

    // --- comparisons -------------------------------------------------------------------------------
    CHECK(Fixed::from_int(2) < Fixed::from_int(3));
    CHECK(Fixed::from_int(3) <= Fixed::from_int(3));
    CHECK(Fixed::from_int(5) > Fixed::from_int(4));
    CHECK(Fixed::from_int(-1) < kZero);
    CHECK(Fixed::from_int(2) != Fixed::from_int(3));

    // --- floor / trunc (sign-aware rounding) -------------------------------------------------------
    CHECK(Fixed::from_ratio(3, 2).floor_int() == 1);
    CHECK(Fixed::from_ratio(-3, 2).floor_int() == -2); // floor toward -inf
    CHECK(Fixed::from_ratio(3, 2).trunc_int() == 1);
    CHECK(Fixed::from_ratio(-3, 2).trunc_int() == -1); // trunc toward zero
    CHECK(Fixed::from_int(4).floor_int() == 4);

    // --- helpers -----------------------------------------------------------------------------------
    CHECK(fixed_abs(Fixed::from_int(-8)) == Fixed::from_int(8));
    CHECK(fixed_abs(Fixed::from_int(8)) == Fixed::from_int(8));
    CHECK(fixed_min(Fixed::from_int(3), Fixed::from_int(7)) == Fixed::from_int(3));
    CHECK(fixed_max(Fixed::from_int(3), Fixed::from_int(7)) == Fixed::from_int(7));
    CHECK(fixed_clamp(Fixed::from_int(10), Fixed::from_int(0), Fixed::from_int(5)) == Fixed::from_int(5));
    CHECK(fixed_clamp(Fixed::from_int(-2), Fixed::from_int(0), Fixed::from_int(5)) == kZero);
    CHECK(fixed_sign(Fixed::from_int(3)) == 1);
    CHECK(fixed_sign(Fixed::from_int(-3)) == -1);
    CHECK(fixed_sign(kZero) == 0);

    // --- determinism: identical operand streams produce identical raw bits -------------------------
    {
        Fixed a = Fixed::from_ratio(7, 3);
        Fixed b = Fixed::from_ratio(7, 3);
        for (int i = 0; i < 50; ++i)
        {
            a = a * Fixed::from_ratio(11, 10) - Fixed::from_ratio(1, 7);
            b = b * Fixed::from_ratio(11, 10) - Fixed::from_ratio(1, 7);
        }
        CHECK(a.raw == b.raw); // bit-identical, no float drift
    }

    SIMMATH_TEST_MAIN_END();
}

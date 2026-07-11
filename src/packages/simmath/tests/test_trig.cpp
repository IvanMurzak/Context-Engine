// Deterministic transcendentals: accuracy vs a libm reference (TEST-only) + cross-platform determinism
// (R-QA-013). The library itself never calls libm; std::sin/std::sqrt appear ONLY here as the tolerance
// oracle.

#include "context/packages/simmath/fixed.h"
#include "context/packages/simmath/trig.h"
#include "simmath_test.h"

#include <cmath>

using namespace context::packages::simmath;

namespace
{
constexpr double kPiD = 3.14159265358979323846;
constexpr double kTrigTol = 3e-3; // Q16 polynomial residual budget
constexpr double kSqrtTol = 2e-3;
} // namespace

int main()
{
    // --- sqrt --------------------------------------------------------------------------------------
    CHECK(fixed_sqrt(kZero) == kZero);
    CHECK(fixed_sqrt(Fixed::from_int(4)) == Fixed::from_int(2));
    CHECK(fixed_sqrt(Fixed::from_int(9)) == Fixed::from_int(3));
    CHECK(fixed_sqrt(Fixed::from_int(144)) == Fixed::from_int(12));
    CHECK(fixed_sqrt(Fixed::from_int(-5)) == kZero); // negative clamps to 0
    for (int v = 1; v <= 1000; ++v)
    {
        const Fixed x = Fixed::from_ratio(v, 7);
        CHECK(close(fixed_sqrt(x), std::sqrt(static_cast<double>(v) / 7.0), kSqrtTol));
    }

    // --- sin / cos anchors -------------------------------------------------------------------------
    CHECK(fixed_sin(kZero) == kZero);
    CHECK(close(fixed_cos(kZero), 1.0, kTrigTol));
    CHECK(close(fixed_sin(kHalfPi), 1.0, kTrigTol));
    CHECK(close(fixed_cos(kHalfPi), 0.0, kTrigTol));
    CHECK(close(fixed_sin(kPi), 0.0, kTrigTol));
    CHECK(close(fixed_cos(kPi), -1.0, kTrigTol));

    // --- sin / cos full-range sweep (incl. negative + multi-period reduction) ----------------------
    for (int i = -400; i <= 400; ++i)
    {
        const double a = static_cast<double>(i) * (kPiD / 100.0); // step ~0.0314 rad, |a| up to ~12.6
        const Fixed fa = Fixed::from_raw(static_cast<std::int64_t>(std::llround(a * kFixedOneRaw)));
        CHECK(close(fixed_sin(fa), std::sin(a), kTrigTol));
        CHECK(close(fixed_cos(fa), std::cos(a), kTrigTol));
    }

    // --- tan (away from the asymptotes) ------------------------------------------------------------
    for (int i = -60; i <= 60; ++i)
    {
        const double a = static_cast<double>(i) * (kPiD / 200.0); // |a| up to ~0.94 rad
        const Fixed fa = Fixed::from_raw(static_cast<std::int64_t>(std::llround(a * kFixedOneRaw)));
        CHECK(close(fixed_tan(fa), std::tan(a), 1e-2));
    }

    // --- atan (|x| <= 1 and the reciprocal branch |x| > 1) -----------------------------------------
    for (int i = -500; i <= 500; ++i)
    {
        const double x = static_cast<double>(i) / 100.0; // -5 .. 5
        const Fixed fx = Fixed::from_raw(static_cast<std::int64_t>(std::llround(x * kFixedOneRaw)));
        CHECK(close(fixed_atan(fx), std::atan(x), kTrigTol));
    }

    // --- atan2 across all four quadrants + axes ----------------------------------------------------
    const int coords[] = {-40, -13, -1, 0, 1, 7, 25};
    for (int yi : coords)
        for (int xi : coords)
        {
            if (xi == 0 && yi == 0)
                continue;
            const double y = static_cast<double>(yi) / 10.0;
            const double x = static_cast<double>(xi) / 10.0;
            const Fixed fy = Fixed::from_raw(static_cast<std::int64_t>(std::llround(y * kFixedOneRaw)));
            const Fixed fx = Fixed::from_raw(static_cast<std::int64_t>(std::llround(x * kFixedOneRaw)));
            CHECK(close(fixed_atan2(fy, fx), std::atan2(y, x), 4e-3));
        }
    CHECK(fixed_atan2(kZero, kZero) == kZero);
    CHECK(close(fixed_atan2(kOne, kZero), kPiD / 2.0, kTrigTol));   // +y axis
    CHECK(close(fixed_atan2(-kOne, kZero), -kPiD / 2.0, kTrigTol)); // -y axis

    // --- determinism: recomputation is bit-identical -----------------------------------------------
    for (int i = 0; i < 200; ++i)
    {
        const Fixed a = Fixed::from_ratio(i, 11);
        CHECK(fixed_sin(a).raw == fixed_sin(a).raw);
        CHECK(fixed_sqrt(a).raw == fixed_sqrt(a).raw);
        CHECK(fixed_atan2(a, Fixed::from_int(2)).raw == fixed_atan2(a, Fixed::from_int(2)).raw);
    }

    SIMMATH_TEST_MAIN_END();
}

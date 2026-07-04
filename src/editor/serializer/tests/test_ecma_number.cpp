// ECMAScript Number::toString notation tests — the fixed-vs-exponent boundaries, -0, subnormals,
// and the shortest-round-trip property (R-FILE-001; ECMA-262 §6.1.6.1.20).

#include "context/editor/serializer/canonical.h"

#include "serializer_test.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

using context::editor::serializer::ecma_number;

namespace
{

std::string fmt(double v)
{
    std::string out;
    ecma_number(v, out);
    return out;
}

} // namespace

int main()
{
    // Zero and signed zero: -0 is defined, serialized as "0" (R-FILE-001).
    CHECK(fmt(0.0) == "0");
    CHECK(fmt(-0.0) == "0");

    // Integral doubles print without a decimal point.
    CHECK(fmt(1.0) == "1");
    CHECK(fmt(-1.0) == "-1");
    CHECK(fmt(42.0) == "42");
    CHECK(fmt(1000000.0) == "1000000");

    // Simple fractions use the shortest round-trip digits.
    CHECK(fmt(0.1) == "0.1");
    CHECK(fmt(0.5) == "0.5");
    CHECK(fmt(-2.75) == "-2.75");
    CHECK(fmt(1.0 / 3.0) == "0.3333333333333333");

    // THE fixed/exponent boundaries (ECMAScript n in (-6, 21] is fixed notation):
    CHECK(fmt(1e20) == "100000000000000000000");  // n = 21: still fixed
    CHECK(fmt(1e21) == "1e+21");                  // n = 22: exponent
    CHECK(fmt(1.5e21) == "1.5e+21");
    CHECK(fmt(1e-6) == "0.000001");               // n = -5: still fixed
    CHECK(fmt(1e-7) == "1e-7");                   // n = -6: exponent
    CHECK(fmt(1.5e-7) == "1.5e-7");
    CHECK(fmt(-1e21) == "-1e+21");
    CHECK(fmt(-1e-7) == "-1e-7");

    // 2^53 neighborhood: the double domain rounds 2^53 + 1 down (the integer TYPE preserves it
    // losslessly — that is test_json_parse's job; HERE the double semantics are pinned).
    CHECK(fmt(9007199254740992.0) == "9007199254740992");
    CHECK(fmt(9007199254740993.0) == "9007199254740992"); // literal rounds to the nearest double

    // Extremes: max double, min normal, min subnormal (shortest-round-trip edge cases).
    CHECK(fmt(1.7976931348623157e308) == "1.7976931348623157e+308");
    CHECK(fmt(2.2250738585072014e-308) == "2.2250738585072014e-308");
    CHECK(fmt(5e-324) == "5e-324"); // min positive subnormal
    CHECK(fmt(std::numeric_limits<double>::denorm_min()) == "5e-324");

    // Exponent form drops the mantissa point for single-digit mantissas.
    CHECK(fmt(1e100) == "1e+100");
    CHECK(fmt(1.2345e-100) == "1.2345e-100");

    // Shortest-round-trip property over a deterministic sample: parse(print(v)) == v exactly.
    {
        std::uint64_t state = 0x123456789ABCDEF0ULL;
        int checked = 0;
        for (int i = 0; i < 4096; ++i)
        {
            // xorshift64* over the raw bit pattern; skip non-finite patterns.
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            const std::uint64_t bits = state * 0x2545F4914F6CDD1DULL;
            double v = 0.0;
            static_assert(sizeof(v) == sizeof(bits));
            std::memcpy(&v, &bits, sizeof(v));
            if (!(v == v) || v == std::numeric_limits<double>::infinity() ||
                v == -std::numeric_limits<double>::infinity())
                continue;
            const std::string s = fmt(v);
            const double round_tripped = std::strtod(s.c_str(), nullptr);
            CHECK(round_tripped == v || (v == 0.0 && round_tripped == 0.0));
            ++checked;
        }
        CHECK(checked > 3000); // the sample genuinely exercised the property
    }

    SERIALIZER_TEST_MAIN_END();
}

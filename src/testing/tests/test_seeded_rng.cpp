// SeededRng determinism + shape (R-QA-011): the harness's reproducibility rests on this generator
// being bit-identical for a given seed. splitmix64 is pure uint64 arithmetic, so the same seed
// yields the same sequence on every platform — the property that lets a failing scenario seed be
// replayed and committed as a regression case.

#include "context/testing/seeded_rng.h"
#include "testing_test.h"

#include <cstdint>
#include <set>
#include <vector>

using namespace context::testing;

int main()
{
    // Determinism: two generators with the same seed produce byte-identical sequences.
    {
        SeededRng a(12345);
        SeededRng b(12345);
        for (int i = 0; i < 1000; ++i)
            CHECK(a.next_u64() == b.next_u64());
    }

    // Distinct seeds diverge (not a proof of quality, but catches a broken seed wire-up).
    {
        SeededRng a(1);
        SeededRng b(2);
        bool differ = false;
        for (int i = 0; i < 8; ++i)
            if (a.next_u64() != b.next_u64())
            {
                differ = true;
                break;
            }
        CHECK(differ);
    }

    // bounded() stays in range and yields 0 for n == 0.
    {
        SeededRng r(99);
        for (int i = 0; i < 1000; ++i)
            CHECK(r.bounded(10) < 10);
        CHECK(r.bounded(0) == 0);
    }

    // range() honours inclusive bounds.
    {
        SeededRng r(7);
        for (int i = 0; i < 1000; ++i)
        {
            const std::uint64_t v = r.range(3, 5);
            CHECK(v >= 3 && v <= 5);
        }
    }

    // shuffle() is deterministic per seed AND a true permutation (no lost/duplicated elements).
    {
        std::vector<int> base;
        for (int i = 0; i < 32; ++i)
            base.push_back(i);
        std::vector<int> x = base;
        std::vector<int> y = base;
        SeededRng r1(555);
        SeededRng r2(555);
        r1.shuffle(x);
        r2.shuffle(y);
        CHECK(x == y); // deterministic
        const std::multiset<int> shuffled(x.begin(), x.end());
        const std::multiset<int> original(base.begin(), base.end());
        CHECK(shuffled == original); // permutation preserves the multiset
    }

    TESTING_TEST_MAIN_END();
}

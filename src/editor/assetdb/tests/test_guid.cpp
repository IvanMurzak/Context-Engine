// GUID form + generator tests: the pinned 32-hex form, randomness, and the deterministic seam.

#include "context/editor/assetdb/guid.h"

#include "assetdb_test.h"

#include <set>
#include <string>

using namespace context::editor::assetdb;

int main()
{
    // --- the pinned form ------------------------------------------------------------------------
    CHECK(is_guid("0123456789abcdef0123456789abcdef"));
    CHECK(is_guid(format_guid(0, 0)));
    CHECK(format_guid(0, 0) == "00000000000000000000000000000000");
    CHECK(format_guid(0xffffffffffffffffULL, 0xffffffffffffffffULL) ==
          "ffffffffffffffffffffffffffffffff");
    CHECK(format_guid(0x0123456789abcdefULL, 0xfedcba9876543210ULL) ==
          "0123456789abcdeffedcba9876543210");

    // Failure paths: wrong length, uppercase, non-hex, empty, whitespace.
    CHECK(!is_guid(""));
    CHECK(!is_guid("0123456789abcdef0123456789abcde"));   // 31 chars
    CHECK(!is_guid("0123456789abcdef0123456789abcdef0")); // 33 chars
    CHECK(!is_guid("0123456789ABCDEF0123456789abcdef"));  // uppercase is not canonical
    CHECK(!is_guid("0123456789abcdeg0123456789abcdef"));  // 'g' is not hex
    CHECK(!is_guid("0123456789abcdef 123456789abcdef"));  // embedded space
    CHECK(!is_guid("{123456789abcdef0123456789abcde}"));  // braced forms are not the pinned form

    // --- the deterministic sequence generator (the test seam) -----------------------------------
    SequenceGuidGenerator seq(1);
    const std::string first = seq.next();
    const std::string second = seq.next();
    CHECK(is_guid(first));
    CHECK(is_guid(second));
    CHECK(first != second);
    CHECK(first == "00000000000000000000000000000001");
    CHECK(second == "00000000000000000000000000000002");
    SequenceGuidGenerator seq_again(1);
    CHECK(seq_again.next() == first); // deterministic: same start, same sequence

    // --- the production random generator ---------------------------------------------------------
    RandomGuidGenerator random;
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i)
    {
        const std::string guid = random.next();
        CHECK(is_guid(guid));
        seen.insert(guid);
    }
    CHECK(seen.size() == 1000); // collision-resistant: 1000 draws, 1000 distinct

    ASSETDB_TEST_MAIN_END();
}

// Stable intra-file ids (L-33): form validation, canonical formatting, minting.
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/compose/stable_id.h"

#include "compose_test.h"

#include <set>
#include <string>

using namespace context::editor::compose;

int main()
{
    // --- form validation: happy path --------------------------------------------------------------
    CHECK(is_stable_id("0123456789abcdef"));                 // exactly 64 bits
    CHECK(is_stable_id("00000000000000000000000000000000")); // exactly 128 bits
    CHECK(is_stable_id("deadbeefdeadbeefcafe"));             // in-between length is fine

    // --- form validation: failure paths ------------------------------------------------------------
    CHECK(!is_stable_id(""));                                  // empty
    CHECK(!is_stable_id("0123456789abcde"));                   // 15 chars — under the 64-bit floor
    CHECK(!is_stable_id("000000000000000000000000000000000")); // 33 chars — over 128 bits
    CHECK(!is_stable_id("0123456789ABCDEF"));                  // uppercase hex is not canonical
    CHECK(!is_stable_id("0123456789abcdeg"));                  // non-hex character
    CHECK(!is_stable_id("0123 56789abcdef"));                  // whitespace
    CHECK(!is_stable_id(kSceneRootId));                        // the reserved $root token is NOT an id

    // --- formatting: canonical 16-char lowercase hex, zero-padded ---------------------------------
    CHECK(format_stable_id(0) == "0000000000000000");
    CHECK(format_stable_id(0xdeadbeefULL) == "00000000deadbeef");
    CHECK(format_stable_id(0xffffffffffffffffULL) == "ffffffffffffffff");
    CHECK(format_stable_id(0x0123456789abcdefULL) == "0123456789abcdef");
    CHECK(is_stable_id(format_stable_id(0x1234)));

    // --- minting: valid form, and no collisions across a healthy sample --------------------------
    // (64 random bits: 4096 draws colliding would indicate a broken entropy source, not bad luck —
    // the birthday bound at this sample size is ~4.5e-13.)
    std::set<std::string> minted;
    for (int i = 0; i < 4096; ++i)
    {
        std::string id = mint_stable_id();
        CHECK(is_stable_id(id));
        CHECK(id.size() == 16);
        CHECK(minted.insert(id).second);
    }

    COMPOSE_TEST_MAIN_END();
}

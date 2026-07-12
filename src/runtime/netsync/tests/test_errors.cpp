// netsync fail-closed refusals (M6 X2, R-QA-013 failure paths): the net.* code strings the contract
// error catalog registers, asserted at their source of truth (errors.h). The behavioural failure
// paths that RETURN these codes are exercised in tests/test_state_sync.cpp; this pins the exact
// catalog identities so the strings the contract catalog's net.* block registers cannot drift.

#include "context/runtime/netsync/errors.h"
#include "netsync_test.h"

#include <cstring>

namespace net = context::runtime::netsync;

int main()
{
    // --- the code strings are the exact catalog identities (pins the net.* block) -----------------
    CHECK(std::strcmp(net::kInvalidNetIdCode, "net.invalid_net_id") == 0);
    CHECK(std::strcmp(net::kDuplicateNetIdCode, "net.duplicate_net_id") == 0);
    CHECK(std::strcmp(net::kSnapshotComponentMismatchCode, "net.snapshot_component_mismatch") == 0);
    CHECK(std::strcmp(net::kAuthorityConflictCode, "net.authority_conflict") == 0);

    // --- all four are in the net.* domain + distinct ----------------------------------------------
    const char* codes[] = {net::kInvalidNetIdCode, net::kDuplicateNetIdCode,
                           net::kSnapshotComponentMismatchCode, net::kAuthorityConflictCode};
    for (const char* c : codes)
        CHECK(std::strncmp(c, "net.", 4) == 0);
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            CHECK(std::strcmp(codes[i], codes[j]) != 0);

    NETSYNC_TEST_MAIN_END();
}

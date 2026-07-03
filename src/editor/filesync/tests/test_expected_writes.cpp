// Expected-writes table: self-echo suppression within the TTL, and the critical invariant that a
// suppression can NEVER mask a genuine later external edit (R-FILE-002).

#include "context/editor/filesync/expected_writes.h"
#include "filesync_test.h"

using namespace context::editor::filesync;

int main()
{
    constexpr std::uint64_t kTtl = 1000;

    // --- self-echo: a registered write matched by (path, hash) within TTL is suppressed once -------
    {
        ExpectedWrites table{kTtl};
        table.register_write("proj/a.txt", 0xAA, /*now=*/0);
        CHECK(table.size() == 1);
        // Our own echo, same hash, still within TTL -> suppressed, and the entry is consumed.
        CHECK(table.is_self_echo("proj/a.txt", 0xAA, /*now=*/100));
        CHECK(table.size() == 0);
        // A second observation is NOT suppressed (entry already consumed).
        CHECK(!table.is_self_echo("proj/a.txt", 0xAA, /*now=*/150));
    }

    // --- a DIFFERENT hash on the same path is a real external change, not an echo ------------------
    {
        ExpectedWrites table{kTtl};
        table.register_write("proj/a.txt", 0xAA, 0);
        CHECK(!table.is_self_echo("proj/a.txt", 0xBB, 100)); // different content -> not our echo
        CHECK(table.size() == 1);                             // non-matching probe does not consume
    }

    // --- expiry: after the TTL, the registered write NEVER masks a later external edit -------------
    {
        ExpectedWrites table{kTtl};
        table.register_write("proj/a.txt", 0xAA, 0);
        // Same path+hash but observed AFTER expiry -> treated as a genuine external edit.
        CHECK(!table.is_self_echo("proj/a.txt", 0xAA, /*now=*/kTtl + 1));
        CHECK(table.size() == 0); // expired entry dropped on probe
    }

    // --- explicit expire() sweep -----------------------------------------------------------------
    {
        ExpectedWrites table{kTtl};
        table.register_write("proj/a.txt", 0xAA, 0);
        table.register_write("proj/b.txt", 0xBB, 500);
        table.expire(600); // a.txt (exp 1000) alive, b.txt (exp 1500) alive
        CHECK(table.size() == 2);
        table.expire(1200); // a.txt expired, b.txt alive
        CHECK(table.size() == 1);
        CHECK(!table.is_self_echo("proj/a.txt", 0xAA, 1200));
        CHECK(table.is_self_echo("proj/b.txt", 0xBB, 1200));
    }

    // --- an unregistered path is never a self-echo -----------------------------------------------
    {
        ExpectedWrites table{kTtl};
        CHECK(!table.is_self_echo("proj/unknown.txt", 0x11, 0));
    }

    FILESYNC_TEST_MAIN_END();
}

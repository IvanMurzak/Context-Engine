// R-QA-013 tests for the declared-access conflict predicate (access.h) — the reader/writer exclusion
// rule the parallel scheduler's DAG is built from. Engine-independent pure logic, so a LOCAL gate on
// every leg. Covers: canonicalization (sort + dedupe), the sorted-list intersection helper, and every
// arm of conflicts() — write-write, read-write, write-read, read-read (non-conflict), and disjoint.

#include "context/editor/schedule/access.h"

#include "schedule_test.h"

#include <vector>

namespace csch = context::editor::schedule;
namespace ck = context::kernel;

namespace
{

void test_canonicalize()
{
    std::vector<ck::ComponentId> ids = {5, 1, 5, 3, 1, 3};
    csch::canonicalize(ids);
    CHECK(ids.size() == 3);
    CHECK(ids[0] == 1 && ids[1] == 3 && ids[2] == 5);

    std::vector<ck::ComponentId> empty;
    csch::canonicalize(empty);
    CHECK(empty.empty());

    // make() canonicalizes both lists.
    const csch::AccessSet a = csch::AccessSet::make({3, 1, 1}, {9, 2, 9});
    CHECK((a.reads == std::vector<ck::ComponentId>{1, 3}));
    CHECK((a.writes == std::vector<ck::ComponentId>{2, 9}));
    CHECK(!a.is_read_only());
    CHECK(csch::AccessSet::make({1, 2}, {}).is_read_only());
}

void test_intersects()
{
    CHECK(csch::intersects({1, 2, 3}, {3, 4, 5}));  // share 3
    CHECK(csch::intersects({1, 5, 9}, {9}));        // share 9 (tail)
    CHECK(csch::intersects({4}, {1, 4, 7}));        // share 4 (middle)
    CHECK(!csch::intersects({1, 2, 3}, {4, 5, 6})); // disjoint
    CHECK(!csch::intersects({}, {1, 2}));           // empty
    CHECK(!csch::intersects({1, 2}, {}));           // empty
}

void test_conflicts()
{
    // write-write on component 1 → conflict.
    CHECK(csch::conflicts(csch::AccessSet::make({}, {1}), csch::AccessSet::make({}, {1})));

    // read-write: A reads 2, B writes 2 → conflict (and symmetric).
    const csch::AccessSet reads2 = csch::AccessSet::make({2}, {});
    const csch::AccessSet writes2 = csch::AccessSet::make({}, {2});
    CHECK(csch::conflicts(reads2, writes2));
    CHECK(csch::conflicts(writes2, reads2)); // symmetric

    // read-read on the SAME component 3 → NO conflict (concurrent reads are safe).
    CHECK(!csch::conflicts(csch::AccessSet::make({3}, {}), csch::AccessSet::make({3}, {})));

    // Fully disjoint access → no conflict.
    CHECK(!csch::conflicts(csch::AccessSet::make({1}, {2}), csch::AccessSet::make({3}, {4})));

    // A writes 5 while reading 6; B reads 5 while writing 7. The write-5/read-5 overlap conflicts even
    // though the two write sets ({5} vs {7}) are disjoint — the read-write arm must catch it.
    CHECK(csch::conflicts(csch::AccessSet::make({6}, {5}), csch::AccessSet::make({5}, {7})));

    // Two read-only systems sharing many reads never conflict.
    CHECK(!csch::conflicts(csch::AccessSet::make({1, 2, 3}, {}), csch::AccessSet::make({2, 3, 4}, {})));
}

} // namespace

int main()
{
    test_canonicalize();
    test_intersects();
    test_conflicts();
    if (scheduletest::g_failures == 0)
    {
        std::printf("schedule access predicate: all checks passed\n");
    }
    SCHEDULE_TEST_MAIN_END();
}

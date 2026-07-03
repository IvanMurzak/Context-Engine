// Watcher seam: FakeWatcher enqueue/drain, and the drop/duplicate/reorder fault primitives the
// R-QA-010 harness relies on. NullWatcher is always degraded (correctness comes from the crawl).

#include "context/editor/filesync/watcher.h"
#include "filesync_test.h"

using namespace context::editor::filesync;

int main()
{
    // --- FakeWatcher: emit then drain --------------------------------------------------------------
    {
        FakeWatcher watcher;
        CHECK(!watcher.degraded());
        watcher.emit("proj/a.txt", ChangeKind::modified);
        watcher.emit("proj/b.txt", ChangeKind::created);
        auto events = watcher.poll();
        CHECK(events.size() == 2);
        CHECK(events[0].path == "proj/a.txt");
        CHECK(events[0].kind == ChangeKind::modified);
        CHECK(events[1].kind == ChangeKind::created);
        // Drained: a second poll is empty.
        CHECK(watcher.poll().empty());
    }

    // --- drop_all models a fully lost event batch ------------------------------------------------
    {
        FakeWatcher watcher;
        watcher.emit("proj/a.txt", ChangeKind::modified);
        watcher.drop_all();
        CHECK(watcher.poll().empty());
    }

    // --- duplicate_all models duplicated delivery ------------------------------------------------
    {
        FakeWatcher watcher;
        watcher.emit("proj/a.txt", ChangeKind::modified);
        watcher.duplicate_all();
        CHECK(watcher.poll().size() == 2);
    }

    // --- reverse models reordered delivery -------------------------------------------------------
    {
        FakeWatcher watcher;
        watcher.emit("proj/a.txt", ChangeKind::modified);
        watcher.emit("proj/b.txt", ChangeKind::modified);
        watcher.reverse();
        auto events = watcher.poll();
        CHECK(events.size() == 2);
        CHECK(events[0].path == "proj/b.txt");
        CHECK(events[1].path == "proj/a.txt");
    }

    // --- set_degraded flips the degraded signal --------------------------------------------------
    {
        FakeWatcher watcher;
        watcher.set_degraded(true);
        CHECK(watcher.degraded());
    }

    // --- NullWatcher is always degraded and emits nothing ----------------------------------------
    {
        NullWatcher watcher;
        CHECK(watcher.degraded());
        CHECK(watcher.poll().empty());
    }

    FILESYNC_TEST_MAIN_END();
}

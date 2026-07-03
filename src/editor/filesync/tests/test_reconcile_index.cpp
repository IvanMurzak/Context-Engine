// Reconcile index: in-memory ops, deterministic serialization round-trip, atomic persist + reload,
// and tolerance of a corrupt/missing file (the index is always rebuildable — R-FILE-002).

#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/reconcile_index.h"
#include "filesync_test.h"

#include <string>

using namespace context::editor::filesync;

int main()
{
    // --- basic map ops --------------------------------------------------------------------------
    {
        ReconcileIndex index;
        CHECK(index.size() == 0);
        index.put("proj/a.txt", IndexEntry{3, 100, 0xABCDEF});
        index.put("proj/b.txt", IndexEntry{5, 200, 0x123456});
        CHECK(index.size() == 2);
        CHECK(index.get("proj/a.txt").has_value());
        CHECK(index.get("proj/a.txt")->content_hash == 0xABCDEF);
        CHECK(!index.get("proj/missing").has_value());
        index.erase("proj/a.txt");
        CHECK(!index.get("proj/a.txt").has_value());
        CHECK(index.size() == 1);
    }

    // --- deterministic serialize/deserialize round-trip (paths with spaces survive) --------------
    {
        ReconcileIndex index;
        index.put("proj/a b.txt", IndexEntry{7, 111, 42});
        index.put("proj/z.txt", IndexEntry{9, 222, 99});
        const std::string text = index.serialize();
        CHECK(index.serialize() == text); // stable / deterministic

        ReconcileIndex back = ReconcileIndex::deserialize(text);
        CHECK(back.size() == 2);
        CHECK(back.get("proj/a b.txt").has_value());
        CHECK(back.get("proj/a b.txt")->size == 7);
        CHECK(back.get("proj/a b.txt")->mtime_nanos == 111);
        CHECK(back.get("proj/a b.txt")->content_hash == 42);
        CHECK(back.get("proj/z.txt")->content_hash == 99);
    }

    // --- atomic persist + reload through a FileStore --------------------------------------------
    {
        MemoryFileStore fs;
        ReconcileIndex index;
        index.put("proj/a.txt", IndexEntry{3, 100, 555});
        CHECK(index.save(fs, "proj/.editor/index"));

        ReconcileIndex loaded = ReconcileIndex::load(fs, "proj/.editor/index");
        CHECK(loaded.size() == 1);
        CHECK(loaded.get("proj/a.txt")->content_hash == 555);
    }

    // --- missing index file -> empty (rebuildable), never a crash -------------------------------
    {
        MemoryFileStore fs;
        ReconcileIndex loaded = ReconcileIndex::load(fs, "proj/.editor/index");
        CHECK(loaded.size() == 0);
    }

    // --- corrupt lines are dropped, valid ones survive ------------------------------------------
    {
        ReconcileIndex back = ReconcileIndex::deserialize("garbage line\n3 100 555 proj/ok.txt\n");
        CHECK(back.size() == 1);
        CHECK(back.get("proj/ok.txt").has_value());
    }

    FILESYNC_TEST_MAIN_END();
}

// NativeFileStore contract, on REAL disk (R-FILE-002/004): read/write/stat/list/remove/exists parity
// with MemoryFileStore, atomic write-temp-rename over a real FS (overwrite is fully-new), fsync
// durability, and THE torn-write proof — a crash between the durable temp write and the rename leaves
// the visible on-disk file byte-for-byte untouched. Covers happy, edge, AND failure paths.

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/native_file_store.h"

#include "filesync_test.h"
#include "native_test_support.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using context::editor::filesync::atomic_temp_path;
using context::editor::filesync::atomic_write;
using context::editor::filesync::FileStat;
using context::editor::filesync::NativeFileStore;
using context::editor::filesync::SimulatedCrash;
using nfstest::CrashInjectingFileStore;
using nfstest::TempDir;

int main()
{
    // --- write/read roundtrip, including binary bytes (embedded NUL + newline), and exists ----------
    {
        TempDir tmp("rw");
        NativeFileStore store(tmp.path());

        CHECK(!store.exists("a.txt"));
        CHECK(!store.read("a.txt").has_value());

        const std::string payload = std::string("line1\n\0binary\xff", 14);
        CHECK(store.write("a.txt", payload));
        CHECK(store.exists("a.txt"));
        auto got = store.read("a.txt");
        CHECK(got.has_value());
        CHECK(*got == payload); // byte-exact: binary mode, no CRLF translation, NUL preserved

        // An empty file reads back as an empty string (not nullopt).
        CHECK(store.write("empty.txt", ""));
        auto empty = store.read("empty.txt");
        CHECK(empty.has_value());
        CHECK(empty->empty());

        // A nested path auto-creates parent directories on write.
        CHECK(store.write("sub/dir/deep.txt", "deep"));
        CHECK(*store.read("sub/dir/deep.txt") == "deep");
        CHECK(fs::exists(tmp.path() / "sub" / "dir" / "deep.txt")); // really on disk
    }

    // --- stat: size + a stable, change-sensitive mtime --------------------------------------------
    {
        TempDir tmp("stat");
        NativeFileStore store(tmp.path());

        CHECK(!store.stat("x.txt").has_value());
        CHECK(store.write("x.txt", "hello")); // 5 bytes
        auto st = store.stat("x.txt");
        CHECK(st.has_value());
        CHECK(st->size == 5);
        // Re-reading an unchanged file yields the SAME mtime (equality is all the reconcile gate needs).
        auto st2 = store.stat("x.txt");
        CHECK(st2.has_value());
        CHECK(st2->mtime_nanos == st->mtime_nanos);
        // A rewrite to a different size updates the size.
        CHECK(store.write("x.txt", "hello world"));
        auto st3 = store.stat("x.txt");
        CHECK(st3.has_value());
        CHECK(st3->size == 11);
    }

    // --- list: recursive, sorted, normalized, and relative to the store base ----------------------
    {
        TempDir tmp("list");
        NativeFileStore store(tmp.path());

        CHECK(store.list("proj").empty()); // missing dir -> empty
        CHECK(store.write("proj/b.txt", "b"));
        CHECK(store.write("proj/a.txt", "a"));
        CHECK(store.write("proj/nested/c.txt", "c"));

        auto files = store.list("proj");
        // list() returns sorted, normalized, base-relative logical paths; assert the exact sorted set.
        const std::vector<std::string> want = {"proj/a.txt", "proj/b.txt", "proj/nested/c.txt"};
        CHECK(files == want);

        // list() on a regular file returns just that file.
        auto one = store.list("proj/a.txt");
        CHECK(one.size() == 1);
        CHECK(one.front() == "proj/a.txt");
    }

    // --- remove -----------------------------------------------------------------------------------
    {
        TempDir tmp("rm");
        NativeFileStore store(tmp.path());
        CHECK(store.write("g.txt", "g"));
        CHECK(store.remove("g.txt"));  // existed -> removed
        CHECK(!store.exists("g.txt"));
        CHECK(!store.remove("g.txt")); // already gone -> false
    }

    // --- rename is an atomic replace of an existing destination (the MinGW _wrename trap) ----------
    {
        TempDir tmp("rename");
        NativeFileStore store(tmp.path());
        CHECK(store.write("dst.txt", "old"));
        CHECK(store.write("src.txt", "new"));
        CHECK(store.rename("src.txt", "dst.txt")); // must REPLACE the existing dst
        CHECK(*store.read("dst.txt") == "new");
        CHECK(!store.exists("src.txt")); // source consumed by the move
    }

    // --- atomic_write over the real store: durable, no temp residue; overwrite is fully-new -------
    {
        TempDir tmp("atomic");
        NativeFileStore store(tmp.path());

        CHECK(atomic_write(store, "proj/a.scene", "v1"));
        CHECK(*store.read("proj/a.scene") == "v1");
        CHECK(!store.exists(atomic_temp_path("proj/a.scene", ""))); // temp renamed away
        CHECK(store.list("proj").size() == 1);                      // only the real file remains

        CHECK(atomic_write(store, "proj/a.scene", "v2"));
        CHECK(*store.read("proj/a.scene") == "v2"); // reader sees fully-new
        CHECK(store.list("proj").size() == 1);
    }

    // --- fsync durability barrier: the flushed bytes are intact and readable ------------------------
    {
        TempDir tmp("fsync");
        NativeFileStore store(tmp.path());
        CHECK(atomic_write(store, "d.txt", "durable")); // atomic_write already fsyncs temp + target
        store.fsync("d.txt");                            // an explicit extra barrier must not corrupt
        CHECK(*store.read("d.txt") == "durable");
        CHECK(store.stat("d.txt")->size == 7);
        store.fsync("does-not-exist.txt"); // best-effort no-op, must not throw
    }

    // --- THE torn-write proof on a REAL FS: a crash between temp-write and rename leaves the visible
    // --- file byte-for-byte untouched, with the new bytes stranded only in the temp residue --------
    {
        TempDir tmp("torn");
        NativeFileStore native(tmp.path());
        CrashInjectingFileStore fs_crash(native);

        CHECK(atomic_write(fs_crash, "proj/a.scene", "committed")); // establish prior content
        fs_crash.crash_on_rename_to("proj/a.scene");                // next rename over it "crashes"

        bool crashed = false;
        try
        {
            atomic_write(fs_crash, "proj/a.scene", "torn?");
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);

        // The visible on-disk file is UNCHANGED — never a partial/torn write (read through the raw
        // native store to prove it is the real file, not a decorator artifact).
        CHECK(*native.read("proj/a.scene") == "committed");
        // The new bytes exist only in the temp residue.
        const std::string temp = atomic_temp_path("proj/a.scene", "");
        CHECK(native.exists(temp));
        CHECK(*native.read(temp) == "torn?");
    }

    // --- crash on a FIRST-ever write leaves the target absent (no half-created file on disk) --------
    {
        TempDir tmp("torn-new");
        NativeFileStore native(tmp.path());
        CrashInjectingFileStore fs_crash(native);
        fs_crash.crash_on_rename_to("proj/new.scene");

        bool crashed = false;
        try
        {
            atomic_write(fs_crash, "proj/new.scene", "hello");
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(!native.exists("proj/new.scene")); // never became visible
    }

    FILESYNC_TEST_MAIN_END();
}

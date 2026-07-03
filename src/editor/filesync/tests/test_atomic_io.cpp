// Atomic IO: happy write, overwrite, and the torn-write proof — a crash between temp-write and rename
// leaves the visible file untouched (R-FILE-004, R-QA-010).

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/file_store.h"
#include "filesync_test.h"

#include <string>

using namespace context::editor::filesync;

int main()
{
    // --- happy path: a durable atomic write is visible, with no leftover temp file ----------------
    {
        MemoryFileStore fs;
        CHECK(atomic_write(fs, "proj/a.txt", "v1"));
        CHECK(fs.read("proj/a.txt").has_value());
        CHECK(*fs.read("proj/a.txt") == "v1");
        // The temp file was renamed away — only the real file remains.
        CHECK(!fs.exists(atomic_temp_path("proj/a.txt", "")));
        CHECK(fs.file_count() == 1);
    }

    // --- overwrite is atomic: reader sees fully-old or fully-new, here fully-new ------------------
    {
        MemoryFileStore fs;
        CHECK(atomic_write(fs, "proj/a.txt", "old"));
        CHECK(atomic_write(fs, "proj/a.txt", "new"));
        CHECK(*fs.read("proj/a.txt") == "new");
        CHECK(fs.file_count() == 1);
    }

    // --- THE torn-write proof: crash between the temp write and the rename ------------------------
    {
        MemoryFileStore fs;
        CHECK(atomic_write(fs, "proj/a.txt", "committed")); // establish prior content
        fs.crash_on_rename_to("proj/a.txt");                // next rename over a.txt "crashes"

        bool crashed = false;
        try
        {
            atomic_write(fs, "proj/a.txt", "torn?");
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);

        // The visible file is UNCHANGED — never a partial/torn write.
        CHECK(fs.read("proj/a.txt").has_value());
        CHECK(*fs.read("proj/a.txt") == "committed");

        // The new bytes exist only in the temp file (the residue), not at the visible path.
        const std::string temp = atomic_temp_path("proj/a.txt", "");
        CHECK(fs.exists(temp));
        CHECK(*fs.read(temp) == "torn?");
    }

    // --- crash on a FIRST-ever write to a path leaves the path absent (no half-created file) -------
    {
        MemoryFileStore fs;
        fs.crash_on_rename_to("proj/new.txt");
        bool crashed = false;
        try
        {
            atomic_write(fs, "proj/new.txt", "hello");
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(!fs.exists("proj/new.txt")); // never became visible
    }

    FILESYNC_TEST_MAIN_END();
}

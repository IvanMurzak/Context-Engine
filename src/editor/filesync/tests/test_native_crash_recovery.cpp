// On-disk intent-log crash recovery (R-FILE-004), over the REAL NativeFileStore:
//   * a clean multi-file op applies every write and clears the entry on real disk;
//   * an interrupted multi-file write (crash between the durable temp write and the rename of the
//     SECOND file) is RESUMED to completion on restart — the first file's committed bytes survive, the
//     second is finished, nothing is lost, and resume is idempotent;
//   * a file that moved on since the crash is NOT clobbered (CAS diagnostic), preserving real bytes;
//   * ensure_hmac_key persists a stable per-project secret on disk.
// The crash is injected by a test-only decorator around the native store (a production NativeFileStore
// never throws SimulatedCrash) so the intent log, atomic IO, HMAC integrity, jail, and CAS all run
// against actual files.

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/intent_log.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/kernel/platform.h"

#include "filesync_test.h"
#include "native_test_support.h"

#include <string>
#include <vector>

using namespace context::editor::filesync;
using nfstest::CrashInjectingFileStore;
using nfstest::TempDir;

namespace
{

const std::string kKey = "native-test-project-hmac-key-00001";

PlannedWrite plan(const std::string& path, const std::string& prev, const std::string& next)
{
    return PlannedWrite{path, content_hash(prev), content_hash(next), next};
}

bool has_code(const std::vector<Diagnostic>& diags, const std::string& code)
{
    for (const Diagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}

} // namespace

int main()
{
    // --- A. clean multi-file op on real disk: all writes land, entry cleared ----------------------
    {
        TempDir tmp("crash-clean");
        NativeFileStore fs(tmp.path());
        context::kernel::ManualClock clock;
        fs.write("proj/a.txt", "A0");
        fs.write("proj/b.txt", "B0");

        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        const std::vector<PlannedWrite> writes = {plan("proj/a.txt", "A0", "A1"),
                                                  plan("proj/b.txt", "B0", "B1")};
        CHECK(queue.execute("op-clean", writes));
        CHECK(*fs.read("proj/a.txt") == "A1");
        CHECK(*fs.read("proj/b.txt") == "B1");
        CHECK(log.pending().empty()); // cleared after the last durable rename
    }

    // --- B. crash mid-op on real disk -> resume to completion (nothing lost); idempotent ----------
    {
        TempDir tmp("crash-resume");
        NativeFileStore native(tmp.path());
        context::kernel::ManualClock clock;
        native.write("proj/a.txt", "A0");
        native.write("proj/b.txt", "B0");

        // The op runs through a crash-injecting decorator over the REAL store: a.txt commits to disk,
        // then the process "dies" before b.txt's rename.
        CrashInjectingFileStore fs(native);
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        const std::vector<PlannedWrite> writes = {plan("proj/a.txt", "A0", "A1"),
                                                  plan("proj/b.txt", "B0", "B1")};

        fs.crash_on_rename_to("proj/b.txt");
        bool crashed = false;
        try
        {
            queue.execute("op-crash", writes);
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);
        CHECK(*native.read("proj/a.txt") == "A1"); // first write durably committed on disk
        CHECK(*native.read("proj/b.txt") == "B0"); // second did not
        CHECK(!log.pending().empty());             // the intent entry survives the crash on real disk

        // Restart: a fresh incarnation over the (non-crashing) native store recovers. a.txt is already
        // applied (idempotent skip); b.txt is resumed to completion. Nothing is lost.
        IntentLog log2(native, "proj/.editor", kKey);
        WriteQueue queue2(native, "proj", log2, clock);
        const std::vector<Diagnostic> diags = queue2.recover();
        CHECK(diags.empty());
        CHECK(*native.read("proj/a.txt") == "A1");
        CHECK(*native.read("proj/b.txt") == "B1");
        CHECK(log2.pending().empty());

        // Idempotent replay: recovering again is a clean no-op.
        CHECK(queue2.recover().empty());
        CHECK(*native.read("proj/a.txt") == "A1");
        CHECK(*native.read("proj/b.txt") == "B1");
    }

    // --- C. file moved on since the crash -> CAS diagnostic, existing real bytes NOT clobbered -----
    {
        TempDir tmp("crash-cas");
        NativeFileStore native(tmp.path());
        context::kernel::ManualClock clock;
        native.write("proj/a.txt", "A0");

        CrashInjectingFileStore fs(native);
        IntentLog log(fs, "proj/.editor", kKey);
        WriteQueue queue(fs, "proj", log, clock);
        const std::vector<PlannedWrite> writes = {plan("proj/a.txt", "A0", "A1")};

        fs.crash_on_rename_to("proj/a.txt");
        bool crashed = false;
        try
        {
            queue.execute("op-cas", writes);
        }
        catch (const SimulatedCrash&)
        {
            crashed = true;
        }
        CHECK(crashed);

        // Something else rewrote a.txt to a THIRD state on disk before recovery ran.
        native.write("proj/a.txt", "A-external");

        IntentLog log2(native, "proj/.editor", kKey);
        WriteQueue queue2(native, "proj", log2, clock);
        const std::vector<Diagnostic> diags = queue2.recover();
        CHECK(has_code(diags, "filesync.intent.cas"));
        CHECK(*native.read("proj/a.txt") == "A-external"); // moved-on state preserved, not clobbered
    }

    // --- D. ensure_hmac_key persists a stable per-project secret on real disk ----------------------
    {
        TempDir tmp("crash-key");
        NativeFileStore fs(tmp.path());
        const std::string k1 = ensure_hmac_key(fs, "proj/.editor");
        const std::string k2 = ensure_hmac_key(fs, "proj/.editor");
        CHECK(k1.size() == 32);
        CHECK(k1 == k2); // second call reads the persisted key back from disk, does not regenerate
        CHECK(fs.exists("proj/.editor/hmac.key"));
    }

    FILESYNC_TEST_MAIN_END();
}

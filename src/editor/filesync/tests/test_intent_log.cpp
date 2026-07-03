// Crash-recovery intent log + write queue (R-FILE-004):
//   * a clean multi-file op applies all writes then clears the entry;
//   * a crash mid-op is RESUMED to completion on restart (nothing lost), and resume is idempotent;
//   * a corrupt entry, a foreign-log (wrong-key) entry, a jail-escaping write, and a moved-on
//     (CAS-mismatch) file each yield a machine-readable diagnostic instead of forcing state;
//   * ensure_hmac_key persists a stable per-project secret.

#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/intent_log.h"
#include "context/kernel/platform.h"
#include "filesync_test.h"

#include <string>
#include <vector>

using namespace context::editor::filesync;

namespace
{

const std::string kKey = "unit-test-project-hmac-key-000001";

PlannedWrite plan(const std::string& path, const std::string& prev, const std::string& next)
{
    return PlannedWrite{path, content_hash(prev), content_hash(next), next};
}

bool has_code(const std::vector<Diagnostic>& diags, const std::string& code)
{
    for (const Diagnostic& diagnostic : diags)
    {
        if (diagnostic.code == code)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    // --- A. clean multi-file op: all writes land, entry cleared ----------------------------------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        fs.write("proj/a.txt", "A0");
        fs.write("proj/b.txt", "B0");

        IntentLog log(fs, "proj/.editor", kKey, "incarnation-1");
        WriteQueue queue(fs, "proj", log, clock);

        const std::vector<PlannedWrite> writes = {plan("proj/a.txt", "A0", "A1"),
                                                  plan("proj/b.txt", "B0", "B1")};
        CHECK(queue.execute("op-clean", writes));
        CHECK(*fs.read("proj/a.txt") == "A1");
        CHECK(*fs.read("proj/b.txt") == "B1");
        CHECK(log.pending().empty()); // cleared after the last durable rename
    }

    // --- B. crash mid-op -> resume to completion (nothing lost); resume is idempotent ------------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        fs.write("proj/a.txt", "A0");
        fs.write("proj/b.txt", "B0");

        IntentLog log(fs, "proj/.editor", kKey, "incarnation-1");
        WriteQueue queue(fs, "proj", log, clock);
        const std::vector<PlannedWrite> writes = {plan("proj/a.txt", "A0", "A1"),
                                                  plan("proj/b.txt", "B0", "B1")};

        fs.crash_on_rename_to("proj/b.txt"); // crash after a.txt lands, before b.txt commits
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
        CHECK(*fs.read("proj/a.txt") == "A1"); // first write committed
        CHECK(*fs.read("proj/b.txt") == "B0"); // second did not
        CHECK(!log.pending().empty());         // intent entry survives the crash

        // Restart: a fresh incarnation recovers. a.txt is already applied (idempotent skip); b.txt is
        // resumed to completion. Nothing is lost.
        IntentLog log2(fs, "proj/.editor", kKey, "incarnation-2");
        WriteQueue queue2(fs, "proj", log2, clock);
        auto diags = queue2.recover();
        CHECK(diags.empty());
        CHECK(*fs.read("proj/a.txt") == "A1");
        CHECK(*fs.read("proj/b.txt") == "B1");
        CHECK(log2.pending().empty());

        // Idempotent replay: recovering again is a clean no-op.
        CHECK(queue2.recover().empty());
        CHECK(*fs.read("proj/a.txt") == "A1");
        CHECK(*fs.read("proj/b.txt") == "B1");
    }

    // --- C. corrupt entry -> integrity diagnostic, write NOT applied -----------------------------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey, "incarnation-1");
        WriteQueue queue(fs, "proj", log, clock);

        IntentEntry entry;
        entry.op_id = "op-corrupt";
        entry.incarnation_id = "incarnation-1";
        entry.writes = {plan("proj/x.txt", "", "X1")};
        CHECK(log.begin(entry));

        // Flip the last byte of the stored entry (in the HMAC-covered body).
        const std::string path = log.op_path("op-corrupt");
        std::string bytes = *fs.read(path);
        bytes.back() = static_cast<char>(bytes.back() ^ 0x01);
        fs.write(path, bytes);

        auto diags = queue.recover();
        CHECK(has_code(diags, "filesync.intent.integrity"));
        CHECK(!fs.exists("proj/x.txt")); // corrupt entry never forces state
    }

    // --- D. foreign-log (wrong key) -> integrity diagnostic, write NOT applied -------------------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;

        IntentLog writer(fs, "proj/.editor", kKey, "incarnation-1");
        IntentEntry entry;
        entry.op_id = "op-foreign";
        entry.incarnation_id = "incarnation-1";
        entry.writes = {plan("proj/y.txt", "", "Y1")};
        CHECK(writer.begin(entry));

        // A daemon holding a DIFFERENT project key cannot resume this foreign entry.
        IntentLog reader(fs, "proj/.editor", "a-completely-different-project-key", "incarnation-2");
        WriteQueue queue(fs, "proj", reader, clock);
        auto diags = queue.recover();
        CHECK(has_code(diags, "filesync.intent.integrity"));
        CHECK(!fs.exists("proj/y.txt"));
    }

    // --- E. jail-escaping planned write -> jail diagnostic, escape NOT written -------------------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        IntentLog log(fs, "proj/.editor", kKey, "incarnation-1");
        WriteQueue queue(fs, "proj", log, clock);

        IntentEntry entry;
        entry.op_id = "op-jail";
        entry.incarnation_id = "incarnation-1";
        entry.writes = {plan("proj/../evil.txt", "", "E")};
        CHECK(log.begin(entry));

        auto diags = queue.recover();
        CHECK(has_code(diags, "filesync.intent.jail"));
        CHECK(!fs.exists("evil.txt"));
    }

    // --- F. file moved on since the crash -> CAS diagnostic, existing content NOT clobbered ------
    {
        MemoryFileStore fs;
        context::kernel::ManualClock clock;
        fs.write("proj/a.txt", "A0");

        IntentLog log(fs, "proj/.editor", kKey, "incarnation-1");
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

        // Something else rewrote a.txt to a THIRD state before recovery ran.
        fs.write("proj/a.txt", "A-external");

        IntentLog log2(fs, "proj/.editor", kKey, "incarnation-2");
        WriteQueue queue2(fs, "proj", log2, clock);
        auto diags = queue2.recover();
        CHECK(has_code(diags, "filesync.intent.cas"));
        CHECK(*fs.read("proj/a.txt") == "A-external"); // moved-on state preserved, not clobbered
    }

    // --- G. ensure_hmac_key persists a stable per-project secret ---------------------------------
    {
        MemoryFileStore fs;
        const std::string k1 = ensure_hmac_key(fs, "proj/.editor");
        const std::string k2 = ensure_hmac_key(fs, "proj/.editor");
        CHECK(k1.size() == 32);
        CHECK(k1 == k2); // second call reads the persisted key, does not regenerate
        CHECK(fs.exists("proj/.editor/hmac.key"));
    }

    FILESYNC_TEST_MAIN_END();
}

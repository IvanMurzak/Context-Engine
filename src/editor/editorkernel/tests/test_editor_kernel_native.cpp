// Real-disk EditorKernel operation (M1 DoD): the composed headless kernel, wired over the native
// on-disk FileStore instead of MemoryFileStore, derives the World from ACTUAL files on disk.
//   * boot on a real project directory (single-instance lock acquired);
//   * a CLI-verb edit lands its bytes on REAL disk (through filesync atomic-IO) and reaches the derived
//     World under the read-your-writes barrier (R-CLI-006);
//   * a RAW out-of-band edit of the same file on real disk (bypassing the kernel entirely) is folded
//     into the derived World by the re-hash crawl — content hash is authoritative (R-FILE-002).
// MemoryFileStore stays selectable for deterministic tests (test_editor_kernel.cpp); this test proves
// the SAME composition runs against the filesystem.

#include "context/editor/editorkernel/editor_kernel.h"

#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include "editorkernel_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using context::editor::derivation::canonical_parse;
using context::editor::editorkernel::EditorKernel;
using context::editor::editorkernel::EditorKernelConfig;
using context::editor::editorkernel::EditOutcome;
using context::editor::filesync::NativeFileStore;
using context::editor::filesync::NullWatcher;
using context::editor::bridge::ScopeSet;
using context::editor::bridge::StartOutcome;

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-ek-native-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

int main()
{
    const fs::path project = make_temp_project("realdisk");

    // The native store roots at the real project dir; logical "proj/..." paths resolve under it, so the
    // daemon lock (project_root) and the file-sync root live on the same on-disk directory.
    NativeFileStore store(project);
    NullWatcher watcher; // portable default: always degraded -> correctness comes from the crawl
    context::kernel::ManualClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernelConfig cfg;
    cfg.project_root = project;                      // daemon single-instance lock (real FS)
    cfg.filesync_root = "proj";                      // logical FileStore root under the native base
    cfg.index_path = "proj/.editor/index"; // reconcile index (real file on disk)

    EditorKernel kernel(store, watcher, clock, tasks, cfg);
    const ScopeSet writer = ScopeSet::parse("write");

    // --- boot on a real directory: the daemon owns the Project (lock acquired) --------------------
    CHECK(kernel.start(ScopeSet::all()) == StartOutcome::booted);
    CHECK(kernel.running());
    CHECK(kernel.daemon().lock().held());
    CHECK(kernel.generation() == 0);

    // --- CLI-verb edit: bytes land on REAL disk and reach the derived World (read-your-writes) -----
    EditOutcome edit = kernel.edit_file("proj/a.scene", "entity: 1", writer);
    CHECK(edit.ok);
    CHECK(edit.ticket.path == "proj/a.scene");
    CHECK(edit.ticket.canonical_hash == canonical_parse("entity: 1").canonical_hash);

    // The bytes are on the actual filesystem, not just the seam abstraction.
    CHECK(store.exists("proj/a.scene"));
    CHECK(*store.read("proj/a.scene") == "entity: 1");
    CHECK(fs::exists(project / "proj" / "a.scene")); // really on disk

    auto observed = kernel.query_after_hash("proj/a.scene", edit.ticket.canonical_hash);
    CHECK(observed.has_value());
    CHECK(observed->canonical_hash == edit.ticket.canonical_hash);
    CHECK(kernel.generation() >= 1);
    CHECK(kernel.world().alive_count() == 1);

    // --- RAW out-of-band edit of the SAME file, straight to disk, reaches the derived World --------
    {
        const std::uint64_t before = kernel.query("proj/a.scene")->canonical_hash;

        // Bypass the kernel entirely: rewrite the real file on disk with an external tool (a plain
        // std::ofstream stands in for a hand edit / git checkout / another process).
        clock.advance(1'000'000'000ULL); // past the self-echo TTL (belt-and-braces; hash differs anyway)
        {
            std::ofstream out(project / "proj" / "a.scene", std::ios::binary | std::ios::trunc);
            CHECK(static_cast<bool>(out));
            out << "entity: 2";
        }
        // Confirm the raw edit is the store's truth now (read goes through the native FS).
        CHECK(*store.read("proj/a.scene") == "entity: 2");

        auto changes = kernel.ingest_external();
        bool saw = false;
        for (const auto& c : changes)
            if (c.path == "proj/a.scene")
                saw = true;
        CHECK(saw); // the re-hash crawl detected the raw on-disk edit (R-FILE-002)

        const std::uint64_t expected = canonical_parse("entity: 2").canonical_hash;
        kernel.settle();
        auto now = kernel.query("proj/a.scene");
        CHECK(now.has_value());
        CHECK(now->canonical_hash == expected);
        CHECK(now->canonical_hash != before); // the derived value genuinely changed
        CHECK(kernel.world().alive_count() == 1);
    }

    // --- the reconcile index persisted to a real file under .editor/ ------------------------------
    CHECK(kernel.reconciler().save_index());
    CHECK(store.exists("proj/.editor/index"));

    kernel.stop();
    CHECK(!kernel.running());

    // Best-effort cleanup of the real project dir (lock released by stop()).
    std::error_code ec;
    fs::remove_all(project, ec);

    EDITORKERNEL_TEST_MAIN_END();
}

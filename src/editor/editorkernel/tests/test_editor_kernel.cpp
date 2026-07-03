// Composed-loop integration smoke (M1 DoD): the merged file-authoritative libraries, wired into ONE
// runnable headless attach path, prove the end-to-end loop:
//   * the daemon boots (single-instance lock acquired) and a client attaches over the handshake;
//   * a CLI-verb edit (through filesync atomic-IO) reaches the derived World, honoring the
//     read-your-writes barrier (R-CLI-006);
//   * a RAW out-of-band edit of the same file also reaches the derived World (content hash is
//     authoritative, R-FILE-002);
//   * a second instance on the same Project gets the attach signal (R-BRIDGE-001);
//   * a scope-denied edit returns the R-SEC-007 permission class (exit code 6).
// This is the SMOKE scope of issue #30; the full 5-criteria M1 gate (kill-9 recovery, CLI≡RPC≡MCP
// parity, scope-denial e2e) is the follow-up exit-integration task.

#include "context/editor/editorkernel/editor_kernel.h"

#include "context/editor/contract/handshake.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include "editorkernel_test.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

namespace fs = std::filesystem;
using context::editor::contract::ClientHandshake;
using context::editor::contract::Envelope;
using context::editor::derivation::canonical_parse;
using context::editor::editorkernel::EditorKernel;
using context::editor::editorkernel::EditorKernelConfig;
using context::editor::editorkernel::EditOutcome;
using context::editor::filesync::MemoryFileStore;
using context::editor::filesync::NullWatcher;
using context::editor::bridge::Scope;
using context::editor::bridge::ScopeSet;
using context::editor::bridge::Session;
using context::editor::bridge::StartOutcome;

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-editorkernel-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

EditorKernelConfig config_for(const fs::path& project)
{
    EditorKernelConfig cfg;
    cfg.project_root = project;                     // real FS: the daemon single-instance lock
    cfg.filesync_root = "proj";                     // logical FileStore root (path jail)
    cfg.index_path = "proj/.editor/reconcile-index"; // under the injectable FileStore seam
    return cfg;
}
} // namespace

int main()
{
    const fs::path project = make_temp_project("loop");

    MemoryFileStore store;
    NullWatcher watcher; // portable default: always degraded -> correctness comes from the crawl
    context::kernel::ManualClock clock;
    context::kernel::InlineTaskRunner tasks;

    EditorKernel kernel(store, watcher, clock, tasks, config_for(project));

    // --- boot: the daemon owns the Project (lock acquired), stream + dispatcher up ----------------
    CHECK(kernel.start(ScopeSet::all()) == StartOutcome::booted);
    CHECK(kernel.running());
    CHECK(kernel.daemon().lock().held());
    CHECK(kernel.generation() == 0);

    // --- a second instance on the SAME project gets the attach signal (single-instance, R-BRIDGE-001)
    {
        MemoryFileStore store2;
        NullWatcher watcher2;
        context::kernel::ManualClock clock2;
        context::kernel::InlineTaskRunner tasks2;
        EditorKernel second(store2, watcher2, clock2, tasks2, config_for(project));
        CHECK(second.start() == StartOutcome::attach);
        CHECK(!second.running());
    }

    // --- a client attaches over the capability handshake, scopes clamped to the launch ceiling ----
    {
        ClientHandshake client;
        client.protocol_major = 0;
        client.capabilities = {"describe"};
        auto result = kernel.attach(client, ScopeSet::parse("write"));
        CHECK(std::holds_alternative<Session>(result));
        const Session& session = std::get<Session>(result);
        CHECK(session.attached);
        CHECK(session.scopes.has(Scope::file_write));
        CHECK(!session.scopes.has(Scope::build_install)); // not requested
    }

    // --- CLI-verb edit through filesync atomic-IO reaches the derived World (read-your-writes) -----
    const ScopeSet writer = ScopeSet::parse("write");
    {
        EditOutcome edit = kernel.edit_file("proj/a.scene", "entity: 1", writer);
        CHECK(edit.ok);
        CHECK(edit.error_code.empty());
        CHECK(edit.ticket.path == "proj/a.scene");
        CHECK(edit.ticket.canonical_hash == canonical_parse("entity: 1").canonical_hash);
        // The bytes actually landed on disk through atomic-IO.
        CHECK(store.exists("proj/a.scene"));
        CHECK(*store.read("proj/a.scene") == "entity: 1");

        // Before a pass runs the derived world has NOT changed yet (the write is pending).
        CHECK(kernel.generation() == 0);

        // Read-your-writes barrier (R-CLI-006): bounded-block until the world reflects the write's
        // canonical hash, then the query observes it.
        auto observed = kernel.query_after_hash("proj/a.scene", edit.ticket.canonical_hash);
        CHECK(observed.has_value());
        CHECK(observed->canonical_hash == edit.ticket.canonical_hash);
        CHECK(kernel.generation() >= 1);
        CHECK(kernel.world().alive_count() == 1);

        // The envelope a CLI/RPC caller returns: ok, generationAfter set, exit 0.
        const Envelope env = edit.envelope();
        CHECK(env.ok());
        CHECK(env.exit_code() == 0);
    }

    // --- a RAW out-of-band edit of the SAME file also reaches the derived World --------------------
    {
        const std::uint64_t before = kernel.query("proj/a.scene")->canonical_hash;

        // Bypass the kernel: mutate the file directly on the FileStore (a hand edit / another tool).
        clock.advance(1'000'000'000ULL); // past the self-echo TTL — belt-and-braces (hash differs anyway)
        CHECK(store.write("proj/a.scene", "entity: 2"));

        auto changes = kernel.ingest_external();
        bool saw = false;
        for (const auto& c : changes)
            if (c.path == "proj/a.scene")
                saw = true;
        CHECK(saw); // the crawl detected the raw edit (content hash authoritative, R-FILE-002)

        const std::uint64_t expected = canonical_parse("entity: 2").canonical_hash;
        kernel.settle();
        auto observed = kernel.query("proj/a.scene");
        CHECK(observed.has_value());
        CHECK(observed->canonical_hash == expected);
        CHECK(observed->canonical_hash != before); // the derived value genuinely changed
        CHECK(kernel.world().alive_count() == 1);   // same source entity, new derived value
    }

    // --- R-SEC-007: a scope-denied edit is refused with the permission class (exit code 6) ---------
    {
        const std::uint64_t gen_before = kernel.generation();
        EditOutcome denied = kernel.edit_file("proj/b.scene", "entity: 9", ScopeSet::read_query());
        CHECK(!denied.ok);
        CHECK(denied.error_code == "scope.denied");

        const Envelope env = denied.envelope();
        CHECK(!env.ok());
        CHECK(env.exit_code() == 6); // permission class (was generic 1 before the catalog fix)

        // Nothing was written and the world did not change.
        CHECK(!store.exists("proj/b.scene"));
        CHECK(kernel.generation() == gen_before);
    }

    // --- a path-jail escape is refused (R-SEC-008), even with the write scope ---------------------
    {
        EditOutcome escape = kernel.edit_file("proj/../escape.scene", "nope", writer);
        CHECK(!escape.ok);
        CHECK(escape.error_code == "path.jail_violation");
        CHECK(escape.envelope().exit_code() == 6);
        CHECK(!store.exists("escape.scene"));
    }

    kernel.stop();
    CHECK(!kernel.running());

    // Best-effort cleanup of the real lock directory.
    std::error_code ec;
    fs::remove_all(project, ec);

    EDITORKERNEL_TEST_MAIN_END();
}

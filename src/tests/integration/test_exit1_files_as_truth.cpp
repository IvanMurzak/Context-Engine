// M1 exit criterion 1 — files-as-truth equivalence (L-19 / R-ARCH-002, issue #36):
// mutate a scene file via a CLI VERB and via a RAW TEXT EDIT and observe the IDENTICAL derived
// World headless — byte-identical derived state through both paths.
//
//   Part A (headless, real disk, in-process): two fresh EditorKernels over the native on-disk
//          FileStore derive the SAME project state — one mutated exclusively through the CLI-verb
//          path (edit_file), the other exclusively through raw std::ofstream writes + the
//          watch-hash-reconcile crawl (R-FILE-002). The serialized derived state of both worlds is
//          compared BYTE-FOR-BYTE. Includes the canonical-form edge (L-22): raw bytes that differ
//          only in insignificant whitespace derive to the identical canonical state.
//   Part B (cross-process, REAL daemon over the REAL IPC wire, #35): a real `context attach`
//          process performs the CLI-verb mutation; the test performs the raw text edit directly on
//          disk; the daemon folds it in via `reconcile`; both derived nodes are read back over the
//          wire and must carry the identical canonical hash.
//
// R-QA-013: happy path + edge cases (whitespace canonicalization, same-file two-path convergence,
// removal) + failure path (divergent content must NOT be identical — the control).

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/json.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/watcher.h"
#include "context/kernel/platform.h"

#include "integration_test.h"
#include "process_util.h"

#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace context;
using editor::editorkernel::EditorKernel;
using editor::editorkernel::EditorKernelConfig;
using editor::editorkernel::EditOutcome;
using itest::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{

// One headless kernel composed over the native on-disk FileStore (the proven daemon composition).
struct NativeKernel
{
    explicit NativeKernel(const fs::path& project)
        : store(project), kernel(store, watcher, clock, tasks, make_config(project))
    {
    }

    static EditorKernelConfig make_config(const fs::path& project)
    {
        EditorKernelConfig cfg;
        cfg.project_root = project;
        cfg.filesync_root = "proj";
        cfg.index_path = "proj/.editor/reconcile-index";
        return cfg;
    }

    editor::filesync::NativeFileStore store;
    editor::filesync::NullWatcher watcher;
    kernel::SteadyClock clock;
    kernel::InlineTaskRunner tasks;
    EditorKernel kernel;
};

// Serialize the OBSERVABLE derived state for a set of source paths: per-path canonical hash (the
// derived value identity) + the world population. Deliberately excludes generation counters (write
// ORDER identity, not derived-value identity). Byte-comparing two of these is the "byte-identical
// derived state" assertion of criterion 1.
std::string derived_state(const EditorKernel& k, const std::vector<std::string>& paths)
{
    std::string out;
    for (const std::string& p : paths)
    {
        const auto node = k.query(p);
        out += p + "=>";
        out += node.has_value() ? std::to_string(node->canonical_hash) : std::string("absent");
        out += ";";
    }
    out += "alive=" + std::to_string(k.world().alive_count());
    return out;
}

} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;

    // ============================================================================================
    // Part A — headless equivalence on real disk: CLI-verb world ≡ raw-edit world, byte-for-byte.
    // ============================================================================================
    {
        const fs::path pa = itest::make_temp_project("truth-cli");
        const fs::path pb = itest::make_temp_project("truth-raw");

        NativeKernel a(pa);
        NativeKernel b(pb);
        CHECK(a.kernel.start() == editor::bridge::StartOutcome::booted);
        CHECK(b.kernel.start() == editor::bridge::StartOutcome::booted);

        const std::string scene = "proj/a.scene";
        const std::string content = "entity: 1";

        // Kernel A: the CLI-verb path (scope-checked daemon-initiated write through filesync).
        const EditOutcome ea = a.kernel.edit_file(scene, content, editor::bridge::ScopeSet::all());
        CHECK(ea.ok);
        CHECK(a.kernel.await_hash(ea.ticket.canonical_hash).ok());

        // Kernel B: the raw text edit — a plain out-of-band ofstream write, folded in by the
        // reconcile crawl (content hash authoritative, R-FILE-002). Raw bytes differ ONLY in
        // insignificant whitespace, so the canonical form (L-22) must be identical.
        CHECK(itest::write_file_raw(pb / "proj" / "a.scene", "  entity:   1  \n"));
        CHECK(!b.kernel.ingest_external().empty());
        b.kernel.settle();

        // THE criterion: identical derived World through both paths, byte-identical serialized state.
        const std::string state_a = derived_state(a.kernel, {scene});
        const std::string state_b = derived_state(b.kernel, {scene});
        CHECK(!state_a.empty());
        CHECK(state_a == state_b);
        CHECK(a.kernel.query(scene).has_value());
        CHECK(a.kernel.query(scene)->canonical_hash == b.kernel.query(scene)->canonical_hash);
        CHECK(a.kernel.world().alive_count() == 1);
        CHECK(b.kernel.world().alive_count() == 1);

        // Failure-path control: DIVERGENT content must NOT be identical.
        const EditOutcome ea2 =
            a.kernel.edit_file("proj/b.scene", "entity: 2", editor::bridge::ScopeSet::all());
        CHECK(ea2.ok);
        CHECK(a.kernel.await_hash(ea2.ticket.canonical_hash).ok());
        CHECK(itest::write_file_raw(pb / "proj" / "b.scene", "entity: 3"));
        CHECK(!b.kernel.ingest_external().empty());
        b.kernel.settle();
        CHECK(derived_state(a.kernel, {"proj/b.scene"}) != derived_state(b.kernel, {"proj/b.scene"}));

        // Edge: SAME file, both paths in sequence — the raw edit overrides the CLI verb (files are
        // the truth), then the CLI verb overrides back; the derived node tracks each.
        const EditOutcome ex =
            a.kernel.edit_file("proj/x.scene", "entity: 10", editor::bridge::ScopeSet::all());
        CHECK(ex.ok);
        CHECK(a.kernel.await_hash(ex.ticket.canonical_hash).ok());
        CHECK(itest::write_file_raw(pa / "proj" / "x.scene", "entity: 11"));
        CHECK(!a.kernel.ingest_external().empty());
        a.kernel.settle();
        const auto raw_won = a.kernel.query("proj/x.scene");
        CHECK(raw_won.has_value());
        CHECK(raw_won->canonical_hash ==
              editor::derivation::canonical_parse("entity: 11").canonical_hash);
        const EditOutcome ex2 =
            a.kernel.edit_file("proj/x.scene", "entity: 12", editor::bridge::ScopeSet::all());
        CHECK(ex2.ok);
        CHECK(a.kernel.await_hash(ex2.ticket.canonical_hash).ok());
        CHECK(a.kernel.query("proj/x.scene")->canonical_hash ==
              editor::derivation::canonical_parse("entity: 12").canonical_hash);

        // Edge: a raw REMOVAL converges too (the file is gone => the derived node is gone).
        std::error_code ec;
        fs::remove(pb / "proj" / "b.scene", ec);
        CHECK(!b.kernel.ingest_external().empty());
        b.kernel.settle();
        CHECK(!b.kernel.query("proj/b.scene").has_value());

        a.kernel.stop();
        b.kernel.stop();
        fs::remove_all(pa, ec);
        fs::remove_all(pb, ec);
    }

    // ============================================================================================
    // Part B — the same equivalence over the REAL daemon process + REAL IPC wire (#35).
    // ============================================================================================
    {
        const fs::path project = itest::make_temp_project("truth-wire");

        ctest_proc::Process daemon =
            ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
        CHECK(ctest_proc::valid(daemon));
        CHECK(itest::wait_for_instance(project, 15000));

        // CLI-verb mutation by a REAL separate CLI process over the wire.
        ctest_proc::Process attach = ctest_proc::spawn(
            bin, {"attach", "--project", project.string(), "--set-path", "proj/cli.scene",
                  "--set-content", "entity: 9"});
        CHECK(ctest_proc::valid(attach));
        int attach_code = -1;
        const bool attach_done = ctest_proc::wait_for(attach, 25000, attach_code);
        if (!attach_done)
            ctest_proc::kill(attach);
        ctest_proc::release(attach);
        CHECK(attach_done);
        CHECK(attach_code == 0);

        // Raw text edit, bypassing the engine entirely — whitespace-variant of the same content.
        CHECK(itest::write_file_raw(project / "proj" / "raw.scene", "  entity:   9 \n"));

        // Read both derived nodes back over the wire; the daemon folds the raw edit in via the
        // reconcile crawl. Identical canonical hash == identical derived World state for the pair.
        itest::RpcClient rpc;
        CHECK(rpc.connect(project));
        const auto attached = rpc.attach(0, {"describe"}, "session");
        CHECK(itest::is_ok(attached));

        const auto reconciled = rpc.call("reconcile", Json::object());
        CHECK(itest::is_ok(reconciled));

        Json q1 = Json::object();
        q1.set("path", Json(std::string("proj/cli.scene")));
        const auto cli_node = rpc.call("query", std::move(q1));
        Json q2 = Json::object();
        q2.set("path", Json(std::string("proj/raw.scene")));
        const auto raw_node = rpc.call("query", std::move(q2));
        CHECK(itest::is_ok(cli_node));
        CHECK(itest::is_ok(raw_node));
        if (itest::is_ok(cli_node) && itest::is_ok(raw_node))
        {
            const Json& c = cli_node->at("result").at("data");
            const Json& r = raw_node->at("result").at("data");
            CHECK(c.at("present").as_bool());
            CHECK(r.at("present").as_bool());
            const std::string ch = c.at("canonicalHash").as_string();
            const std::string rh = r.at("canonicalHash").as_string();
            CHECK(!ch.empty());
            CHECK(ch == rh); // byte-identical derived state through both mutation paths
            // and it is the canonical form both raw byte streams reduce to (L-22):
            CHECK(ch == std::to_string(
                            editor::derivation::canonical_parse("entity: 9").canonical_hash));
        }

        const auto stopped = rpc.call("shutdown", Json::object());
        CHECK(itest::is_ok(stopped));
        rpc.close();

        int daemon_code = -1;
        const bool daemon_done = ctest_proc::wait_for(daemon, 15000, daemon_code);
        if (!daemon_done)
            ctest_proc::kill(daemon);
        ctest_proc::release(daemon);
        CHECK(daemon_done);
        if (daemon_done)
            CHECK(daemon_code == 0);

        std::error_code ec;
        fs::remove_all(project, ec);
    }

    ITEST_MAIN_END();
}

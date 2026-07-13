// The L-37 parse-time migration node (M2 wave 3) wired into the derivation graph: stamped-older
// payloads migrate IN MEMORY on ingest (disk identity untouched — the ticket still carries the
// AUTHORED canonical hash); blocking migration findings (newer-than stamps, failed steps) retain
// last-good derived state exactly like a failed validation; non-blocking orphan-override findings
// surface through validation(); and the registered-set hash is the second memoization key
// component (R-FILE-005: a pass-0 set change re-keys pass-1 — identical content re-derives).
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/migrate/migration_set.h"
#include "context/editor/schema/kind_schema.h"

#include "derivation_test.h"
#include "mock_runner.h" // the migrate module's byte-only seam mock (sandboxed VM-slot cases)

#include <algorithm>
#include <string>
#include <string_view>

using context::editor::derivation::DerivationConfig;
using context::editor::derivation::DerivationGraph;
using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;
namespace migrate = context::editor::migrate;
namespace schema = context::editor::schema;
using context::editor::serializer::JsonValue;

namespace
{
ReconcileChange change(std::string path, ChangeType type)
{
    ReconcileChange c;
    c.path = std::move(path);
    c.type = type;
    return c;
}

// One-step test set: "test:sprite" v1 -> v2 renames tint -> color and DROPS size (so /size paths
// orphan); "phys:body" is registered at current 1 (a v2-stamped payload is newer-than-package);
// "test:bad" v1 -> v2 always fails.
migrate::MigrationSet make_set()
{
    migrate::MigrationSet set;
    std::string problem;
    bool ok = set.register_component("test:sprite", 2, problem);
    ok = set.register_component("phys:body", 1, problem) && ok;
    ok = set.register_component("test:bad", 2, problem) && ok;

    migrate::MigrationStep rename;
    rename.component_type = "test:sprite";
    rename.from_version = 1;
    rename.transform = [](JsonValue& p) {
        for (auto& m : p.members)
            if (m.key == "tint")
                m.key = "color";
        p.members.erase(std::remove_if(p.members.begin(), p.members.end(),
                                       [](const auto& m) { return m.key == "size"; }),
                        p.members.end());
        return true;
    };
    rename.map_path = [](std::string_view p) -> std::optional<std::string> {
        if (p == "/tint")
            return std::string("/color");
        if (p == "/size")
            return std::nullopt;
        return std::string(p);
    };
    ok = set.register_step(std::move(rename), problem) && ok;

    migrate::MigrationStep failing;
    failing.component_type = "test:bad";
    failing.from_version = 1;
    failing.transform = [](JsonValue&) { return false; };
    ok = set.register_step(std::move(failing), problem) && ok;

    if (!ok)
        std::fprintf(stderr, "test set registration failed: %s\n", problem.c_str());
    return set;
}

const char* kOldStamped = R"({
  "componentVersions": {"test:sprite": 1},
  "entities": [{"components": {"test:sprite": {"size": 4, "tint": "red"}}}],
  "instances": [{"overrides": [{"path": "components/test:sprite/size", "value": 8}], "source": "t"}]
})";

const char* kNewerStamped = R"({
  "componentVersions": {"phys:body": 2},
  "c": {"phys:body": {"m": 1}}
})";

const char* kFailingStep = R"({
  "componentVersions": {"test:bad": 1},
  "c": {"test:bad": {"a": 1}}
})";
} // namespace

int main()
{
    const migrate::MigrationSet set = make_set();

    // --- happy path: an old-stamped payload migrates in memory and derives -----------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas(), nullptr, &set);
        const auto ticket =
            graph.apply(change("a.doc.json", ChangeType::created), kOldStamped);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_derived == 1);
        CHECK(pass.nodes_invalid == 0);
        CHECK(graph.node("a.doc.json").has_value());
        // The ticket's canonical hash is the AUTHORED identity (the own-write barrier keys on
        // what is on disk, not the migrated in-memory view).
        CHECK(graph.reflects_hash(ticket.canonical_hash));

        // The orphaned override surfaces as a NON-blocking finding through validation().
        auto v = graph.validation("a.doc.json");
        CHECK(v.has_value());
        CHECK(v->report.ok); // orphans do not block
        bool orphan_seen = false;
        for (const auto& d : v->report.diagnostics)
            if (d.code == "migration.orphan_override")
            {
                orphan_seen = true;
                CHECK(d.pointer == "/instances/0/overrides/0/path");
            }
        CHECK(orphan_seen);
    }

    // --- downgrade rule: newer-than stamps block; last-good state is retained --------------------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas(), nullptr, &set);
        // First a GOOD ingest derives (no stamps at all: migration is a no-op).
        graph.apply(change("b.doc.json", ChangeType::created), R"({"c": {"phys:body": {"m": 1}}})");
        auto pass1 = graph.run_pass();
        CHECK(pass1.nodes_derived == 1);
        const auto derived = graph.node("b.doc.json");
        CHECK(derived.has_value());

        // Then a NEWER-stamped rewrite: blocked, node keeps its last-good value + generation.
        graph.apply(change("b.doc.json", ChangeType::modified), kNewerStamped);
        auto pass2 = graph.run_pass();
        CHECK(pass2.nodes_invalid == 1);
        CHECK(pass2.nodes_derived == 0);
        const auto still = graph.node("b.doc.json");
        CHECK(still.has_value());
        CHECK(still->canonical_hash == derived->canonical_hash);
        CHECK(still->generation == derived->generation);

        auto v = graph.validation("b.doc.json");
        CHECK(v.has_value());
        CHECK(!v->report.ok);
        CHECK(!v->report.diagnostics.empty());
        CHECK(v->report.diagnostics[0].code == "schema.newer_than_package");
        CHECK(v->report.diagnostics[0].pointer == "/componentVersions/phys:body");
        CHECK(v->report.diagnostics[0].line == 2); // located in the AUTHORED source bytes
    }

    // --- a failing step blocks a NEW source from deriving (never a half-migrated derive) ---------
    {
        DerivationGraph graph({}, nullptr, &schema::engine_schemas(), nullptr, &set);
        graph.apply(change("c.doc.json", ChangeType::created), kFailingStep);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 1);
        CHECK(pass.nodes_derived == 0);
        CHECK(!graph.node("c.doc.json").has_value());
        auto v = graph.validation("c.doc.json");
        CHECK(v.has_value());
        CHECK(!v->report.ok);
        CHECK(v->report.diagnostics[0].code == "migration.step_failed");
    }

    // --- the migration node also runs with NO kind schemas wired (independent seams) -------------
    {
        DerivationGraph graph({}, nullptr, nullptr, nullptr, &set);
        graph.apply(change("d.doc.json", ChangeType::created), kNewerStamped);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_invalid == 1);
        CHECK(!graph.node("d.doc.json").has_value());
        auto v = graph.validation("d.doc.json");
        CHECK(v.has_value());
        CHECK(!v->report.ok);
    }

    // --- R-FILE-005 keying: the registered-set hash is a memoization key component ---------------
    {
        DerivationConfig config;
        config.registered_set_hash = set.set_hash();
        DerivationGraph graph(config, nullptr, nullptr, nullptr, &set);
        CHECK(graph.registered_set_hash() == set.set_hash());

        const char* content = R"({"plain": true})";
        graph.apply(change("e.json", ChangeType::created), content);
        auto pass1 = graph.run_pass();
        CHECK(pass1.nodes_derived == 1);

        // Identical content under the SAME set memoizes away.
        graph.apply(change("e.json", ChangeType::modified), content);
        auto pass2 = graph.run_pass();
        CHECK(pass2.nodes_skipped == 1);
        CHECK(pass2.nodes_derived == 0);

        // A pass-0 set change re-keys pass-1: identical content RE-derives under the new hash
        // (a package upgrade never serves derivations made under the old migration set).
        graph.set_registered_set_hash(set.set_hash() ^ 0xabcdef);
        graph.apply(change("e.json", ChangeType::modified), content);
        auto pass3 = graph.run_pass();
        CHECK(pass3.nodes_derived == 1);
        CHECK(pass3.nodes_skipped == 0);

        // And memoizes again once stable under the new set.
        graph.apply(change("e.json", ChangeType::modified), content);
        auto pass4 = graph.run_pass();
        CHECK(pass4.nodes_skipped == 1);
    }

    // --- no-migrations graphs are byte-for-byte the M1 behavior (seam off) -----------------------
    {
        DerivationGraph graph;
        graph.apply(change("f.json", ChangeType::created), kOldStamped);
        auto pass = graph.run_pass();
        CHECK(pass.nodes_derived == 1); // no migration, no validation: derives as plain content
        CHECK(!graph.validation("f.json").has_value());
    }

    // --- sandboxed-tier VM slot (issue #71): the graph's injected MigrationRunner routes
    // package_sandboxed steps at parse time; with NO runner the honest refusal blocks ------------
    {
        migrate::MigrationSet pkg_set;
        std::string problem;
        CHECK(pkg_set.register_component("phys:body", 2, problem));
        migrate::MigrationStep pkg_step;
        pkg_step.component_type = "phys:body";
        pkg_step.from_version = 1;
        pkg_step.tier = migrate::MigrationTier::package_sandboxed;
        pkg_step.wasm_module = "phys/migrations.wasm";
        CHECK(pkg_set.register_step(std::move(pkg_step), problem));
        const char* pkg_doc =
            R"({"componentVersions": {"phys:body": 1}, "c": {"phys:body": {"hp": 5}}})";

        // (a) No runner injected: package steps are refused in-process (runner_unavailable) and
        // the NEW source never derives — never run unsandboxed (L-37).
        {
            DerivationGraph graph({}, nullptr, &schema::engine_schemas(), nullptr, &pkg_set);
            graph.apply(change("pkg.doc.json", ChangeType::created), pkg_doc);
            auto pass = graph.run_pass();
            CHECK(pass.nodes_invalid == 1);
            CHECK(pass.nodes_derived == 0);
            CHECK(!graph.node("pkg.doc.json").has_value());
            auto v = graph.validation("pkg.doc.json");
            CHECK(v.has_value());
            CHECK(!v->report.ok);
            CHECK(v->report.diagnostics[0].code == "migration.runner_unavailable");
        }

        // (b) A runner injected at the graph's VM slot (the R-FILE-005 cold-start "VM" component,
        // threaded from EditorKernelConfig::migration_runner): the SAME ingest routes the package
        // step through the seam and derives.
        {
            migratetest::MockMigrationRunner runner([](std::string_view in, std::string& out) {
                out.assign(in.data(), in.size()); // identity guest — routing is the subject
                return true;
            });
            DerivationGraph graph({}, nullptr, &schema::engine_schemas(), nullptr, &pkg_set,
                                  &runner);
            graph.apply(change("pkg.doc.json", ChangeType::created), pkg_doc);
            auto pass = graph.run_pass();
            CHECK(pass.nodes_derived == 1);
            CHECK(pass.nodes_invalid == 0);
            CHECK(graph.node("pkg.doc.json").has_value());
            CHECK(runner.run_calls == 1);
        }
    }

    DERIVATION_TEST_MAIN_END();
}

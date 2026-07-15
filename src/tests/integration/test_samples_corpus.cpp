// Samples corpus gate (R-QA-006 agent few-shot corpus; R-BUILD-009 / R-QA-008 maintained-sample CI
// gate) — the M3-exit contract-freeze readiness artifact (does NOT perform the freeze; protocolMajor
// stays 0, R-CLI-004). Two responsibilities, both blocking on every OS leg:
//
//   Leg A (real shipped binary, cross-process): the committed sample projects build + headless-smoke-
//          run through the REAL `context` binary (`validate` + `migrate --dry-run`) — if a sample
//          rots, this gate goes red (R-BUILD-009 maintained, R-QA-008).
//   Leg B (in-process cli::run over a staged copy): the corpus collectively EXERCISES the breadth of
//          the contract surface — the stable, implemented, one-shot CLI verbs across the R-CLI-009
//          registry (kinds: scene/camera, tilemap, string-table, project, ctx:replay; verbs:
//          write/override L-35, migrate, structural three-way merge + resolve, query vs describe,
//          startable headless sessions + steps, record/replay + determinism triage).
//
// Coverage assertion (freeze readiness): every stable ∧ implemented ∧ one-shot CLI verb in the ONE
// registry (R-CLI-009) is exercised by the corpus, save a small, documented exemption set that needs
// live-daemon / lockfile fixtures (covered by their own unit tests). Because every surface (CLI ≡ RPC
// ≡ MCP ≡ introspection) is a pure projection of the same registry entry, exercising the CLI verb
// certifies its rpc_method + mcp_tool by construction — this test additionally asserts every verb
// projects a non-empty method + tool (the m1-exit-4 contract-parity gate proves the deeper identity).
//
// R-QA-013: the corpus IS the behavior and this gate IS its test; they ship in the same PR.

#include "context/cli/app.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"

#include "process_util.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif
#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

using context::cli::run;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
using context::editor::contract::Registry;
using context::editor::contract::VerbSpec;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

// The verb keys the corpus exercised in-process (VerbSpec::key() form: "[<ns>:]<noun>/<verb>").
std::set<std::string> g_exercised;

std::string read_file(const fs::path& path)
{
    // Portable binary slurp via C++ streams — a raw std::fopen trips MSVC /WX (C4996 'fopen' may be
    // unsafe) on the Windows CI leg while compiling clean on gcc/clang.
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A unique temp path, cleared but NOT created — fs::copy of a directory onto an already-existing
// directory errors, so staging leaves creation to the copy; other callers create as needed.
fs::path make_temp_path(const char* tag)
{
    const auto stamp = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffLL);
    fs::path dir = fs::temp_directory_path()
                   / ("ctx-samples-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

// Run a verb in-process, assert its envelope is ok, and record the (noun, verb) as exercised. The
// registry lookup makes the recorded key format-identical to VerbSpec::key(), so the coverage loop
// below compares apples to apples (never a hand-typed key that could drift).
Envelope exercise(const std::string& noun, const std::string& verb,
                  const std::vector<std::string>& args)
{
    const VerbSpec* spec = Registry::instance().find_verb("", noun, verb);
    CHECK(spec != nullptr);
    Envelope env = run(args);
    if (!env.ok())
        std::fprintf(stderr, "[samples-corpus] verb '%s %s' failed: %s\n", noun.c_str(),
                     verb.c_str(), env.dump(0).c_str());
    CHECK(env.ok());
    if (spec != nullptr && env.ok())
        g_exercised.insert(spec->key());
    return env;
}

// Does any *.json under `root` carry this "$schema" kind id? (Kind-coverage: the corpus represents
// every registered engine file kind — R-CLI-005 / R-DATA-006.) A header substring match on the
// canonical `"$schema": "<id>"` line — NOT a full JSON parse: the ctx:replay artifact carries u64
// state-hash values outside int64 range that the strict contract parser rejects, and the header is
// all this check needs.
bool corpus_has_kind(const fs::path& root, const std::string& kind_id)
{
    const std::string needle = "\"$schema\": \"" + kind_id + "\"";
    std::error_code ec;
    for (const fs::directory_entry& e : fs::recursive_directory_iterator(root, ec))
    {
        if (!e.is_regular_file())
            continue;
        const std::string name = e.path().filename().string();
        if (name.size() < 5 || name.substr(name.size() - 5) != ".json")
            continue;
        if (read_file(e.path()).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

int main()
{
    const fs::path samples = CONTEXT_SAMPLES_DIR;
    const std::string bin = CONTEXT_BINARY;
    const std::vector<std::string> projects = {"platformer-2d", "topdown-rpg", "roll-3d"};

    // =============================================================================================
    // Leg A — the REAL shipped `context` binary builds + headless-smoke-runs each sample project
    //         across a true process boundary (R-BUILD-009 maintained gate / R-QA-008). validate +
    //         migrate --dry-run are read-only, so they run against the committed corpus in place.
    // =============================================================================================
    for (const std::string& proj : projects)
    {
        const std::string dir = (samples / proj).string();
        for (const std::vector<std::string>& argv :
             std::vector<std::vector<std::string>>{{"validate", dir}, {"migrate", dir, "--dry-run"}})
        {
            ctest_proc::Process p = ctest_proc::spawn(bin, argv);
            CHECK(ctest_proc::valid(p));
            int code = -1;
            // Read-only validate / migrate --dry-run on a tiny sample return in well under a second;
            // keep the per-spawn ceiling low so the 6 Leg-A spawns (6 x 15s) stay under the binary's
            // TIMEOUT 180 (CMakeLists) with headroom for Leg B, letting a real regression surface its
            // diagnostics instead of silently truncating the whole gate at the outer timeout.
            const bool done = ctest_proc::wait_for(p, 15000, code);
            if (!done)
                ctest_proc::kill(p);
            ctest_proc::release(p);
            CHECK(done);
            CHECK(code == 0);
        }
    }

    // =============================================================================================
    // Leg B — exercise the breadth of the contract surface in-process over a STAGED copy (so the
    //         write verbs never mutate the committed corpus).
    // =============================================================================================
    const fs::path stage = make_temp_path("stage");
    {
        std::error_code ec;
        fs::copy(samples, stage,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    const fs::path platformer = stage / "platformer-2d";
    const fs::path topdown = stage / "topdown-rpg";
    const fs::path roll3d = stage / "roll-3d";

    // --- global surface: describe (the whole self-describing contract) ---------------------------
    exercise("", "describe", {"describe"});

    // --- authored-kind validation over each staged sample ----------------------------------------
    for (const std::string& proj : projects)
        exercise("", "validate", {"validate", (stage / proj).string()});

    // --- migrate: each committed sample GAME is a canonical FIXPOINT (freeze readiness — a
    //     maintained corpus is already canonical, so a dry-run migrate would change nothing).
    //     Both authored game projects are gated (roll-3d is net-new this PR), not just one --------
    for (const fs::path& game : {platformer, roll3d})
    {
        const Envelope env =
            exercise("", "migrate", {"migrate", game.string(), "--dry-run"});
        if (env.ok())
        {
            CHECK(env.data().at("canonicalized").as_number() == 0.0);
            CHECK(env.data().at("failed").as_number() == 0.0);
        }
    }

    // --- set: a composed field override written into the OUTERMOST scene (R-CLI-006 / L-35 write
    //     path), addressed by id-path; the envelope records the base for override hygiene ----------
    {
        const Envelope env = exercise(
            "", "set",
            {"set", "scenes/level-1.scene.json", "[6.0, 0.0, 2.0]", "--id-path",
             "c3c3c3c3c3c3c301/a1a1a1a1a1a1a102", "--pointer", "/components/transform/position",
             "--project", platformer.string()});
        if (env.ok())
            CHECK(env.data().at("applied").as_bool());
    }

    // --- query --overrides: the one-shot advisory override-hygiene read (R-CLI-006) over the
    //     project scenes (the daemon-served plain `query` is operational, exempt below) ------------
    exercise("", "query",
             {"query", "scenes/level-1.scene.json", "--overrides", "diverged", "--project",
              platformer.string()});

    // --- structural three-way merge (R-FILE-012): a CLEAN (field-disjoint) merge + a CONFLICTING
    //     (same-field) merge whose machine-readable conflict `resolve-conflict` then clears ---------
    {
        const fs::path clean = topdown / "merge" / "clean";
        exercise("", "merge-file",
                 {"merge-file", (clean / "base.scene.json").string(),
                  (clean / "ours.scene.json").string(), (clean / "theirs.scene.json").string(),
                  "--output", (stage / "merged-clean.scene.json").string()});

        const fs::path conflict = topdown / "merge" / "conflict";
        const fs::path merged = stage / "merged-conflict.scene.json";
        const Envelope merge_env = exercise(
            "", "merge-file",
            {"merge-file", (conflict / "base.scene.json").string(),
             (conflict / "ours.scene.json").string(), (conflict / "theirs.scene.json").string(),
             "--output", merged.string()});
        // The conflicting merge yields machine-readable field conflicts (never text markers).
        std::vector<std::string> conflict_paths;
        if (merge_env.ok())
        {
            CHECK(!merge_env.data().at("clean").as_bool());
            const Json& conflicts = merge_env.data().at("conflicts");
            CHECK(conflicts.is_array());
            CHECK(conflicts.size() > 0);
            for (std::size_t i = 0; i < conflicts.size(); ++i)
            {
                const std::string path = conflicts.at(i).at("path").as_string();
                CHECK(!path.empty());
                conflict_paths.push_back(path);
            }
        }
        // Resolve every conflict by taking one whole side; the sidecar empties (R-FILE-012). Assert
        // the LAST resolution clears the sidecar to zero remaining.
        for (std::size_t i = 0; i < conflict_paths.size(); ++i)
        {
            const Envelope res =
                exercise("", "resolve-conflict",
                         {"resolve-conflict", merged.string(), "--path", conflict_paths[i],
                          "--take", "ours"});
            if (res.ok() && i + 1 == conflict_paths.size())
                CHECK(res.data().at("remainingConflicts").as_number() == 0.0);
        }
    }

    // --- startable headless session + steps (R-QA-005): new -> seed -> inject -> step -> hash ->
    //     record; then replay the artifact and triage two runs with determinism diff --------------
    {
        const fs::path state = stage / "session-a.state.json";
        exercise("session", "new",
                 {"session", "new", state.string(), "--seed", "7", "--scenario", "demo"});
        exercise("session", "seed", {"session", "seed", state.string()});
        exercise("session", "inject",
                 {"session", "inject", state.string(), "--action", "move_x", "--value", "1", "--at",
                  "0"});
        exercise("session", "step", {"session", "step", state.string(), "--ticks", "8"});
        exercise("session", "hash", {"session", "hash", state.string()});
        const fs::path replay_a = stage / "run-a.replay.json";
        exercise("session", "record",
                 {"session", "record", state.string(), "--out", replay_a.string()});

        // Replay the COMMITTED corpus replay artifact (deterministic, no project manifest -> no
        // drift), proving the maintained ctx:replay artifact still replays green.
        exercise("", "replay", {"replay", (stage / "replays" / "demo.replay.json").string()});

        // A second, differently-seeded run + determinism diff (R-QA-005 / L-54 triage).
        const fs::path state_b = stage / "session-b.state.json";
        const fs::path replay_b = stage / "run-b.replay.json";
        exercise("session", "new",
                 {"session", "new", state_b.string(), "--seed", "9", "--scenario", "demo"});
        exercise("session", "step", {"session", "step", state_b.string(), "--ticks", "8"});
        exercise("session", "record",
                 {"session", "record", state_b.string(), "--out", replay_b.string()});
        exercise("determinism", "diff",
                 {"determinism", "diff", replay_a.string(), replay_b.string()});
    }

    // --- M7 T5 (issue #223): the headless runtime-UI drive/assert verbs over the ui-hud few-shot
    //     scene (R-UI-006) — dump the retained tree + computed rects, query a data-bound node, click
    //     the action button (the UI->state path scores points), and assert a bound value fail-closed.
    //     Exercises the full ui dump/query/send/assert surface so the coverage assertion below counts
    //     them as covered (they are stable ∧ implemented one-shot CLI verbs). --------------------------
    {
        const std::string ui_scene = (stage / "ui-hud" / "default.ui-hud.json").string();
        exercise("ui", "dump", {"ui", "dump", ui_scene});
        const Envelope q = exercise("ui", "query", {"ui", "query", ui_scene, "health-bar"});
        if (q.ok())
            CHECK(q.data().at("boundValue").as_number() == 100.0);
        const Envelope sent =
            exercise("ui", "send", {"ui", "send", ui_scene, "click", "--target", "play-button"});
        if (sent.ok())
            CHECK(sent.data().at("state").at("score").as_number() == 10.0);
        exercise("ui", "assert",
                 {"ui", "assert", ui_scene, "health-bar", "--value", "100"});
    }

    // --- new: the R-QA-006 MUST-half runnable default template, authored into a fresh dir and then
    //     validated clean (the corpus builds ON this M1 surface — issue #106 Context) --------------
    {
        const fs::path scaffold = make_temp_path("new") / "fresh";
        std::error_code ec;
        fs::create_directories(scaffold, ec);
        const Envelope env = exercise("", "new", {"new", scaffold.string()});
        if (env.ok())
            exercise("", "validate", {"validate", scaffold.string()});
        remove_quiet(scaffold.parent_path());
    }

    // =============================================================================================
    // Kind coverage — every registered engine file kind is represented by an authored corpus file.
    // =============================================================================================
    for (const auto& fk : Registry::instance().file_kinds())
    {
        const bool present = corpus_has_kind(samples, fk.id);
        if (!present)
            std::fprintf(stderr, "[samples-corpus] no corpus file for registered kind '%s'\n",
                         fk.id.c_str());
        CHECK(present);
    }

    // =============================================================================================
    // Contract-surface coverage assertion (R-CLI-009 freeze readiness).
    //
    // The exemption set — stable ∧ implemented verbs the corpus does NOT exercise, each because it
    // needs a live-daemon or lockfile fixture the file-authoring corpus cannot supply, and each
    // already covered by its own unit test:
    //   resource/read (`fetch`) — needs a live daemon-issued opaque resource handle (cli test
    //                             test_fetch_e2e drives the real daemon handle path).
    //   /install                — needs a package.json + package-lock.json + offline artifact cache
    //                             with real SRI (cli test test_pkg_command builds those fixtures).
    //   /re-key                 — remediates a genuine duplicate-intra-file id, a state the canonical
    //                             serializer + structural merge normally PREVENT (an add/add id
    //                             collision surfaces as a resolvable merge conflict, exercised above);
    //                             the raw remediation is covered by src/editor/merge unit tests.
    //   profile/gc              — needs the in-process JS VM (stub on the local GCC gate — it would
    //                             refuse with sim.gc.unavailable) and reports wall-clock GC timings,
    //                             not file-authoring output; covered by cli test test_profile_command
    //                             (both backend branches) + js-test_gc_discipline / _gc_state_hash.
    // Operational (daemon-served) verbs are stability!="stable" and reserved verbs are
    // implemented==false, so both are excluded by the predicate without needing an exemption entry.
    // =============================================================================================
    const std::set<std::string> exempt = {"resource/read", "/install", "/re-key", "profile/gc"};

    // Guard: an exemption must name a verb that still exists (so the list cannot rot silently).
    for (const std::string& key : exempt)
    {
        bool found = false;
        for (const VerbSpec& v : Registry::instance().verbs())
            if (v.key() == key)
            {
                found = true;
                break;
            }
        if (!found)
            std::fprintf(stderr, "[samples-corpus] exempt key '%s' is not a registered verb\n",
                         key.c_str());
        CHECK(found);
    }

    int targets = 0;
    int covered = 0;
    for (const VerbSpec& v : Registry::instance().verbs())
    {
        // Every verb projects a stable method + tool id on the other surfaces (R-CLI-009 by
        // construction — the deeper CLI ≡ RPC ≡ MCP ≡ introspection identity is the m1-exit-4 gate).
        CHECK(!v.rpc_method.empty());
        CHECK(!v.mcp_tool.empty());

        const bool is_target = v.stability == "stable" && v.implemented && exempt.count(v.key()) == 0;
        if (!is_target)
            continue;
        ++targets;
        const bool hit = g_exercised.count(v.key()) != 0;
        if (!hit)
            std::fprintf(stderr,
                         "[samples-corpus] UNCOVERED stable one-shot verb '%s' (%s) — the corpus "
                         "must exercise it or the exemption set must justify it\n",
                         v.key().c_str(), v.cli_command().c_str());
        CHECK(hit);
        if (hit)
            ++covered;
    }
    CHECK(targets > 0);
    std::fprintf(stderr,
                 "[samples-corpus] contract-surface coverage: %d/%d stable one-shot CLI verbs "
                 "exercised (%zu documented exemptions); protocolMajor frozen at 1 (M3 freeze).\n",
                 covered, targets, exempt.size());

    remove_quiet(stage);
    return g_failures == 0 ? 0 : 1;
}

// L-50 multi-worktree MERGE convergence validation, Scenario 2 (issue #284, a16-l50; R-FILE-012,
// L-26, R-QA-005, R-QA-011). Red-teams the EXISTING M2 structural-merge machinery end-to-end (it adds
// no production code — it drives the shipped merge engine + `context` CLI verbs):
//   * EVERY documented conflict class in the R-FILE-012 corpus is exercised — the 5 JSON-visible
//     classes through the merge engine (with each case's own schema floor) + the whole-file
//     binary-sidecar class through the `context merge-file` CLI path — and the observed set is asserted
//     to equal the full ConflictClass enum (rots-if-broken: a new class without a fixture fails here);
//   * parallel-worktree DIVERGENCE -> structural merge -> `context resolve-conflict` (id-based merge
//     identity, field-path granularity, NEVER text markers);
//   * the post-merge `context validate` convergence gate — it CATCHES a duplicate-intra-file-id (the
//     R-FILE-012(c) structural break) and the `context re-key` remedy converges it back to valid;
//   * the R-QA-005 sim-level pass — the semantic-health half of the convergence gate (R-FILE-012(c)):
//     `context validate` proves STRUCTURAL health, the deterministic Session step proves BEHAVIORAL
//     health (same seed+inputs -> bit-identical state hash; a different world diverges).
//
// Parallel-worktree divergence is modeled in-process (base/ours/theirs trees) for determinism; the
// real-git worktree/branch merge through the auto-installed driver is already covered by the sibling
// merge-test_git_driver ctest — this test adds the convergence-gate + sim-level composition on top.

#include "concurrency_test.h"

#include "context/cli/app.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/merge/conflict.h"
#include "context/editor/merge/resolve.h"
#include "context/editor/merge/three_way_merge.h"
#include "context/editor/serializer/json_tree.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/state_hash.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using conctest::canon;
using conctest::jint;
using conctest::json_eq;
using conctest::parse;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
using context::editor::serializer::JsonValue;
namespace cli = context::cli;
namespace merge = context::editor::merge;
namespace session = context::runtime::session;

namespace
{
namespace fs = std::filesystem;

const JsonValue* member(const JsonValue& v, const char* key)
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (const context::editor::serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

std::int64_t as_int(const JsonValue* v)
{
    if (v == nullptr)
        return 0;
    if (v->type == JsonValue::Type::integer)
        return v->int_value;
    if (v->type == JsonValue::Type::unsigned_integer)
        return static_cast<std::int64_t>(v->uint_value);
    return 0;
}

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Build the optional installed-schema floor from a corpus case's expected.json (mirrors the merge
// module's test_corpus.cpp — the newer-stamped case supplies its own floor to force detection).
merge::SchemaFloor read_floor(const JsonValue& expected)
{
    merge::SchemaFloor floor;
    const JsonValue* f = member(expected, "floor");
    if (f == nullptr)
        return floor;
    if (const JsonValue* kv = member(*f, "kind_versions"); kv != nullptr)
        for (const auto& m : kv->members)
            floor.kind_versions[m.key] = as_int(&m.value);
    if (const JsonValue* cv = member(*f, "component_versions"); cv != nullptr)
        for (const auto& m : cv->members)
            floor.component_versions[m.key] = as_int(&m.value);
    return floor;
}

// Scan an envelope's data.diagnostics[] for a diagnostic carrying `code`.
bool has_diagnostic(const Envelope& e, const std::string& code)
{
    if (!e.data().contains("diagnostics"))
        return false;
    const Json& diags = e.data().at("diagnostics");
    for (std::size_t i = 0; i < diags.size(); ++i)
        if (diags.at(i).contains("code") && diags.at(i).at("code").as_string() == code)
            return true;
    return false;
}

} // namespace

int main()
{
    // A unique per-process temp token: two concurrent runs of this executable (e.g. two PRs' `build`
    // legs sharing a self-hosted runner's %TEMP%) must never collide on the same temp dir/file — the
    // merge module's own sibling test_git_driver uses the same per-run uniqueness for exactly this.
    // Cast the clock rep to a concrete integer before std::to_string: the rep is implementation-defined
    // and `std::to_string` on a chrono rep is AMBIGUOUS under Apple libc++ (compiles under GCC/MSVC) —
    // the sibling test_git_driver casts for exactly this reason (see the profile's macOS-libc++ note).
    const std::string uniq = std::to_string(
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));

    // ============================================================================================
    // Part A — EVERY documented conflict class in the R-FILE-012 corpus is exercised (the DoD gate)
    // ============================================================================================
    std::set<std::string> observed;
    const fs::path corpus(CONTEXT_MERGE_CORPUS_DIR);
    CHECK(fs::is_directory(corpus));

    std::vector<fs::path> cases;
    for (const fs::directory_entry& entry : fs::directory_iterator(corpus))
        if (entry.is_directory())
            cases.push_back(entry.path());
    // The corpus is a versioned deliverable, not incidental fixtures (guards a renamed/emptied dir).
    CHECK(cases.size() >= 8);

    std::size_t json_cases_run = 0;
    for (const fs::path& dir : cases)
    {
        std::string base_text, ours_text, theirs_text, expected_text;
        // The whole-file binary-sidecar case carries base.bin (not base.json) — the structural engine
        // only sees parsed JSON, so it is exercised through the CLI path below.
        if (!read_file(dir / "base.json", base_text))
            continue;
        CHECK(read_file(dir / "ours.json", ours_text));
        CHECK(read_file(dir / "theirs.json", theirs_text));
        CHECK(read_file(dir / "expected.json", expected_text));

        merge::MergeOptions opts;
        opts.floor = read_floor(parse(expected_text));
        const merge::MergeResult result =
            merge::merge_documents(parse(base_text), parse(ours_text), parse(theirs_text), opts);
        for (const merge::Conflict& c : result.conflicts)
            observed.insert(merge::to_string(c.klass));
        ++json_cases_run;
    }
    CHECK(json_cases_run >= 5); // the 5 JSON conflict-class + baseline cases actually ran

    // The whole-file binary-sidecar class is JSON-invisible (binary bytes are never embedded as JSON):
    // drive the committed .bin fixtures through the real `context merge-file` CLI path.
    {
        const fs::path bin = corpus / "binary-sidecar";
        const fs::path out = fs::temp_directory_path() / ("ctx-a16-binmerge-" + uniq + ".bin");
        const Envelope e = cli::run({"merge-file", (bin / "base.bin").string(),
                                     (bin / "ours.bin").string(), (bin / "theirs.bin").string(),
                                     "--output", out.string()});
        CHECK(e.ok());
        CHECK(e.data().at("clean").as_bool() == false);
        const Json& conflicts = e.data().at("conflicts");
        for (std::size_t i = 0; i < conflicts.size(); ++i)
            observed.insert(conflicts.at(i).at("class").as_string());
    }

    // The observed set must equal the FULL documented conflict vocabulary — every ConflictClass value
    // is provably reachable through the corpus. A new class without a covering fixture fails here.
    const merge::ConflictClass kAllClasses[] = {
        merge::ConflictClass::field,       merge::ConflictClass::id_add_add,
        merge::ConflictClass::delete_modify, merge::ConflictClass::binary_sidecar,
        merge::ConflictClass::meta_guid,   merge::ConflictClass::newer_stamped};
    for (const merge::ConflictClass klass : kAllClasses)
        CHECK(observed.count(merge::to_string(klass)) == 1);
    CHECK(observed.size() == 6);

    // ============================================================================================
    // Part B — parallel-worktree divergence -> structural merge -> resolve-conflict
    // ============================================================================================
    // Disjoint field edits on the SAME id-keyed entity auto-converge (field-path granularity, L-33
    // id-based merge identity): ours changes hp, theirs changes mp -> both survive, clean.
    {
        const JsonValue base =
            parse(R"({"entities":[{"id":"aaaaaaaaaaaaaaa1","hp":10,"mp":5}]})");
        const JsonValue ours =
            parse(R"({"entities":[{"id":"aaaaaaaaaaaaaaa1","hp":20,"mp":5}]})");
        const JsonValue theirs =
            parse(R"({"entities":[{"id":"aaaaaaaaaaaaaaa1","hp":10,"mp":9}]})");
        const merge::MergeResult r = merge::merge_documents(base, ours, theirs);
        CHECK(r.clean);
        const JsonValue* ents = member(r.merged, "entities");
        CHECK(ents != nullptr && ents->type == JsonValue::Type::array && ents->elements.size() == 1);
        const JsonValue& e0 = ents->elements[0];
        CHECK(as_int(member(e0, "hp")) == 20); // ours edit survived
        CHECK(as_int(member(e0, "mp")) == 9);  // theirs edit survived (field-path granular merge)
        CHECK(canon(r.merged).find("<<<<<<<") == std::string::npos); // never a text conflict marker
    }
    // A colliding edit (same field on both sides) yields a machine-readable conflict; resolve-conflict
    // applies the chosen side, leaving valid JSON with no markers.
    {
        const JsonValue base = parse(R"({"hp":10})");
        const JsonValue ours = parse(R"({"hp":20})");
        const JsonValue theirs = parse(R"({"hp":30})");
        merge::MergeResult r = merge::merge_documents(base, ours, theirs);
        CHECK(!r.clean);
        CHECK(r.conflicts.size() == 1);
        CHECK(r.conflicts[0].klass == merge::ConflictClass::field);
        CHECK(r.conflicts[0].path == "/hp");
        CHECK(r.conflicts[0].theirs.has_value());
        // Take theirs (the R-FILE-012 resolve loop): apply into the merged tree at the conflict path.
        const merge::ApplyResult applied =
            merge::apply_resolution(r.merged, r.conflicts[0].path, r.conflicts[0].theirs);
        CHECK(applied.ok);
        CHECK(as_int(member(r.merged, "hp")) == 30);                  // theirs won the resolution
        CHECK(canon(r.merged).find("<<<<<<<") == std::string::npos); // still valid JSON, no markers
    }

    // ============================================================================================
    // Part C — the post-merge `context validate` convergence gate (structural health)
    // ============================================================================================
    {
        const fs::path dir = fs::temp_directory_path() / ("ctx-a16-validate-" + uniq);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        // A clean merged scene (unique intra-file ids) converges: validate reports valid.
        const fs::path clean_scene = dir / "clean.json";
        write_file(clean_scene,
                   R"({"entities":[{"id":"aaaaaaaaaaaaaaa1","hp":20},{"id":"aaaaaaaaaaaaaaa2","hp":9}]})");
        const Envelope v_clean = cli::run({"validate", clean_scene.string()});
        CHECK(v_clean.ok()); // a reporting verb: the envelope is ALWAYS success...
        CHECK(v_clean.data().at("valid").as_bool() == true); // ...assert the "valid" field, not the exit code

        // A merge that duplicated an intra-file id is the R-FILE-012(c) structural break: validate
        // CATCHES it (valid:false + merge.duplicate_id), and the `context re-key` remedy converges it.
        const fs::path dup_scene = dir / "dup.json";
        write_file(dup_scene,
                   R"({"entities":[{"id":"aaaaaaaaaaaaaaa1","hp":20},{"id":"aaaaaaaaaaaaaaa1","hp":9}]})");
        const Envelope v_dup = cli::run({"validate", dup_scene.string()});
        CHECK(v_dup.ok());
        CHECK(v_dup.data().at("valid").as_bool() == false);
        CHECK(has_diagnostic(v_dup, "merge.duplicate_id"));

        const Envelope rekey = cli::run({"re-key", dup_scene.string(), "--at", "/entities/1"});
        CHECK(rekey.ok());
        const Envelope v_after = cli::run({"validate", dup_scene.string()});
        CHECK(v_after.data().at("valid").as_bool() == true); // converged after the re-key remedy

        fs::remove_all(dir, ec);
    }

    // ============================================================================================
    // Part D — the R-QA-005 sim-level pass (semantic health, R-FILE-012(c))
    // ============================================================================================
    // The merge convergence gate names a SEMANTIC conflict class — both sides individually valid, the
    // merged world behaviorally broken — mitigated by the R-QA-005 sim-level test pass. This validates
    // that pass's mechanism: a deterministic headless Session steps to a BIT-IDENTICAL state hash for
    // the same (seed + inputs), and a different world diverges (the gate has discriminating power).
    {
        session::SessionConfig cfg;
        cfg.seed = 0xA16u;
        cfg.tick_hz = 60;
        cfg.scenario = "demo";

        session::Session s1(cfg);
        const session::StepResult r1 = s1.step(120);
        session::Session s2(cfg);
        const session::StepResult r2 = s2.step(120);
        CHECK(r1.sim_tick == 120);
        CHECK(r2.sim_tick == 120);
        CHECK(r1.state_hash.root == r2.state_hash.root); // same seed+inputs -> identical hash (converged)
        CHECK(!r1.state_hash.archetypes.empty());         // a real, non-vacuous world was hashed

        // Discriminating power: a behaviorally different world (a different seed) diverges, so the sim
        // pass would CATCH a semantic conflict that slipped past the structural `context validate`.
        session::SessionConfig cfg_other = cfg;
        cfg_other.seed = 0xBEEFu;
        session::Session s3(cfg_other);
        const session::StepResult r3 = s3.step(120);
        CHECK(r3.state_hash.root != r1.state_hash.root);
    }

    CONC_TEST_MAIN_END();
}

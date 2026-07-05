// The `context` merge family through the CLI (R-FILE-012 / R-QA-013): merge-file (clean, conflict,
// driver-mode exit, binary), resolve-conflict, re-key, validate — driven end to end through run(),
// asserting the R-CLI-008 envelope + the on-disk result.

#include "context/cli/app.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "cli_test.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace context::cli;
using context::editor::contract::Envelope;
using context::editor::contract::Json;

namespace
{
namespace fs = std::filesystem;

fs::path g_dir;

fs::path p(const std::string& name) { return g_dir / name; }

void write(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Json read_json(const fs::path& path) { return Json::parse(read(path)); }

void test_merge_clean_disjoint()
{
    write(p("b1.json"), R"({"a": 1, "b": 2})");
    write(p("o1.json"), R"({"a": 10, "b": 2})");
    write(p("t1.json"), R"({"a": 1, "b": 20})");
    const std::string out = p("m1.json").string();
    const Envelope e = run({"merge-file", p("b1.json").string(), p("o1.json").string(),
                            p("t1.json").string(), "--output", out});
    CHECK(e.ok());
    CHECK(e.exit_code() == 0);
    CHECK(e.data().at("clean").as_bool() == true);
    const Json merged = read_json(out);
    CHECK(merged.at("a").as_int() == 10);
    CHECK(merged.at("b").as_int() == 20);
}

void test_merge_conflict_envelope()
{
    write(p("b2.json"), R"({"hp": 10})");
    write(p("o2.json"), R"({"hp": 11})");
    write(p("t2.json"), R"({"hp": 12})");
    const std::string out = p("m2.json").string();
    const Envelope e = run({"merge-file", p("b2.json").string(), p("o2.json").string(),
                            p("t2.json").string(), "--output", out});
    // CLI mode: the merge OPERATION succeeded and reports conflicts in data (R-CLI-008 envelope).
    CHECK(e.ok());
    CHECK(e.data().at("clean").as_bool() == false);
    const Json& conflicts = e.data().at("conflicts");
    CHECK(conflicts.size() == 1);
    CHECK(conflicts.at(0).at("path").as_string() == "/hp");
    CHECK(conflicts.at(0).at("class").as_string() == "field");
    // the machine-readable base/ours/theirs sides are present.
    CHECK(conflicts.at(0).at("ours").as_int() == 11);
    CHECK(conflicts.at(0).at("theirs").as_int() == 12);
    // a valid-JSON merged file with the deterministic ours placeholder (never a text marker).
    CHECK(read(out).find("<<<<<<<") == std::string::npos);
    CHECK(read_json(out).at("hp").as_int() == 11);
    // the conflict sidecar was written for the resolve loop.
    CHECK(fs::exists(out + ".ctxconflicts.json"));
}

void test_merge_conflict_no_ancestor_omits_base()
{
    // Both sides created this file with NO common ancestor and INCOMPATIBLE root shapes (two plain,
    // non-id-keyed arrays), so the top-level field conflict has no `base`. It must be OMITTED — an
    // absent ancestor is never emitted as {} (the R-CLI-008 "absent side is omitted" contract). This
    // guards the general field-conflict path against the same base-leak the whole-file path already fixes.
    write(p("nb.json"), "");                 // empty => no ancestor (add/add)
    write(p("no.json"), R"([1, 2])");
    write(p("nt.json"), R"([3, 4])");
    const std::string out = p("nm.json").string();
    const Envelope e = run({"merge-file", p("nb.json").string(), p("no.json").string(),
                            p("nt.json").string(), "--output", out});
    CHECK(e.ok());
    CHECK(e.data().at("clean").as_bool() == false);
    const Json& conflicts = e.data().at("conflicts");
    CHECK(conflicts.size() == 1);
    CHECK(conflicts.at(0).at("path").as_string() == "");
    CHECK(conflicts.at(0).at("class").as_string() == "field");
    CHECK(!conflicts.at(0).contains("base")); // absent ancestor is OMITTED, never {}
    CHECK(conflicts.at(0).contains("ours"));
    CHECK(conflicts.at(0).contains("theirs"));
}

void test_merge_driver_mode_exit()
{
    write(p("b3.json"), R"({"hp": 10})");
    write(p("o3.json"), R"({"hp": 11})");
    write(p("t3.json"), R"({"hp": 12})");
    const std::string out = p("m3.json").string();
    const Envelope e = run({"merge-file", p("b3.json").string(), p("o3.json").string(),
                            p("t3.json").string(), "--output", out, "--driver"});
    // driver mode: a non-zero exit tells git the conflict remains.
    CHECK(!e.ok());
    CHECK(e.exit_code() == 4); // conflict class
    CHECK(e.error()->code == "merge.conflict");
    CHECK(fs::exists(out + ".ctxconflicts.json"));
}

void test_merge_binary_sidecar()
{
    // R-QA-011: exercise the COMMITTED binary-sidecar corpus fixtures (the source of truth), not
    // inline literals — a corrupted/edited .bin must fail here. The engine-level corpus test skips
    // non-JSON cases (binary whole-file merge lives in the CLI layer), so this is the fixtures' gate.
    const fs::path corpus = fs::path(CONTEXT_MERGE_CORPUS_DIR) / "binary-sidecar";
    const std::string out = p("m4.bin").string();
    const Envelope e = run({"merge-file", (corpus / "base.bin").string(), (corpus / "ours.bin").string(),
                            (corpus / "theirs.bin").string(), "--output", out});
    CHECK(e.ok());
    CHECK(e.data().at("clean").as_bool() == false);
    CHECK(e.data().at("conflicts").at(0).at("class").as_string() == "binary_sidecar");
    // whole-file default is OURS: the merged bytes are byte-identical to the committed ours.bin.
    CHECK(read(out) == read(corpus / "ours.bin"));
    CHECK(read(out) != read(corpus / "theirs.bin"));
}

void test_resolve_conflict_take_theirs()
{
    // Reuse the conflict from test_merge_conflict_envelope's output + sidecar.
    const std::string out = p("m2.json").string();
    const Envelope e = run({"resolve-conflict", out, "--path", "/hp", "--take", "theirs"});
    CHECK(e.ok());
    CHECK(read_json(out).at("hp").as_int() == 12); // theirs's value applied
    CHECK(e.data().at("remainingConflicts").as_int() == 0);
    CHECK(!fs::exists(out + ".ctxconflicts.json")); // sidecar cleared when empty
}

void test_resolve_conflict_value()
{
    write(p("r.json"), R"({"a": 1})");
    const Envelope e = run({"resolve-conflict", p("r.json").string(), "--path", "/a", "--value", "42"});
    CHECK(e.ok());
    CHECK(read_json(p("r.json")).at("a").as_int() == 42);
}

void test_resolve_conflict_take_and_value_mutually_exclusive()
{
    // --take and --value are two different resolution sources; supplying BOTH is ambiguous input and
    // must fail loudly (usage.invalid) rather than silently prefer one and drop the other unnoticed.
    // Validation precedes any read/write, so the target file is left untouched.
    write(p("me.json"), R"({"a": 1})");
    const Envelope e = run({"resolve-conflict", p("me.json").string(), "--path", "/a",
                            "--take", "ours", "--value", "42"});
    CHECK(!e.ok());
    CHECK(e.error()->code == "usage.invalid");
    CHECK(read_json(p("me.json")).at("a").as_int() == 1); // rejected before any write
}

void test_resolve_conflict_whole_file_take_theirs()
{
    // A meta_guid whole-file conflict (both sides re-guid) defaults the merged file to OURS; the
    // whole-document sentinel path "" must be resolvable to theirs via `resolve-conflict --path ""`.
    write(p("wb.json"), R"({"guid": "0000000000000000", "importer": "png"})");
    write(p("wo.json"), R"({"guid": "aaaa000000000000", "importer": "png"})");
    write(p("wt.json"), R"({"guid": "bbbb000000000000", "importer": "png"})");
    const std::string out = p("wm.json").string();
    const Envelope m = run({"merge-file", p("wb.json").string(), p("wo.json").string(),
                            p("wt.json").string(), "--output", out});
    CHECK(m.ok());
    CHECK(m.data().at("clean").as_bool() == false);
    CHECK(m.data().at("wholeFile").as_bool() == true);
    CHECK(m.data().at("conflicts").at(0).at("path").as_string() == "");
    CHECK(m.data().at("conflicts").at(0).at("class").as_string() == "meta_guid");
    CHECK(read_json(out).at("guid").as_string() == "aaaa000000000000"); // merged defaults to ours

    const Envelope r = run({"resolve-conflict", out, "--path", "", "--take", "theirs"});
    CHECK(r.ok());
    CHECK(read_json(out).at("guid").as_string() == "bbbb000000000000"); // whole document -> theirs
    CHECK(r.data().at("remainingConflicts").as_int() == 0);
    CHECK(!fs::exists(out + ".ctxconflicts.json"));
}

void test_rekey()
{
    write(p("dup.json"),
          R"({"entities": [{"id": "aaaa0000aaaa0001"}, {"id": "aaaa0000aaaa0001"}]})");
    const Envelope e = run({"re-key", p("dup.json").string(), "--id", "aaaa0000aaaa0001"});
    CHECK(e.ok());
    CHECK(e.data().at("oldId").as_string() == "aaaa0000aaaa0001");
    CHECK(e.data().at("newId").as_string().size() == 16);
    // after re-key the file has no duplicate ids.
    const Envelope v = run({"validate", p("dup.json").string()});
    CHECK(v.ok());
    CHECK(v.data().at("valid").as_bool() == true);
}

void test_rekey_at_pathological_index_no_crash()
{
    // A pathological over-long array index must yield a clean failure envelope, never an uncaught
    // std::out_of_range from std::stoull (the pointer index parse caps the token length).
    write(p("bigidx.json"), R"({"entities": [{"id": "aaaa0000aaaa0001"}]})");
    const Envelope e = run({"re-key", p("bigidx.json").string(),
                            "--at", "/entities/999999999999999999999999"});
    CHECK(!e.ok());
    CHECK(e.error()->code == "merge.rekey_target_invalid");
}

void test_rekey_at_and_id_mutually_exclusive()
{
    // --at (a pointer) and --id (a duplicated id) are two different targeting modes; supplying BOTH
    // must fail loudly rather than silently prefer --at and ignore --id.
    write(p("mek.json"), R"({"entities": [{"id": "aaaa0000aaaa0001"}]})");
    const Envelope e = run({"re-key", p("mek.json").string(),
                            "--at", "/entities/0", "--id", "aaaa0000aaaa0001"});
    CHECK(!e.ok());
    CHECK(e.error()->code == "usage.invalid");
}

void test_validate()
{
    write(p("clean.json"), R"({"entities": [{"id": "aaaa0000aaaa0001"}, {"id": "bbbb0000bbbb0002"}]})");
    const Envelope clean = run({"validate", p("clean.json").string()});
    CHECK(clean.ok());
    CHECK(clean.data().at("valid").as_bool() == true);
    CHECK(clean.data().at("duplicateIds").as_int() == 0);

    write(p("bad.json"), R"({"entities": [{"id": "cccc0000cccc0003"}, {"id": "cccc0000cccc0003"}]})");
    const Envelope bad = run({"validate", p("bad.json").string()});
    CHECK(bad.ok()); // the gate RAN; the result is in data.valid
    CHECK(bad.data().at("valid").as_bool() == false);
    CHECK(bad.data().at("duplicateIds").as_int() >= 1);
    CHECK(bad.data().at("diagnostics").at(0).at("code").as_string() == "merge.duplicate_id");
}

void test_merge_file_dry_run_writes_nothing()
{
    // --dry-run is a core flag ("honored by EVERY verb"): COMPUTE + report the conflict, write NOTHING
    // (no merged output, no sidecar) — mirroring `context migrate --dry-run`.
    write(p("db.json"), R"({"hp": 10})");
    write(p("do.json"), R"({"hp": 11})");
    write(p("dt.json"), R"({"hp": 12})");
    const std::string out = p("dm.json").string();
    const Envelope e = run({"merge-file", p("db.json").string(), p("do.json").string(),
                            p("dt.json").string(), "--output", out, "--dry-run"});
    CHECK(e.ok());
    CHECK(e.data().at("dryRun").as_bool() == true);
    CHECK(e.data().at("clean").as_bool() == false);      // the conflict was still computed + reported
    CHECK(e.data().at("conflicts").size() == 1);
    CHECK(!fs::exists(out));                              // ...but nothing was written to disk
    CHECK(!fs::exists(out + ".ctxconflicts.json"));
}

void test_resolve_conflict_dry_run_writes_nothing()
{
    write(p("rcb.json"), R"({"hp": 10})");
    write(p("rco.json"), R"({"hp": 11})");
    write(p("rct.json"), R"({"hp": 12})");
    const std::string out = p("rcm.json").string();
    const Envelope setup = run({"merge-file", p("rcb.json").string(), p("rco.json").string(),
                                p("rct.json").string(), "--output", out});
    CHECK(setup.ok() && setup.data().at("clean").as_bool() == false);
    const std::string merged_before = read(out);
    const std::string sidecar_before = read(out + ".ctxconflicts.json");
    const Envelope e = run({"resolve-conflict", out, "--path", "/hp", "--take", "theirs", "--dry-run"});
    CHECK(e.ok());
    CHECK(e.data().at("dryRun").as_bool() == true);
    CHECK(e.data().at("remainingConflicts").as_int() == 0);      // computed
    CHECK(read(out) == merged_before);                           // file untouched
    CHECK(read(out + ".ctxconflicts.json") == sidecar_before);   // sidecar untouched
}

void test_rekey_dry_run_writes_nothing()
{
    write(p("drk.json"), R"({"entities": [{"id": "aaaa0000aaaa0001"}, {"id": "aaaa0000aaaa0001"}]})");
    const std::string before = read(p("drk.json"));
    const Envelope e = run({"re-key", p("drk.json").string(), "--id", "aaaa0000aaaa0001", "--dry-run"});
    CHECK(e.ok());
    CHECK(e.data().at("dryRun").as_bool() == true);
    CHECK(e.data().at("newId").as_string().size() == 16); // the fresh id was still minted (reported)
    CHECK(read(p("drk.json")) == before);                 // ...but the file is untouched
}

void test_resolve_conflict_reindexes_after_array_shrink()
{
    // Two delete_modify conflicts in ONE id-keyed array. Resolving the first by taking the deleting
    // side shrinks the array, shifting the second element down. The surviving sidecar entry must be
    // reindexed (/items/1 -> /items/0) so a second resolve still targets the intended element — not a
    // silently mis-targeted neighbour (the pre-fix corruption).
    write(p("xb.json"),
          R"({"items": [{"id": "e1", "v": 1}, {"id": "e2", "v": 2}, {"id": "e3", "v": 3}]})");
    write(p("xo.json"),
          R"({"items": [{"id": "e1", "v": 11}, {"id": "e2", "v": 22}, {"id": "e3", "v": 3}]})");
    write(p("xt.json"), R"({"items": [{"id": "e3", "v": 3}]})"); // theirs deleted e1 and e2
    const std::string out = p("xm.json").string();
    const Envelope m = run({"merge-file", p("xb.json").string(), p("xo.json").string(),
                            p("xt.json").string(), "--output", out});
    CHECK(m.ok());
    CHECK(m.data().at("conflicts").size() == 2);

    // Resolve the first conflict by taking theirs (a delete) — removes element 0, shrinking the array.
    const Envelope r1 = run({"resolve-conflict", out, "--path", "/items/0", "--take", "theirs"});
    CHECK(r1.ok());
    CHECK(r1.data().at("remainingConflicts").as_int() == 1);

    // The survivor was reindexed to /items/0; a second take-theirs deletes e2, leaving ONLY e3.
    const Envelope r2 = run({"resolve-conflict", out, "--path", "/items/0", "--take", "theirs"});
    CHECK(r2.ok());
    CHECK(r2.data().at("remainingConflicts").as_int() == 0);
    const Json merged = read_json(out);
    CHECK(merged.at("items").size() == 1);
    CHECK(merged.at("items").at(0).at("id").as_string() == "e3"); // e3 survived; e1,e2 deleted
    CHECK(!fs::exists(out + ".ctxconflicts.json"));
}

} // namespace

int main()
{
    std::error_code ec;
    g_dir = fs::temp_directory_path() / "ctx-merge-cli-test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);

    test_merge_clean_disjoint();
    test_merge_conflict_envelope();
    test_merge_conflict_no_ancestor_omits_base();
    test_merge_driver_mode_exit();
    test_merge_binary_sidecar();
    test_resolve_conflict_take_theirs();
    test_resolve_conflict_value();
    test_resolve_conflict_take_and_value_mutually_exclusive();
    test_resolve_conflict_whole_file_take_theirs();
    test_rekey();
    test_rekey_at_pathological_index_no_crash();
    test_rekey_at_and_id_mutually_exclusive();
    test_validate();
    test_merge_file_dry_run_writes_nothing();
    test_resolve_conflict_dry_run_writes_nothing();
    test_rekey_dry_run_writes_nothing();
    test_resolve_conflict_reindexes_after_array_shrink();

    fs::remove_all(g_dir, ec);
    CLI_TEST_MAIN_END();
}

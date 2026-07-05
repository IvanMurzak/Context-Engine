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

} // namespace

int main()
{
    std::error_code ec;
    g_dir = fs::temp_directory_path() / "ctx-merge-cli-test";
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);

    test_merge_clean_disjoint();
    test_merge_conflict_envelope();
    test_merge_driver_mode_exit();
    test_merge_binary_sidecar();
    test_resolve_conflict_take_theirs();
    test_resolve_conflict_value();
    test_resolve_conflict_whole_file_take_theirs();
    test_rekey();
    test_rekey_at_pathological_index_no_crash();
    test_validate();

    fs::remove_all(g_dir, ec);
    CLI_TEST_MAIN_END();
}

// `context bench` subject tests (R-QA-013: happy path, edge cases, failure paths) — drives
// run_bench() in-process against a tiny generated-corpus-shaped fixture and asserts the harness
// subject contract (one JSON object; per-stage attach timings; honest unsupported scenarios).

#include "context/cli/bench_command.h"
#include "context/editor/contract/json.h"

#include "cli_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using context::cli::run_bench;
using context::editor::contract::Json;

namespace
{

void write_file(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

// A miniature gen_corpus.py-shaped corpus: scenes (with the root SceneSettings' `"timeScale": 1`
// the edit/bulk mutation keys on), meta sidecars, and binary assets under `project/`.
int populate_corpus(const fs::path& corpus)
{
    int files = 0;
    for (int i = 0; i < 6; ++i)
    {
        const std::string idx = std::to_string(i);
        write_file(corpus / ("project/scenes/00/scene-000" + idx + ".scene.json"),
                   "{\n  \"$schema\": \"test\",\n  \"root\": {\n    \"components\": {\n"
                   "      \"ctx:SceneSettings\": {\n        \"timeScale\": 1\n      }\n    }\n"
                   "  },\n  \"entities\": [\n    {\n      \"id\": \"e" + idx + "\"\n    }\n  ]\n}\n");
        ++files;
        write_file(corpus / ("project/scenes/00/scene-000" + idx + ".scene.json.meta.json"),
                   "{\n  \"guid\": \"0000-000" + idx + "\",\n  \"version\": 1\n}\n");
        ++files;
    }
    for (int i = 0; i < 2; ++i)
    {
        const std::string idx = std::to_string(i);
        write_file(corpus / ("project/assets/00/asset-000" + idx + ".mesh.bin"),
                   std::string(1024, static_cast<char>('a' + i)));
        ++files;
        write_file(corpus / ("project/assets/00/asset-000" + idx + ".mesh.bin.meta.json"),
                   "{\n  \"guid\": \"1111-000" + idx + "\",\n  \"version\": 1\n}\n");
        ++files;
    }
    return files;
}

Json run_json(const std::vector<std::string>& args, int expected_rc)
{
    std::string out;
    const int rc = run_bench(args, out);
    CHECK(rc == expected_rc);
    return Json::parse(out);
}

double num(const Json& doc, const std::string& key)
{
    CHECK(doc.contains(key));
    return doc.at(key).as_number();
}

} // namespace

int main()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path corpus = fs::temp_directory_path() / ("context-bench-test-" +
                                                         std::to_string(stamp));
    const int total_files = populate_corpus(corpus);
    const std::string corpus_str = corpus.string();

    // --- failure paths ---------------------------------------------------------------------
    {
        std::string out;
        CHECK(run_bench({}, out) == 2); // no scenario
        CHECK(Json::parse(out).contains("error"));

        CHECK(run_bench({"attach"}, out) == 2); // missing --corpus
        CHECK(Json::parse(out).contains("error"));

        CHECK(run_bench({"attach", "--corpus", (corpus / "no-such-dir").string()}, out) == 2);
        CHECK(run_bench({"frobnicate", "--corpus", corpus_str}, out) == 2); // unknown scenario
        CHECK(run_bench({"attach", "--corpus", corpus_str, "--mode", "tepid"}, out) == 2);

        // Edge: a warm attach before any index exists must refuse, not silently go cold.
        CHECK(run_bench({"attach", "--corpus", corpus_str, "--mode", "warm"}, out) == 2);
        CHECK(Json::parse(out).contains("error"));
    }

    // --- honest unsupported scenarios ------------------------------------------------------
    {
        const Json imp = run_json({"import", "--corpus", corpus_str}, 0);
        CHECK(imp.contains("unsupported") && imp.at("unsupported").as_bool());
        const Json mrg = run_json({"merge", "--corpus", corpus_str}, 0);
        CHECK(mrg.contains("unsupported") && mrg.at("unsupported").as_bool());
    }

    // --- attach: fresh (parse+canonicalize+hash bound, per-stage split) ----------------------
    {
        const Json doc = run_json({"attach", "--corpus", corpus_str, "--threads", "4"}, 0);
        CHECK(doc.at("mode").as_string() == "fresh");
        CHECK(static_cast<int>(num(doc, "files_changed")) == total_files);
        CHECK(static_cast<int>(num(doc, "files_applied")) == total_files);
        CHECK(static_cast<int>(num(doc, "world_entities")) == total_files);
        CHECK(num(doc, "wall_seconds") > 0.0);
        CHECK(num(doc, "generation") > 0.0);
        CHECK(doc.contains("stages"));
        const Json& stages = doc.at("stages");
        CHECK(stages.contains("watch_seconds"));
        CHECK(stages.contains("hash_seconds"));
        CHECK(stages.contains("parse_seconds"));
        CHECK(stages.at("validate_seconds").is_null()); // M2 stage: explicit pending, never fake
        CHECK(stages.at("compose_seconds").is_null());
        CHECK(stages.contains("instantiate_seconds"));
        CHECK(stages.contains("fanout_seconds"));
        CHECK(doc.at("index_saved").as_bool());
        CHECK(static_cast<int>(num(doc, "threads_requested")) == 4);
        CHECK(static_cast<int>(num(doc, "threads_effective")) == 1); // honest: M1 crawl is serial
    }

    // --- attach: warm (mtime/size-gated scan; pristine corpus -> 0 changes) ------------------
    {
        const Json doc = run_json({"attach", "--corpus", corpus_str, "--mode", "warm"}, 0);
        CHECK(doc.at("mode").as_string() == "warm");
        CHECK(static_cast<int>(num(doc, "index_entries_loaded")) == total_files);
        CHECK(static_cast<int>(num(doc, "files_changed")) == 0);
        CHECK(num(doc, "wall_seconds") > 0.0);
    }

    // --- edit: single-file incremental latency, both write paths ----------------------------
    {
        const Json doc = run_json({"edit", "--corpus", corpus_str, "--seed", "7"}, 0);
        CHECK(num(doc, "latency_ms") > 0.0);
        CHECK(num(doc, "edit_cli_verb_ms") > 0.0);
        CHECK(doc.at("external_reflected").as_bool());
        CHECK(doc.at("cli_verb_reflected").as_bool());
        CHECK(static_cast<int>(num(doc, "changes_detected")) >= 1);
    }

    // --- bulk: burst convergence -------------------------------------------------------------
    {
        const Json doc = run_json({"bulk", "--corpus", corpus_str, "--count", "3", "--seed", "3"},
                                  0);
        CHECK(num(doc, "wall_seconds") > 0.0);
        CHECK(static_cast<int>(num(doc, "files_mutated")) >= 1);
        CHECK(static_cast<int>(num(doc, "changes_detected")) >= 1);
        CHECK(num(doc, "generation") > 0.0);
    }

    // --- query: the R-BRIDGE-008 session-query budget measurement ----------------------------
    {
        const Json doc = run_json({"query", "--corpus", corpus_str, "--samples", "40"}, 0);
        CHECK(static_cast<int>(num(doc, "samples")) == 40);
        CHECK(static_cast<int>(num(doc, "present_hits")) == 40); // populated world, scene paths
        CHECK(num(doc, "p99_ms") >= num(doc, "p50_ms"));
        CHECK(num(doc, "max_ms") >= num(doc, "p99_ms"));
        CHECK(static_cast<int>(num(doc, "world_entities")) >= total_files);
    }

    // --- sustained: R-FILE-013 backpressure under deliberate overload ------------------------
    {
        const Json doc = run_json({"sustained", "--corpus", corpus_str, "--writes", "64",
                                   "--sample-every", "8", "--pump-every", "4",
                                   "--high-watermark", "2", "--max-batch", "1"},
                                  0);
        CHECK(num(doc, "dirty_latency_max_ms") > 0.0);
        CHECK(num(doc, "dirty_latency_max_ms") >= num(doc, "dirty_latency_p50_ms"));
        CHECK(static_cast<int>(num(doc, "samples_reflected")) ==
              static_cast<int>(num(doc, "samples_tracked"))); // catch-up drain reflects them all
        CHECK(num(doc, "max_queue_depth") > 2.0);      // exceeded the tiny watermark
        CHECK(num(doc, "overload_transitions") >= 1.0); // the load-shed policy actually tripped
        CHECK(num(doc, "writes") == 64.0);
        const Json& cfg = doc.at("derivation_config");
        CHECK(static_cast<int>(cfg.at("high_watermark").as_number()) == 2);
    }

    // --- sustained cleanup left the corpus convergent: a warm attach still sees no changes ---
    {
        const Json doc = run_json({"attach", "--corpus", corpus_str, "--mode", "warm"}, 0);
        CHECK(static_cast<int>(num(doc, "files_changed")) == 0);
    }

    std::error_code ec;
    fs::remove_all(corpus, ec);

    CLI_TEST_MAIN_END();
}

// The committed golden pack corpus (R-QA-011: test corpora are versioned deliverables). Each case is
// a <name>.scenes.json (an in-memory scene set + optional sidecar blobs) with a committed <name>.pack
// golden built by context_pack_golden_gen. The corpus covers flat scenes, nested-instance content
// (nested sub-units packed inside their top-level parent unit), composed overrides, and packed
// binary sidecar payloads. This runner byte-compares each freshly built pack against its golden — so
// a format/encoding change is a reviewed golden diff — and additionally asserts determinism (build
// twice -> identical) and round-trip (the golden re-reads clean, hashes verified).

#include "pack_corpus.h"
#include "pack_test.h"

#include "context/editor/pack/pack_reader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

[[nodiscard]] std::string read_file_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

int main()
{
    const fs::path dir = PACK_CORPUS_DIR;
    CHECK(fs::exists(dir));

    const std::vector<fs::path> cases = pack_corpus::list_cases(dir);
    // A versioned deliverable — a silently-empty corpus must fail, not pass. The committed set is
    // flat / nested / overrides / sidecars.
    CHECK(cases.size() >= 4);

    for (const fs::path& scenes_file : cases)
    {
        const int failures_before = packtest::g_failures;

        const pack_corpus::CaseResult built = pack_corpus::build_pack_from_case(scenes_file);
        CHECK(built.ok);
        if (!built.ok)
            continue;

        // --- golden byte-compare: a freshly built pack must equal its committed <name>.pack -------
        const fs::path golden = pack_corpus::golden_path(scenes_file);
        CHECK(fs::exists(golden));
        if (fs::exists(golden))
        {
            const std::string want = read_file_bytes(golden);
            CHECK(built.bytes == want);
        }

        // --- determinism: the same case built twice is byte-identical (R-FILE-010) ----------------
        const pack_corpus::CaseResult again = pack_corpus::build_pack_from_case(scenes_file);
        CHECK(again.ok);
        CHECK(again.bytes == built.bytes);

        // --- round-trip: the pack re-reads clean, every chunk hash verified -----------------------
        const context::editor::pack::ParsedPack parsed =
            context::editor::pack::read_pack(built.bytes);
        CHECK(parsed.ok);
        CHECK(!parsed.entries.empty());

        if (packtest::g_failures != failures_before)
            std::fprintf(stderr, "pack corpus case FAILED: %s\n",
                         scenes_file.filename().string().c_str());
    }

    PACK_TEST_MAIN_END();
}

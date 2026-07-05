// The R-QA-011 / R-SEC-006 fuzz-corpus REPLAY gate. Per-PR CI replays every committed seed (valid +
// minimized malformed crashers) through the registry-routed importer under the isolation policy, and
// asserts: no crash (reaching the assertion == survived), a GRACEFUL result (ok-with-artifacts XOR
// failed-with-diagnostics — never ok-with-nothing), determinism even on garbage, and the sandbox
// audit (input-bytes-only, no network). This is NEVER open-ended fuzz time — it is corpus regression.

#include "context/editor/import/import_settings.h"
#include "context/editor/import/importer_registry.h"
#include "context/editor/import/isolated_runner.h"
#include "context/editor/import/platform_profile.h"
#include "context/editor/import/sandbox.h"

#include "import_test.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

using namespace context::editor::import;

namespace
{
std::optional<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "[fuzz] no corpora dir argument; skipping file replay (manual run)\n");
        IMPORT_TEST_MAIN_END();
    }

    namespace fs = std::filesystem;
    const fs::path root(argv[1]);
    CHECK(fs::exists(root)); // wiring guard: a supplied corpora dir MUST exist

    ImporterRegistry registry = default_importer_registry();
    const PlatformProfile win = *find_platform_profile("windows");
    const ImportSettings settings = resolve_import_settings("", "windows");

    std::size_t replayed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string path = entry.path().generic_string();
        const Importer* importer = registry.importer_for_path(path);
        if (importer == nullptr)
            continue; // e.g. the generator script / READMEs living beside the seeds
        const std::optional<std::string> bytes = read_file(entry.path());
        CHECK(bytes.has_value());

        // A jailed policy: the seed is the sole input; writes confined to its own cache key.
        SandboxPolicy policy;
        policy.jail_root = "/corpus";
        policy.input_path = "/corpus/in";
        policy.output_key = "/corpus/.cache/out";

        ImportInput in;
        in.source_path = policy.input_path;
        in.source_bytes = *bytes;
        in.settings = settings;
        in.platform = win;

        // Reaching past this call means the (possibly malformed) seed did not crash the importer.
        const IsolatedImport iso = run_isolated(*importer, in, policy);

        // Graceful: ok => at least one artifact; not-ok => at least one diagnostic. Never silent.
        if (iso.result.ok)
            CHECK(!iso.result.artifacts.empty());
        else
            CHECK(!iso.result.diagnostics.empty());

        // Deterministic even on garbage (a malformed seed must fail identically twice).
        const DeterminismReport report = check_deterministic(*importer, in);
        if (!report.deterministic)
            std::fprintf(stderr, "NON-DETERMINISTIC (corpus) %s: %s\n", path.c_str(),
                         report.divergence.c_str());
        CHECK(report.deterministic);

        // Isolation audit: the run saw only the input, no network (R-SEC-006/010).
        CHECK(!iso.audit.network_allowed);
        CHECK(iso.audit.input_path == policy.input_path);
        CHECK(iso.audit.output_key == policy.output_key);

        ++replayed;
    }

    std::fprintf(stderr, "[fuzz] replayed %s corpus entries\n", std::to_string(replayed).c_str());
    CHECK(replayed > 0); // the corpus MUST contain routable seeds, else the gate is vacuous

    IMPORT_TEST_MAIN_END();
}

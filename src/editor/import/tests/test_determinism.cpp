// The R-ASSET-001 run-determinism GATE: import twice, byte-compare. Covers in-code fixtures for all
// three importers, proves the gate has TEETH (a flaky importer is caught), and double-runs every
// routable committed corpus entry (argv[1]). The shared cache (L-28) is only sound if this holds.

#include "context/editor/import/import_settings.h"
#include "context/editor/import/importer_registry.h"
#include "context/editor/import/importers/gltf_importer.h"
#include "context/editor/import/importers/png_importer.h"
#include "context/editor/import/importers/wav_importer.h"
#include "context/editor/import/isolated_runner.h"
#include "context/editor/import/platform_profile.h"

#include "import_test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

// A deliberately NON-deterministic importer — returns different bytes each call — used only to prove
// check_deterministic actually detects a violation (a teeth test, not a tautology).
class FlakyImporter final : public Importer
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "flaky"; }
    [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {".flaky"}; }
    [[nodiscard]] std::uint32_t derived_format_version(ArtifactKind) const noexcept override
    {
        return 1;
    }
    [[nodiscard]] ImportResult import(const ImportInput&) const override
    {
        ImportResult result;
        result.ok = true;
        DerivedArtifact artifact;
        artifact.kind = ArtifactKind::texture;
        artifact.name = "texture";
        artifact.bytes = std::to_string(counter_++); // changes each call
        result.artifacts.push_back(std::move(artifact));
        return result;
    }

private:
    mutable int counter_ = 0;
};
} // namespace

int main(int argc, char** argv)
{
    const PlatformProfile win = *find_platform_profile("windows");
    const ImportSettings settings = resolve_import_settings("", "windows");

    auto run_det = [&](const Importer& importer, const std::string& bytes, const char* path) {
        ImportInput in;
        in.source_path = path;
        in.source_bytes = bytes;
        in.settings = settings;
        in.platform = win;
        const DeterminismReport report = check_deterministic(importer, in);
        if (!report.deterministic)
            std::fprintf(stderr, "NON-DETERMINISTIC %s: %s\n", path, report.divergence.c_str());
        CHECK(report.deterministic);
    };

    const PngImporter png;
    const WavImporter wav;
    const GltfImporter gltf;
    run_det(png, importtest::make_png(8, 8, 6), "fx-rgba.png");
    run_det(png, importtest::make_png(1, 1, 0), "fx-gray.png");
    run_det(wav, importtest::make_wav(2, 44100, 16, 16), "fx-stereo.wav");
    run_det(gltf, importtest::make_gltf_json(true), "fx-uv2.gltf");
    run_det(gltf, importtest::make_glb(importtest::make_gltf_json(false)), "fx.glb");

    // Teeth: a non-deterministic importer IS caught.
    {
        FlakyImporter flaky;
        const std::string data = "data";
        ImportInput in;
        in.source_path = "x.flaky";
        in.source_bytes = data;
        in.settings = settings;
        in.platform = win;
        const DeterminismReport report = check_deterministic(flaky, in);
        CHECK(!report.deterministic);
        CHECK(!report.divergence.empty());
    }

    // The committed corpus double-run gate (valid AND malformed entries must both be deterministic).
    if (argc >= 2)
    {
        namespace fs = std::filesystem;
        const fs::path root(argv[1]);
        CHECK(fs::exists(root)); // wiring guard: a supplied corpora dir MUST exist
        ImporterRegistry registry = default_importer_registry();
        std::size_t seen = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const std::string path = entry.path().generic_string();
            const Importer* importer = registry.importer_for_path(path);
            if (importer == nullptr)
                continue;
            const std::optional<std::string> bytes = read_file(entry.path());
            CHECK(bytes.has_value());
            ImportInput in;
            in.source_path = path;
            in.source_bytes = *bytes;
            in.settings = settings;
            in.platform = win;
            const DeterminismReport report = check_deterministic(*importer, in);
            if (!report.deterministic)
                std::fprintf(stderr, "NON-DETERMINISTIC %s: %s\n", path.c_str(),
                             report.divergence.c_str());
            CHECK(report.deterministic);
            ++seen;
        }
        std::fprintf(stderr, "[determinism] double-ran %s corpus entries\n",
                     std::to_string(seen).c_str());
        CHECK(seen > 0); // the corpus must contain routable files, else the gate is vacuous
    }

    IMPORT_TEST_MAIN_END();
}

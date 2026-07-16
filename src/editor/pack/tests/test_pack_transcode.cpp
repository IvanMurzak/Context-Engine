// End-to-end per-platform variant PACK PATH (task a03, R-BUILD-003): import → transcode → pack, the
// real chain the M8 build pipeline (a05) will drive. Proves the a03 Definition of Done on real
// artifacts rather than synthetic blobs (test_pack_writer.cpp covers the writer's SELECTION logic in
// isolation): ONE source project packs for TWO targets with per-target variant payloads, a per-platform
// meta override (L-36) reaches the packed bytes, the pack self-verifies each variant chunk, and a
// repeat pack of the same (project, target) is byte-identical — the R-FILE-010 cache-hit property.
//
// This test links context_import (the transcode node) on top of context_pack deliberately: the pack
// LIBRARY keeps no import dependency (layering), so the wiring is proven here, in a test.

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/import/import_settings.h"
#include "context/editor/import/importer.h"
#include "context/editor/import/importers/png_importer.h"
#include "context/editor/import/importers/wav_importer.h"
#include "context/editor/import/platform_profile.h"
#include "context/editor/import/transcode.h"
#include "context/editor/pack/pack_format.h"
#include "context/editor/pack/pack_reader.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/canonical.h"

#include "import_test.h" // the CHECK harness + the minimal PNG/WAV asset builders

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace import = context::editor::import;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;

namespace
{

class MapResolver final : public compose::SceneResolver
{
public:
    void add(const char* path, const char* json)
    {
        serializer::CanonicalizeResult r = serializer::canonicalize(json);
        CHECK(r.is_json);
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, r.root);
        CHECK(doc.has_value());
        if (doc.has_value())
            docs_[path] = std::move(*doc);
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// Import `source` and transcode the named artifact for `platform_id` — one link of the build chain.
[[nodiscard]] import::TranscodeResult import_and_transcode(const import::Importer& importer,
                                                           const std::string& source,
                                                           const char* source_path,
                                                           const std::string& meta,
                                                           const char* platform_id,
                                                           import::ArtifactKind kind,
                                                           const char* name_needle)
{
    const import::PlatformProfile* profile = import::find_platform_profile(platform_id);
    CHECK(profile != nullptr);
    import::TranscodeResult out;
    if (profile == nullptr)
        return out;

    import::ImportInput in;
    in.source_path = source_path;
    in.source_bytes = source;
    in.settings = import::resolve_import_settings(meta, platform_id);
    in.platform = *profile;

    const import::ImportResult imported = importer.import(in);
    CHECK(imported.ok);
    for (const import::DerivedArtifact& a : imported.artifacts)
        if (a.kind == kind && a.name.find(name_needle) != std::string::npos)
            return import::transcode_variant(a, *profile);
    CHECK(false); // the expected artifact was not produced
    return out;
}

// Build the sidecar set for one target: the texture + audio assets, each carrying the target's
// transcoded variant (what the a05 build pipeline hands the writer).
[[nodiscard]] std::vector<pack::PackSidecar> build_sidecars(const std::string& png_bytes,
                                                            const std::string& wav_bytes,
                                                            const std::string& meta,
                                                            const char* platform_id)
{
    const import::PngImporter png;
    const import::WavImporter wav;

    const import::TranscodeResult tex =
        import_and_transcode(png, png_bytes, "hero.png", meta, platform_id,
                             import::ArtifactKind::texture, "texture");
    const import::TranscodeResult pcm =
        import_and_transcode(wav, wav_bytes, "shot.wav", meta, platform_id,
                             import::ArtifactKind::audio, "pcm");
    CHECK(tex.ok);
    CHECK(pcm.ok);

    const std::uint32_t selector = static_cast<std::uint32_t>(pack::platform_variant_for(platform_id));

    pack::PackSidecar tex_sc;
    tex_sc.relpath = "textures/hero.png.bin";
    tex_sc.raw_hash = 0x1111111111111111ULL; // the untranscoded source's address (the common fallback)
    tex_sc.bytes = png_bytes;
    tex_sc.variants.push_back({selector, tex.variant.content_hash, tex.variant.bytes});

    pack::PackSidecar pcm_sc;
    pcm_sc.relpath = "audio/shot.wav.bin";
    pcm_sc.raw_hash = 0x2222222222222222ULL;
    pcm_sc.bytes = wav_bytes;
    pcm_sc.variants.push_back({selector, pcm.variant.content_hash, pcm.variant.bytes});

    return {std::move(tex_sc), std::move(pcm_sc)};
}

} // namespace

int main()
{
    // A per-platform override (L-36): the Web build imports this texture as linear, desktop as sRGB.
    const std::string meta =
        R"({"importSettings":{"srgb":true},"platforms":{"web":{"srgb":false}}})";
    const std::string png_bytes = importtest::make_png(64, 32, 6, 8, 0, true);
    const std::string wav_bytes = importtest::make_wav(2, 44100, 16, 64);

    MapResolver resolver;
    resolver.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "aaaa0000aaaa0001", "name": "Hero", "components": {
          "sprite": {"$sidecar": "textures/hero.png.bin", "hash": "1229782938247303441"},
          "sfx": {"$sidecar": "audio/shot.wav.bin", "hash": "2459565876494606882"}
        }}
      ]})");
    const compose::ComposedScene scene = compose::flatten("root.scene.json", resolver);
    CHECK(scene.ok);
    const compose::ContentUnitSet units = compose::partition_content_units(scene, resolver);

    pack::PackWriteOptions win_opts;
    win_opts.engine_version = 42;
    win_opts.target_platform = static_cast<std::uint32_t>(pack::PlatformVariant::windows);
    pack::PackWriteOptions web_opts = win_opts;
    web_opts.target_platform = static_cast<std::uint32_t>(pack::PlatformVariant::web);

    const std::vector<pack::PackSidecar> win_sidecars =
        build_sidecars(png_bytes, wav_bytes, meta, "windows");
    const std::vector<pack::PackSidecar> web_sidecars =
        build_sidecars(png_bytes, wav_bytes, meta, "web");

    // --- DoD: the same project packs for TWO targets with per-target variant payloads --------------
    const pack::PackWriteResult win_pack = pack::write_pack(units, scene, win_sidecars, win_opts);
    const pack::PackWriteResult web_pack = pack::write_pack(units, scene, web_sidecars, web_opts);
    CHECK(win_pack.ok);
    CHECK(web_pack.ok);
    CHECK(win_pack.bytes != web_pack.bytes);

    const pack::ParsedPack win_parsed = pack::read_pack(win_pack.bytes);
    const pack::ParsedPack web_parsed = pack::read_pack(web_pack.bytes);
    CHECK(win_parsed.ok); // read_pack verifies every chunk's content hash — the variants self-verify
    CHECK(web_parsed.ok);

    // Every sidecar entry in each pack carries that target's platform column + its transcoded variant.
    int win_variant_entries = 0;
    int web_variant_entries = 0;
    for (const pack::PackEntry& e : win_parsed.entries)
        if (e.is_sidecar)
        {
            CHECK(e.platform == static_cast<std::uint32_t>(pack::PlatformVariant::windows));
            CHECK(e.chunk_bytes.rfind("CTXV", 0) == 0); // a transcoded variant container, not the source
            ++win_variant_entries;
        }
    for (const pack::PackEntry& e : web_parsed.entries)
        if (e.is_sidecar)
        {
            CHECK(e.platform == static_cast<std::uint32_t>(pack::PlatformVariant::web));
            CHECK(e.chunk_bytes.rfind("CTXV", 0) == 0);
            ++web_variant_entries;
        }
    CHECK(win_variant_entries == 2); // the texture + the audio
    CHECK(web_variant_entries == 2);

    // The packed variants are genuinely the per-target formats: BCn desktop vs ASTC Web (texture),
    // verbatim PCM16 desktop vs µ-law-companded Web (audio) — the R-BUILD-003 "one source asset →
    // each target's optimal format" property, visible in the PACKED bytes.
    {
        const import::TranscodeResult win_tex = import_and_transcode(
            import::PngImporter{}, png_bytes, "hero.png", meta, "windows",
            import::ArtifactKind::texture, "texture");
        const import::TranscodeResult web_tex = import_and_transcode(
            import::PngImporter{}, png_bytes, "hero.png", meta, "web",
            import::ArtifactKind::texture, "texture");
        CHECK(win_tex.variant.target_format == "bc7");
        CHECK(web_tex.variant.target_format == "astc_8x8");

        const pack::PackEntry* win_e = pack::find_unit(win_parsed, win_tex.variant.content_hash);
        const pack::PackEntry* web_e = pack::find_unit(web_parsed, web_tex.variant.content_hash);
        CHECK(win_e != nullptr); // addressable by the variant's content hash (load-by-GUID)
        CHECK(web_e != nullptr);
        CHECK(win_e->chunk_bytes == win_tex.variant.bytes);
        CHECK(web_e->chunk_bytes == web_tex.variant.bytes);
        CHECK(win_e->chunk_bytes != web_e->chunk_bytes); // per-target payloads

        // The audio Web variant is materially smaller than desktop PCM16 (µ-law halves the samples) —
        // the memory-constrained ceiling actually pays off in the packed bytes (R-ASSET-003).
        const import::TranscodeResult win_pcm = import_and_transcode(
            import::WavImporter{}, wav_bytes, "shot.wav", meta, "windows",
            import::ArtifactKind::audio, "pcm");
        const import::TranscodeResult web_pcm = import_and_transcode(
            import::WavImporter{}, wav_bytes, "shot.wav", meta, "web",
            import::ArtifactKind::audio, "pcm");
        CHECK(web_pcm.variant.bytes.size() < win_pcm.variant.bytes.size());
        CHECK(pack::find_unit(web_parsed, web_pcm.variant.content_hash) != nullptr);
    }

    // --- DoD: cache hit on repeat — the same (project, target) re-packs byte-identically ------------
    {
        const std::vector<pack::PackSidecar> again =
            build_sidecars(png_bytes, wav_bytes, meta, "windows");
        const pack::PackWriteResult win_again = pack::write_pack(units, scene, again, win_opts);
        CHECK(win_again.ok);
        CHECK(win_again.bytes == win_pack.bytes);
    }

    IMPORT_TEST_MAIN_END();
}

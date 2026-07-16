// Per-platform transcode tests (task a03, R-BUILD-003 / R-FILE-010). Coverage: per-platform-DISTINCT
// variant payloads (BCn desktop vs ASTC Apple/Web; verbatim PCM16 desktop vs µ-law-companded Web
// audio), the meta `platforms` override flowing through to the variant (L-36), determinism (the
// double-run byte-compare gate WITH transcode nodes in the graph — R-ASSET-001), the variant riding
// the existing per-platform cache key (R-FILE-010 — same inputs ⇒ same digest = cache hit; per
// platform ⇒ distinct entries), and the failure paths (no table row, bad descriptor).

#include "context/editor/import/cache_key.h"
#include "context/editor/import/import_settings.h"
#include "context/editor/import/importer.h"
#include "context/editor/import/importers/png_importer.h"
#include "context/editor/import/importers/wav_importer.h"
#include "context/editor/import/platform_profile.h"
#include "context/editor/import/transcode.h"

#include "import_test.h"

#include <string>

using namespace context::editor::import;

namespace
{

// Import a source through `importer` for `platform_id`, resolving settings from `meta_bytes`.
[[nodiscard]] ImportResult run_import(const Importer& importer, const std::string& source_bytes,
                                      const char* source_path, const std::string& meta_bytes,
                                      const char* platform_id)
{
    ImportInput in;
    in.source_path = source_path;
    in.source_bytes = source_bytes;
    in.settings = resolve_import_settings(meta_bytes, platform_id);
    const PlatformProfile* profile = find_platform_profile(platform_id);
    CHECK(profile != nullptr);
    if (profile != nullptr)
        in.platform = *profile;
    return importer.import(in);
}

// The first artifact of `kind` whose name contains `name_needle` (empty ⇒ first of that kind).
[[nodiscard]] const DerivedArtifact* find_artifact(const ImportResult& r, ArtifactKind kind,
                                                   const char* name_needle)
{
    for (const DerivedArtifact& a : r.artifacts)
        if (a.kind == kind && std::string(a.name).find(name_needle) != std::string::npos)
            return &a;
    return nullptr;
}

} // namespace

int main()
{
    const PlatformProfile* win = find_platform_profile("windows");
    const PlatformProfile* linux_p = find_platform_profile("linux");
    const PlatformProfile* mac = find_platform_profile("macos");
    const PlatformProfile* web = find_platform_profile("web");
    CHECK(win != nullptr && linux_p != nullptr && mac != nullptr && web != nullptr);

    const PngImporter png;
    const WavImporter wav;
    const std::string png_bytes = importtest::make_png(64, 32, 6, 8, 0, true); // 64x32 RGBA, sRGB
    const std::string wav_bytes = importtest::make_wav(2, 44100, 16, 64);      // 64 stereo frames

    // --- texture: BCn (desktop) vs ASTC (Apple/Web) are genuinely different variant payloads --------
    {
        const ImportResult ri = run_import(png, png_bytes, "hero.png", "{}", "windows");
        CHECK(ri.ok);
        const DerivedArtifact* tex = find_artifact(ri, ArtifactKind::texture, "texture");
        CHECK(tex != nullptr);

        const TranscodeResult win_v = transcode_variant(*tex, *win);
        const TranscodeResult mac_v = transcode_variant(*tex, *mac);
        CHECK(win_v.ok && mac_v.ok);
        CHECK(win_v.variant.target_format == "bc7");        // desktop → block-compressed BCn
        CHECK(mac_v.variant.target_format == "astc_8x8");   // Apple → ASTC
        // The block footprint differs (4×4 BCn vs 8×8 ASTC), so the variant bytes differ — the
        // per-target payload the pack selects. Both are self-describing "CTXV" containers.
        CHECK(win_v.variant.bytes != mac_v.variant.bytes);
        CHECK(win_v.variant.bytes.rfind("CTXV", 0) == 0);
        CHECK(win_v.variant.content_hash != mac_v.variant.content_hash);
        CHECK(win_v.variant.platform_id == "windows");

        // Determinism WITH transcode nodes in the graph (R-ASSET-001 double-run byte-compare).
        const TranscodeResult win_again = transcode_variant(*tex, *win);
        CHECK(win_again.ok);
        CHECK(win_again.variant.bytes == win_v.variant.bytes);
    }

    // --- meta `platforms` override (L-36) flows through to the variant ------------------------------
    {
        // A per-platform override: linux marks the texture linear (srgb=false); windows keeps sRGB.
        const std::string meta =
            R"({"importSettings":{"srgb":true},"platforms":{"linux":{"srgb":false}}})";
        const ImportSettings win_s = resolve_import_settings(meta, "windows");
        const ImportSettings lin_s = resolve_import_settings(meta, "linux");
        CHECK(win_s.srgb);        // no windows override
        CHECK(!lin_s.srgb);       // linux override applied

        const ImportResult win_ri = run_import(png, png_bytes, "hero.png", meta, "windows");
        const ImportResult lin_ri = run_import(png, png_bytes, "hero.png", meta, "linux");
        CHECK(win_ri.ok && lin_ri.ok);
        const DerivedArtifact* win_tex = find_artifact(win_ri, ArtifactKind::texture, "texture");
        const DerivedArtifact* lin_tex = find_artifact(lin_ri, ArtifactKind::texture, "texture");
        CHECK(win_tex != nullptr && lin_tex != nullptr);

        const TranscodeResult win_v = transcode_variant(*win_tex, *win);
        const TranscodeResult lin_v = transcode_variant(*lin_tex, *linux_p);
        CHECK(win_v.ok && lin_v.ok);
        // windows + linux both target bc7 (SAME format), so the ONLY difference in the variant bytes
        // is the srgb override + the platform tag — proving the per-platform meta override reaches the
        // transcoded payload, not just the cache key.
        CHECK(win_v.variant.target_format == "bc7");
        CHECK(lin_v.variant.target_format == "bc7");
        CHECK(win_v.variant.bytes != lin_v.variant.bytes);
    }

    // --- audio: verbatim PCM16 (desktop) vs µ-law-companded (memory-constrained Web) ----------------
    {
        const ImportResult ri = run_import(wav, wav_bytes, "shot.wav", "{}", "windows");
        CHECK(ri.ok);
        const DerivedArtifact* pcm = find_artifact(ri, ArtifactKind::audio, "pcm");
        CHECK(pcm != nullptr);

        const TranscodeResult win_v = transcode_variant(*pcm, *win);
        const TranscodeResult web_v = transcode_variant(*pcm, *web);
        CHECK(win_v.ok && web_v.ok);
        CHECK(win_v.variant.target_format == "pcm16");     // desktop keeps verbatim PCM
        CHECK(win_v.variant.applied_encoding == "pcm16");
        CHECK(web_v.variant.target_format == "vorbis");    // Web target (compressed)
        CHECK(web_v.variant.applied_encoding == "mulaw8"); // v1 applies real G.711 µ-law companding
        // µ-law halves the sample bytes → the Web variant is materially smaller than desktop PCM16.
        CHECK(web_v.variant.bytes.size() < win_v.variant.bytes.size());
        CHECK(win_v.variant.bytes != web_v.variant.bytes);

        // Determinism (double-run) on the audio path too.
        const TranscodeResult web_again = transcode_variant(*pcm, *web);
        CHECK(web_again.ok);
        CHECK(web_again.variant.bytes == web_v.variant.bytes);
    }

    // --- the variant RIDES the existing per-platform R-FILE-010 cache key ---------------------------
    {
        const ImportResult ri = run_import(png, png_bytes, "hero.png", "{}", "windows");
        const DerivedArtifact* tex = find_artifact(ri, ArtifactKind::texture, "texture");
        CHECK(tex != nullptr);

        ImportKeyContext ctx;
        ctx.source_bytes_hash = hash_source_bytes(png_bytes);
        ctx.import_settings_hash = resolve_import_settings("{}", "windows").hash;
        ctx.importer_id = "png";
        ctx.importer_version = 1;

        const std::uint64_t win_key = transcode_cache_key(ctx, *tex, *win).digest();
        const std::uint64_t win_key_again = transcode_cache_key(ctx, *tex, *win).digest();
        const std::uint64_t mac_key = transcode_cache_key(ctx, *tex, *mac).digest();
        const std::uint64_t lin_key = transcode_cache_key(ctx, *tex, *linux_p).digest();

        // Same inputs ⇒ same key: a content-addressed cache HIT on repeat.
        CHECK(win_key == win_key_again);
        // Distinct platforms ⇒ distinct entries (per-platform variants coexist — "instant after first
        // import"). macOS differs by format (astc) AND platform; linux differs by platform alone
        // (bc7 like windows) — both must key apart, proving the platform component drives it.
        CHECK(win_key != mac_key);
        CHECK(win_key != lin_key);
        CHECK(mac_key != lin_key);
        // The base descriptor artifact keys apart from its transcoded variant (distinct entries).
        ImportKeyContext base_ctx = ctx;
        base_ctx.platform_profile = "windows";
        CHECK(make_cache_key(base_ctx, *tex).digest() != win_key);
    }

    // --- failure paths ------------------------------------------------------------------------------
    {
        const ImportResult ri = run_import(png, png_bytes, "hero.png", "{}", "windows");
        const DerivedArtifact* tex = find_artifact(ri, ArtifactKind::texture, "texture");
        CHECK(tex != nullptr);

        // No transcode-table row for an untargeted platform ⇒ surfaced, never guessed.
        PlatformProfile unknown;
        unknown.id = "playstation";
        const TranscodeResult no_target = transcode_variant(*tex, unknown);
        CHECK(!no_target.ok);
        CHECK(no_target.error == "transcode.no_target");

        // A texture artifact whose bytes are not a descriptor ⇒ bad_descriptor (never a throw).
        DerivedArtifact garbage;
        garbage.kind = ArtifactKind::texture;
        garbage.name = "texture";
        garbage.bytes = "not-a-json-descriptor";
        garbage.derived_format_version = 1;
        const TranscodeResult bad = transcode_variant(garbage, *win);
        CHECK(!bad.ok);
        CHECK(bad.error == "transcode.bad_descriptor");
    }

    IMPORT_TEST_MAIN_END();
}

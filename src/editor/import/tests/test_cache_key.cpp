// The R-FILE-010 cache key: exhaustive-component sensitivity + sibling-artifact keying.

#include "context/editor/import/cache_key.h"

#include "import_test.h"

#include <string>

using namespace context::editor::import;

namespace
{
DerivedArtifact make_artifact(ArtifactKind kind, std::string name, std::string bytes,
                              std::uint32_t fmt)
{
    DerivedArtifact a;
    a.kind = kind;
    a.name = std::move(name);
    a.bytes = std::move(bytes);
    a.derived_format_version = fmt;
    return a;
}
} // namespace

int main()
{
    // The source-hash helper is deterministic + input-sensitive.
    CHECK(hash_source_bytes("abc") == hash_source_bytes("abc"));
    CHECK(hash_source_bytes("abc") != hash_source_bytes("abd"));

    // Build identity: populated + stable across calls (a warm-cache prerequisite).
    CHECK(!current_cpu_isa().empty());
    CHECK(importer_build_hash() != 0);
    CHECK(importer_build_hash() == importer_build_hash());

    ImportKeyContext ctx;
    ctx.source_bytes_hash = hash_source_bytes("SOURCE-BYTES");
    ctx.import_settings_hash = 111;
    ctx.importer_id = "png";
    ctx.importer_version = 1;
    ctx.platform_profile = "windows";
    ctx.registered_set_hash = 0;
    const DerivedArtifact art = make_artifact(ArtifactKind::texture, "texture", "BYTES", 1);

    const ImportCacheKey base = make_cache_key(ctx, art);
    CHECK(base.importer_build_hash == importer_build_hash()); // auto-filled by make_cache_key
    CHECK(base.cpu_isa == std::string(current_cpu_isa()));    // auto-filled
    CHECK(base.artifact_name == "texture");
    CHECK(base.derived_format_version == 1);

    // Deterministic: same inputs -> same digest AND path.
    CHECK(make_cache_key(ctx, art).digest() == base.digest());
    CHECK(make_cache_key(ctx, art).cache_path() == base.cache_path());

    const std::uint64_t d = base.digest();

    // EVERY enumerated component changes the digest (R-FILE-010: nothing is keyed out by accident).
    ImportCacheKey k = base;
    k = base; k.source_bytes_hash ^= 1U; CHECK(k.digest() != d);
    k = base; k.import_settings_hash ^= 1U; CHECK(k.digest() != d);
    k = base; k.importer_id = "wav"; CHECK(k.digest() != d);
    k = base; k.importer_version = 2; CHECK(k.digest() != d);
    k = base; k.platform_profile = "web"; CHECK(k.digest() != d);
    k = base; k.importer_build_hash ^= 1U; CHECK(k.digest() != d);
    k = base; k.cpu_isa = "arm64"; CHECK(k.digest() != d);
    k = base; k.artifact_kind = ArtifactKind::mesh; CHECK(k.digest() != d);
    k = base; k.artifact_name = "texture.hi"; CHECK(k.digest() != d);
    k = base; k.derived_format_version = 2; CHECK(k.digest() != d);
    k = base; k.registered_set_hash = 7; CHECK(k.digest() != d);

    // Sibling artifacts of ONE import (same source/settings/platform, different name + bytes) key
    // APART — the descriptor-vs-payload content-address collision guard.
    const DerivedArtifact pcm = make_artifact(ArtifactKind::audio, "audio.pcm", "PCM", 1);
    const DerivedArtifact desc = make_artifact(ArtifactKind::audio, "audio", "DESC", 1);
    CHECK(make_cache_key(ctx, pcm).digest() != make_cache_key(ctx, desc).digest());
    CHECK(make_cache_key(ctx, pcm).cache_path() != make_cache_key(ctx, desc).cache_path());

    // cache_path shape: "<importer_id>/<kind>/<16-hex>", producer + kind namespaced.
    CHECK(base.cache_path().rfind("png/texture/", 0) == 0);
    CHECK(base.cache_path().size() == std::string("png/texture/").size() + 16);

    IMPORT_TEST_MAIN_END();
}

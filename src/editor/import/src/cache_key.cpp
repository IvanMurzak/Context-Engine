// Import cache key — CPU-ISA + importer-build-hash detection and the deterministic component fold.

#include "context/editor/import/cache_key.h"

#include "context/editor/filesync/content_hash.h"
#include "context/editor/serializer/canonical.h"

#include <string>
#include <string_view>

// Stringize helper for the MSVC compiler-version macro (an integer, not a string literal).
#define CTX_IMPORT_STR2(x) #x
#define CTX_IMPORT_STR(x) CTX_IMPORT_STR2(x)

namespace context::editor::import
{
namespace
{
// FNV-1a 64-bit (the engine's content-hash family — filesync/serializer use the same). Folding the
// key's components through it is the "extend the existing machinery" mandate: no new hash family.
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

std::uint64_t fold_bytes(std::uint64_t h, const unsigned char* p, std::size_t n) noexcept
{
    for (std::size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

std::uint64_t fold_u64(std::uint64_t h, std::uint64_t v) noexcept
{
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i)
    {
        bytes[i] = static_cast<unsigned char>(v & 0xffU);
        v >>= 8;
    }
    return fold_bytes(h, bytes, sizeof(bytes));
}

// A length-prefixed string fold so component boundaries never collide ("ab"+"c" != "a"+"bc").
std::uint64_t fold_str(std::uint64_t h, std::string_view s) noexcept
{
    h = fold_u64(h, s.size());
    return fold_bytes(h, reinterpret_cast<const unsigned char*>(s.data()), s.size());
}
} // namespace

std::uint64_t hash_source_bytes(std::string_view bytes) noexcept
{
    return filesync::content_hash(bytes);
}

std::string_view current_cpu_isa() noexcept
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

std::uint64_t importer_build_hash() noexcept
{
    // Identity of the compiled importer set: the framework's derived-format epoch + the toolchain
    // stamp, assembled ENTIRELY from compile-time string literals (no allocation → honestly noexcept).
    // Stable across identical rebuilds (the shared cache stays warm), but changes when the toolchain
    // changes — a compiler/flag swap that could alter float rounding re-keys, which is exactly the
    // cross-machine determinism scope R-FILE-010 defers. A real per-importer object-hash lands with
    // the native build pipeline (documented in cache_key.h — never silently assumed object-exact).
#if defined(__clang__)
    constexpr std::string_view kStamp = "context-importers/1;clang;" __clang_version__;
#elif defined(__GNUC__)
    constexpr std::string_view kStamp = "context-importers/1;gcc;" __VERSION__;
#elif defined(_MSC_VER)
    constexpr std::string_view kStamp = "context-importers/1;msvc;" CTX_IMPORT_STR(_MSC_FULL_VER);
#else
    constexpr std::string_view kStamp = "context-importers/1;unknown-toolchain";
#endif
    return serializer::canonical_hash_of(kStamp);
}

std::uint64_t ImportCacheKey::digest() const noexcept
{
    // Order-fixed fold over EVERY component (R-FILE-010's exhaustive enumeration). The same
    // components always yield the same digest; any single change yields a different one.
    std::uint64_t h = kFnvOffset;
    h = fold_u64(h, source_bytes_hash);
    h = fold_u64(h, import_settings_hash);
    h = fold_str(h, importer_id);
    h = fold_u64(h, importer_version);
    h = fold_str(h, platform_profile);
    h = fold_u64(h, importer_build_hash);
    h = fold_str(h, cpu_isa);
    h = fold_u64(h, static_cast<std::uint64_t>(artifact_kind));
    h = fold_str(h, artifact_name);
    h = fold_u64(h, derived_format_version);
    h = fold_u64(h, registered_set_hash);
    return h;
}

std::string ImportCacheKey::cache_path() const
{
    // "<importer_id>/<kind>/<16-hex digest>" — content-addressed + producer-namespaced, write-once.
    std::uint64_t d = digest();
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        hex[static_cast<std::size_t>(i)] = kHex[d & 0xfU];
        d >>= 4;
    }
    std::string out;
    std::string_view kind_name = artifact_kind_name(artifact_kind);
    out.reserve(importer_id.size() + kind_name.size() + hex.size() + 2);
    out += importer_id;
    out += '/';
    out += kind_name;
    out += '/';
    out += hex;
    return out;
}

ImportCacheKey make_cache_key(const ImportKeyContext& context, const DerivedArtifact& artifact)
{
    ImportCacheKey key;
    key.source_bytes_hash = context.source_bytes_hash;
    key.import_settings_hash = context.import_settings_hash;
    key.importer_id = context.importer_id;
    key.importer_version = context.importer_version;
    key.platform_profile = context.platform_profile;
    key.importer_build_hash = importer_build_hash(); // this build's compiled-importer identity
    key.cpu_isa = std::string(current_cpu_isa());     // this build's ISA (determinism scope)
    key.artifact_kind = artifact.kind;
    key.artifact_name = artifact.name;
    key.derived_format_version = artifact.derived_format_version;
    key.registered_set_hash = context.registered_set_hash;
    return key;
}

} // namespace context::editor::import

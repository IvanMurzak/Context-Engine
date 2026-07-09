// Material/shader authoring IR + shader-variant enumeration (R-REND-005, first slice; issue #121).
// The in-memory form an authored shader/material compiles from — deliberately BACKEND-FREE: no native
// shader toolchain is pulled here, so this builds and tests under every toolchain including the local
// Ninja+Strawberry-GCC Windows dev gate. The real glslang/SPIRV-Cross backend lands in a later
// sub-task behind the IShaderCompiler seam (shader_compiler.h); shader compilation + variant
// generation are derivation-graph nodes cached per R-FILE-010 (shader_cache.h).

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::render::material
{

// The authored shader stages this backend-free slice models. The real backend adds more as needed;
// the seam does not depend on this being exhaustive.
enum class ShaderStageKind
{
    Vertex,
    Fragment,
    Compute,
};

// Canonical lowercase token for a stage kind (used by (de)serialize + the fake backend).
[[nodiscard]] std::string_view to_string(ShaderStageKind kind) noexcept;

// Inverse of to_string(); nullopt for an unknown token.
[[nodiscard]] std::optional<ShaderStageKind> stage_from_string(std::string_view name) noexcept;

// One authored shader stage: its kind, entry point, and verbatim authored source body. The source is
// carried opaquely — it is NEVER compiled in this slice (backend-free).
struct ShaderStage
{
    ShaderStageKind kind = ShaderStageKind::Vertex;
    std::string entry_point;
    std::string source;

    bool operator==(const ShaderStage&) const = default;
};

// A shader keyword: a define/permutation axis with its ordered set of possible values. The cartesian
// product of every keyword's values is the shader-variant space (enumerate_variants()). A boolean
// toggle is just a two-value keyword, e.g. {"off", "on"}.
struct ShaderKeyword
{
    std::string name;
    std::vector<std::string> values;

    bool operator==(const ShaderKeyword&) const = default;
};

// The authoring IR for one shader/material: a name, its keyword axes, and its stages.
struct ShaderIr
{
    std::string name;
    std::vector<ShaderKeyword> keywords;
    std::vector<ShaderStage> stages;

    bool operator==(const ShaderIr&) const = default;
};

// One point in the variant space: an assignment keyword -> chosen value, kept SORTED by keyword name
// so a variant has a single canonical, order-independent identity.
struct VariantKey
{
    std::vector<std::pair<std::string, std::string>> defines;

    // Canonical "KW=VAL;KW=VAL;..." rendering (already sorted by keyword name); "" for the empty
    // (no-keyword) variant. This string is a cache-key component (shader_cache.h).
    [[nodiscard]] std::string canonical() const;

    bool operator==(const VariantKey&) const = default;
};

// Parse an authored shader (the corpus text format — see corpus/*.shader) into the IR. Returns nullopt
// on a malformed document (unknown directive, a `stage` without its `endstage`, a `keyword` with no
// value, a missing/empty shader name).
[[nodiscard]] std::optional<ShaderIr> parse_shader(std::string_view text);

// Serialize the IR to the canonical authored form. serialize() is CANONICALIZING: keywords are
// emitted sorted by name, so serialize(parse(serialize(ir))) == serialize(ir) and the output is a
// stable content-hash subject regardless of authored keyword order.
[[nodiscard]] std::string serialize_shader(const ShaderIr& ir);

// Enumerate every shader variant (the cartesian product of the keyword value sets). Deterministic
// order: keywords sorted by name, values in declared order, the last keyword varying fastest. A shader
// with no keywords yields exactly one empty variant. Keywords with an empty value set contribute no
// axis (defensive — parse() never produces one).
[[nodiscard]] std::vector<VariantKey> enumerate_variants(const ShaderIr& ir);

// FNV-1a 64-bit of raw bytes, lowercase hex (16 chars). The dependency-free content-hash primitive the
// IR hash, the fake backend's stub blob, and the R-FILE-010 cache key all build on. A real backend may
// substitute SHA-256 without changing any seam.
[[nodiscard]] std::string content_hash_hex(std::string_view bytes);

// Content hash of the canonical IR serialization (content_hash_hex(serialize_shader(ir))). The
// R-FILE-010 shader-compile cache keys on this (shader_cache.h) — see § "cache key" there.
[[nodiscard]] std::string ir_content_hash(const ShaderIr& ir);

} // namespace context::render::material

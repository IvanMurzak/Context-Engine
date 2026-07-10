// Material/shader authoring IR + shader-variant enumeration (R-REND-005, first slice; issue #121).
// The in-memory form an authored shader/material compiles from — deliberately BACKEND-FREE: no native
// shader toolchain is pulled here, so this builds and tests under every toolchain including the local
// Ninja+Strawberry-GCC Windows dev gate. The real glslang/SPIRV-Cross backend lands in a later
// sub-task behind the IShaderCompiler seam (shader_compiler.h); shader compilation + variant
// generation are derivation-graph nodes cached per R-FILE-010 (the re-homed ShaderCompileNode in
// src/editor/derivation/, issue #126).

#pragma once

#include <cstddef>
#include <cstdint>
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

// ------------------------------------------------------------------ the material contract (M4)
// The material-facing input surface an authored shader declares: typed scalar/vector parameters and
// semantic texture slots (R-REND-004 metallic-roughness baseline). This is the M4 "material
// contract" R-REND-006 requires to carry LIGHTMAP INPUTS: a slot with TextureSemantic::Lightmap plus
// its UV-channel selection (channel 1 = the reserved UV2), so the frozen contract never forecloses
// baked lighting — the baker itself is COULD/post-v1 and deliberately absent here.

// The type of one material parameter (its component count).
enum class MaterialParamType
{
    Float,
    Vec2,
    Vec3,
    Vec4,
};

// Number of components of a parameter type (Float=1 ... Vec4=4).
[[nodiscard]] std::size_t component_count(MaterialParamType type) noexcept;

// Canonical lowercase token ("float", "vec2", ...) and its inverse; nullopt for an unknown token.
[[nodiscard]] std::string_view to_string(MaterialParamType type) noexcept;
[[nodiscard]] std::optional<MaterialParamType> param_type_from_string(std::string_view name) noexcept;

// What a texture slot feeds in the metallic-roughness model. Lightmap is the R-REND-006 hook.
enum class TextureSemantic
{
    BaseColor,
    MetallicRoughness,
    Normal,
    Emissive,
    Occlusion,
    Lightmap,
};

// Canonical lowercase token ("base_color", "lightmap", ...) and its inverse; nullopt when unknown.
[[nodiscard]] std::string_view to_string(TextureSemantic semantic) noexcept;
[[nodiscard]] std::optional<TextureSemantic> semantic_from_string(std::string_view name) noexcept;

// One declared material parameter. Defaults are kept as the authored decimal TOKENS (validated float
// literals, one per component) rather than parsed floats: the canonical serialized form — and thus
// the ir_content_hash / R-FILE-010 cache key — is then byte-stable with no locale- or
// float-formatting dependency (the R-DATA-006 canonical-form discipline). Factors are unitless; any
// angular parameter is radians (R-DATA-006 SI + radians).
struct MaterialParam
{
    std::string name;
    MaterialParamType type = MaterialParamType::Float;
    std::vector<std::string> defaults; // component_count(type) validated float-literal tokens

    bool operator==(const MaterialParam&) const = default;
};

// One declared texture slot: its binding name, semantic, and which UV set it samples. uv_channel 1
// is the UV2 channel mesh import reserves (R-REND-006); the parser bounds the channel to [0,3].
struct TextureSlot
{
    std::string name;
    TextureSemantic semantic = TextureSemantic::BaseColor;
    std::uint32_t uv_channel = 0;

    bool operator==(const TextureSlot&) const = default;
};

// Validate one float-literal token: [+-]?digits[.digits]?([eE][+-]?digits)? — locale-independent by
// construction (the token is never converted through the C locale machinery on the authoring path).
[[nodiscard]] bool is_float_literal(std::string_view token) noexcept;

// The authoring IR for one shader/material: a name, its keyword axes, its material contract
// (parameters + texture slots — possibly empty; a contract-free document serializes exactly as it
// did before the contract existed, so pre-contract content hashes are unchanged), and its stages.
struct ShaderIr
{
    std::string name;
    std::vector<ShaderKeyword> keywords;
    std::vector<MaterialParam> params;
    std::vector<TextureSlot> textures;
    std::vector<ShaderStage> stages;

    bool operator==(const ShaderIr&) const = default;
};

// One point in the variant space: an assignment keyword -> chosen value, kept SORTED by keyword name
// so a variant has a single canonical, order-independent identity.
struct VariantKey
{
    std::vector<std::pair<std::string, std::string>> defines;

    // Canonical "KW=VAL;KW=VAL;..." rendering (already sorted by keyword name); "" for the empty
    // (no-keyword) variant. This string is a cache-key component (ShaderCompileNode, R-FILE-010).
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
// R-FILE-010 shader-compile node keys on this (ShaderCompileNode::cache_key, src/editor/derivation/).
[[nodiscard]] std::string ir_content_hash(const ShaderIr& ir);

} // namespace context::render::material

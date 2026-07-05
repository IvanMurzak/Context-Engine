// The engine schema vocabulary + units law (R-DATA-006) — pinned in M2 BEFORE the first component
// schemas freeze, so every per-kind JSON Schema shares ONE encoding for the same semantic types.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::schema
{

// --- the x-ctx-* annotation keys (the shared vocabulary every kind schema uses, L-32) -----------

inline constexpr std::string_view kKeySemanticType = "x-ctx-type";
inline constexpr std::string_view kKeyStorage = "x-ctx-storage";
inline constexpr std::string_view kKeyRef = "x-ctx-ref";
inline constexpr std::string_view kKeyUnits = "x-ctx-units";
inline constexpr std::string_view kKeyUnion = "x-ctx-union";
inline constexpr std::string_view kKeySidecar = "x-ctx-sidecar";

// The schema-blessed human/AI annotation field (L-32 bans JSON comments): ACCEPTED on every object
// level of every authored kind, as a string or an array of strings.
inline constexpr std::string_view kNotesField = "notes";

// --- engine semantic types (`x-ctx-type`) --------------------------------------------------------
//
// JSON's primitives cannot express the engine's recurring value shapes; without a pinned encoding
// every package invents its own (the ecosystem-fracture retrofit R-DATA-006 exists to prevent).
// The pinned shapes:
//   quaternion — [x, y, z, w]: exactly 4 finite numbers (normalization is NOT enforced in v1).
//   color      — {"space": <color space>, "value": [c, ...]} with a DECLARED color space and
//                3 (RGB) or 4 (RGBA) finite components. An undeclared color space is invalid by
//                construction — that is the requirement's point.
//   curve      — {"keys": [{"t": <num>, "v": <num>}, ...]}: >= 1 key, finite t/v, t strictly
//                increasing (duplicate key times have no single evaluation order).
//   gradient   — {"stops": [{"t": <0..1>, "color": <color>}, ...]}: >= 1 stop, t within [0, 1] and
//                non-decreasing (equal t encodes a hard step), each stop color a valid `color`.
//   bit-flags  — ["flag", ...]: unique, non-empty strings (set semantics; authored order is the
//                canonical order, L-32 stable array ordering).
enum class SemanticType
{
    quaternion,
    color,
    curve,
    gradient,
    bit_flags,
};

// The stable vocabulary ids ("quaternion", "color", "curve", "gradient", "bit-flags").
[[nodiscard]] std::string_view semantic_type_id(SemanticType type) noexcept;
[[nodiscard]] bool is_semantic_type_id(std::string_view id) noexcept;
// Precondition: is_semantic_type_id(id).
[[nodiscard]] SemanticType semantic_type_from_id(std::string_view id) noexcept;

// One problem found while checking a semantic-type instance. `subpointer` is a JSON-pointer
// fragment RELATIVE to the checked value ("" = the value itself, "/value/2" = a component inside).
struct SemanticIssue
{
    std::string subpointer;
    std::string message;
};

// Check `value` against the pinned shape of `type`. Empty result = valid.
[[nodiscard]] std::vector<SemanticIssue> check_semantic(SemanticType type,
                                                        const serializer::JsonValue& value);

// --- the units law (`x-ctx-units`) ----------------------------------------------------------------
//
// GLOBAL LAW: authored data is SI units + radians EVERYWHERE — there are no per-field unit choices
// (mixed units are the classic silent-corruption bug an agent cannot see in JSON). `x-ctx-units` is
// therefore pure METADATA surfaced through schema introspection (R-CLI-005/013) so humans, GUIs,
// and agents render/reason without guessing — it is NEVER a conversion switch. A schema declaring a
// non-SI unit (degrees, feet, milliseconds, ...) is rejected when the kind schema is compiled.
// Pinned unit ids (lowercase; "1" = dimensionless): 1, m, kg, s, rad, sr, hz, m/s, m/s^2, rad/s,
// rad/s^2, n, j, w, pa, kg/m^3.
[[nodiscard]] bool is_si_unit(std::string_view unit) noexcept;

// --- numeric storage declarations (`x-ctx-storage`) ----------------------------------------------
//
// Declares the numeric width/layout the declarative component compiler derives storage from (feeds
// the M3 R-LANG-010 layout derivation). Pinned grammar: "<base>" or "<base>x<lanes>" with
// base in {f32, f64, i8, i16, i32, i64, u8, u16, u32, u64} and lanes in {2, 3, 4, 9, 16}
// (vectors, quaternions, 3x3 / 4x4 matrices).
[[nodiscard]] bool is_storage_layout(std::string_view layout) noexcept;

// --- the pinned tagged-union convention (`x-ctx-union`) ------------------------------------------
//
// Polymorphic fields use ONE convention — {"type": "<ns>:<shape>", ...} — never per-package ad-hoc
// encodings. A tag is "<ns>:<shape>" where ns and shape each match [a-z][a-z0-9_-]*.
[[nodiscard]] bool is_union_tag(std::string_view tag) noexcept;

// --- typed references (`x-ctx-ref`) ---------------------------------------------------------------
//
// A reference field declares its REQUIRED target kind. The authored value is the L-34 dual form
// {"$ref": "<guid>", "path": "<hint>"} (cross-file; path optional) or {"$entity": "<id>"}
// (same-file). Target-kind enforcement runs through the RefTargetResolver meta-lookup seam
// (validator.h) once the asset database lands; the vocabulary + shape check ship now.
inline constexpr std::string_view kRefGuidField = "$ref";
inline constexpr std::string_view kRefPathField = "path";
inline constexpr std::string_view kRefEntityField = "$entity";

// --- binary-sidecar fields (`x-ctx-sidecar`) -----------------------------------------------------
//
// A field carrying `x-ctx-sidecar` holds a versioned binary-sidecar REFERENCE — the L-33 escape
// hatch for heavy numeric payloads (tilemap cell grids, mesh buffers, …) that would bloat the
// authored JSON past the ~1 MB split-nudge ceiling if inlined as base64. The authored value is the
// pinned reference object {"$sidecar": "<relpath>", "hash": "<decimal>"} (serializer/sidecar_ref.h);
// the annotation's STRING value names the sidecar's logical content-type (e.g. "tilemap-cells"), so
// schema introspection (R-CLI-005) tells agents/tools what a sidecar carries without opening it —
// the symmetric analogue of x-ctx-ref naming a reference's target kind. This makes the binary-sidecar
// mechanism (PR #54) a first-class, introspectable schema vocabulary citizen: the validator checks
// the reference SHAPE (serializer::is_sidecar_ref) and the file-sync layer owns resolution +
// on-disk hash verification (filesync/sidecar.h). x-ctx-sidecar is mutually exclusive with
// x-ctx-type / x-ctx-ref / x-ctx-union (a field is exactly one of a semantic value, a typed ref, a
// tagged union, or a sidecar ref).
[[nodiscard]] bool is_sidecar_content_type(std::string_view content_type) noexcept;

// --- color spaces ---------------------------------------------------------------------------------

// The pinned color-space ids a `color` must declare: srgb, srgb-linear, display-p3, rec2020.
[[nodiscard]] bool is_color_space(std::string_view space) noexcept;

// --- notes ----------------------------------------------------------------------------------------

// The blessed `notes` shape: a string, or an array of strings.
[[nodiscard]] bool is_valid_notes(const serializer::JsonValue& value) noexcept;

} // namespace context::editor::schema

// Player save-game document (R-DATA-005): RuntimeKernel's OWN serialization of player state —
// progress, world snapshots — versioned like other serialized data (R-DATA-004) and fully distinct
// from authored project files (R-FILE-009: RuntimeKernel never parses authored files; a save is a
// different format with a different feed). A save records the per-component schemaVersion map (the
// SAME per-payload stamps as authored files — L-32/L-37) and addresses every entity by its L-37
// composed identity (the ONE identity shared by saves, network ids, and query results), so a save
// taken before a re-derivation or engine upgrade still addresses the same entities.
//
// This is the M2 groundwork seam (the first src/runtime/ target). The full runtime save/load API
// surface (streaming world snapshots, incremental saves) lands with the shipped-runtime milestones;
// what M2 freezes is the FORMAT (header + per-component versions + composed-identity binding) and
// the minimal migration runner (save_migration.h) so player persistence is not a retrofit.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::runtime::save
{

namespace serializer = context::editor::serializer;

// The save ENVELOPE format version. Bumped only if the save container shape itself changes; the
// per-component PAYLOAD versions live in SaveDocument::component_versions (L-37), independent of it.
inline constexpr std::int64_t kSaveFormatVersion = 1;

// The default declared save back-compat scope (N versions, R-DATA-005): how many schema versions
// behind the current one the migration runner will still migrate a component payload from. A
// bounded promise, not unbounded — a payload older than N versions is refused, not best-effort read.
inline constexpr std::int64_t kDefaultBackCompatScope = 8;

// The save-kind marker value (the "$save" member). Distinct from an authored file's "$schema" so a
// save can never be mistaken for an authored document (R-FILE-009 boundary made explicit on disk).
inline constexpr std::string_view kSaveKind = "ctx:save";

// One saved entity: its L-37 composed identity plus its per-component saved payloads. The identity
// is the deterministic id-path/stable hash — stable across re-derivation and engine upgrade — so a
// save re-addresses the same entities after the world is re-derived (R-DATA-005).
struct SaveEntity
{
    std::uint64_t identity = 0;       // composed identity (L-37); serialized as a 16-hex string
    serializer::JsonValue components; // object: "<ns>:<type>" -> saved payload (runtime state)
};

// A player save (R-DATA-005). RuntimeKernel's own serialization; the canonical serializer emits it
// (R-DATA-004 versioned data), but it is NOT an authored file.
struct SaveDocument
{
    std::int64_t format_version = kSaveFormatVersion;
    std::string engine_version; // informational: the build that wrote the save (never a compat gate
                                // — back-compat is per-component, not per-engine-version)
    std::int64_t back_compat_scope = kDefaultBackCompatScope;
    // Per-component schemaVersion map (the L-32/L-37 stamps) — authored order preserved on parse,
    // canonically key-sorted on serialize.
    std::vector<std::pair<std::string, std::int64_t>> component_versions;
    std::vector<SaveEntity> entities;

    // The saved schemaVersion of a component type, or nullopt when the header does not stamp it.
    [[nodiscard]] const std::int64_t* saved_version(std::string_view type) const;
};

// Render a composed identity as its canonical 16-char lowercase-hex string (zero-padded) and parse
// it back. The save's addressing key round-trips losslessly through the canonical JSON form.
[[nodiscard]] std::string format_identity(std::uint64_t identity);
[[nodiscard]] bool parse_identity(std::string_view text, std::uint64_t& out) noexcept;

// Serialize a save to canonical JSON (R-DATA-004). Deterministic: keys sorted, entity + component
// order stable. The bytes are RuntimeKernel's own on-disk save format.
[[nodiscard]] std::string serialize_save(const SaveDocument& save);

// The outcome of parsing a save document.
struct SaveParseResult
{
    bool ok = false;
    SaveDocument save;
    std::vector<serializer::Diagnostic> diagnostics; // `code` drawn from the save.* / json.* families
};

// Parse a save from its canonical-JSON bytes. Deterministic and total (never throws). ok == false
// with a diagnostic when: the bytes are not strict JSON; the root is not a save envelope (missing
// or mis-typed "$save"/"saveFormatVersion"/"entities"); the envelope version is newer than this
// build supports (save.format_unsupported); an entity carries a non-16-hex identity or a
// non-object component map (save.malformed).
[[nodiscard]] SaveParseResult parse_save(std::string_view bytes);

} // namespace context::runtime::save

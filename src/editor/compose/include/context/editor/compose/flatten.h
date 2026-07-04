// Scene-composition flatten (L-35 / R-DATA-002): scenes instance other scenes with id-path
// addressed, innermost-out override entries; composition flattens in the derivation graph at zero
// runtime cost. Composed identity is the deterministic id-path (L-37); every composed value
// carries a winning-value-first provenance chain (R-CLI-006 read side).

#pragma once

#include "context/editor/compose/scene_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::compose
{

// The M2 schema invariants over composition shape (R-FILE-011(e)) plus a flatten output budget.
// Defaults are the profile the engine ships; a daemon may tune them, tests may shrink them.
struct ComposeLimits
{
    // Composition nesting-depth cap: an instance chain deeper than this is not expanded
    // (compose.depth_exceeded, blocking). The root scene is depth 0.
    std::size_t max_depth = 32;
    // Fan-in diagnostic threshold: when one scene file is instanced at least this many times in a
    // single flatten, compose.fan_in (advisory) is emitted once for that file.
    std::size_t fan_in_threshold = 64;
    // Output budget: expansion stops once a flatten has composed this many entities
    // (compose.too_many_entities, blocking) — dense fan-in/diamond graphs cannot run away with
    // memory (the R-FILE-011(e) bounded-index discipline applied to the flatten output).
    std::size_t max_entities = 100000;
};

// Where a composed value came from — one link of the R-CLI-006 provenance chain.
struct ProvenanceEntry
{
    enum class Source
    {
        schema_default,  // reserved: the schema dialect declares no defaults yet (emitter-ready)
        template_value,  // the defining scene's authored value
        override_value,  // an override entry in an instancing scene
    };
    Source source = Source::template_value;
    std::string file;    // the contributing authored file
    std::string pointer; // where the contributing value lives inside `file`
    std::size_t level = 0; // instancing depth of the contributor (0 = the flatten root scene)
};

// The provenance chain for one overridden pointer inside a composed entity, winning-value-first
// (R-CLI-006: the agent sees every contributor — which template supplied the value and which
// instancing level overrode it).
struct FieldProvenance
{
    std::string pointer; // JSON pointer inside the composed entity value
    std::vector<ProvenanceEntry> chain;
};

// One flattened entity of the composed scene.
struct ComposedEntity
{
    // The L-35 id-path from the flatten root: [instanceId, …, entityId]. Composed identity per
    // L-37 is [rootScene] + this path — see identity_hash_of().
    std::vector<std::string> id_path;
    std::uint64_t identity_hash = 0; // identity_hash_of(root_path, id_path) — stable across
                                     // re-derivation and engine upgrade (L-37)
    serializer::JsonValue value;     // the composed entity object (overrides applied)
    std::string template_file;       // the defining scene (for an added entity: the adding scene)
    std::string template_pointer;    // the entity's pointer inside template_file
    std::size_t template_level = 0;  // instancing depth of the defining scene
    // Chains for every pointer an override touched. Pointers without an entry resolve to the
    // template value — provenance_for() applies that fallback.
    std::vector<FieldProvenance> field_provenance;
};

// The flatten output: the zero-runtime-cost composed form of one root scene. Deterministic —
// entity order is expansion order (root entity, authored entities, structural adds, instances
// depth-first, all in authored order), diagnostics are emission-ordered.
struct ComposedScene
{
    std::string root_path;
    bool ok = true; // false iff any blocking diagnostic was emitted
    std::vector<ComposedEntity> entities;
    std::vector<ComposeDiagnostic> diagnostics;
};

// Resolves a project-root-relative scene path to its composition view. The derivation graph
// implements this over its retained scene docs; tests implement it over in-memory maps. Instance
// `scene` paths match resolver keys VERBATIM (project-root-relative, the derivation node key) —
// there is no relative-to-referrer resolution in v1.
class SceneResolver
{
public:
    virtual ~SceneResolver() = default;
    [[nodiscard]] virtual const SceneDoc* resolve(std::string_view path) const = 0;
};

// Flatten `root_path`. Total and deterministic: blocking findings (missing scene, cycle, depth
// cap, duplicate ids, entity budget) flip `ok` but never abort the rest of the expansion, so the
// composed view is always the best-effort whole (R-FILE-003 last-good philosophy lives one layer
// up, in the derivation graph).
[[nodiscard]] ComposedScene flatten(std::string_view root_path, const SceneResolver& resolver,
                                    const ComposeLimits& limits = {});

// The provenance chain for `pointer` inside `entity`, winning-value-first. Chains are recorded
// per EXACT overridden pointer; any other pointer falls back to the entity's template origin
// (a single template_value link) — it is template-sourced by construction.
[[nodiscard]] std::vector<ProvenanceEntry> provenance_for(const ComposedEntity& entity,
                                                          std::string_view pointer);

// The deterministic composed identity hash (L-37): FNV-1a 64 over the id-path prefixed with the
// root scene path ([rootScene, instanceId, …, entityId]), segments joined with a 0x1F separator.
// The root path is length-prefixed, so the encoding stays injective even for a path containing
// the separator byte (id-path segments — stable ids or the $root token — never contain it), and
// distinct paths never collide by concatenation. Stable across re-derivation, engine upgrade, and
// process restarts.
[[nodiscard]] std::uint64_t identity_hash_of(std::string_view root_path,
                                             const std::vector<std::string>& id_path);

// Canonical-JSON emitters (the R-CLI-006 read-side result shapes; the query surface serializes
// composed results through these so CLI/RPC and tests can never drift):
//   provenance_json  -> [{"file", "level", "pointer", "source"}…]  (winning-value-first;
//                        source ∈ "schemaDefault" | "template" | "override")
//   composed_scene_json -> {"diagnostics": […], "entities": [{"idPath", "identityHash" (16-hex
//                        string), "provenance", "templateFile", "templatePointer", "value"}…],
//                        "ok", "rootScene"}
[[nodiscard]] std::string provenance_json(const std::vector<ProvenanceEntry>& chain);
[[nodiscard]] std::string composed_scene_json(const ComposedScene& scene);

} // namespace context::editor::compose

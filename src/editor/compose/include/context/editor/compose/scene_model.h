// The typed composition view over one authored ctx:scene document (L-33/L-35): id-keyed entity
// and instance collections, the scene-root entity, and the id-path-addressed override entries.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::compose
{

// One machine-readable composition finding (the R-FILE-003 diagnostic shape, compose vocabulary).
// `blocking` mirrors the validate node's semantics: a blocking finding flips ComposedScene::ok to
// false (the flatten still emits everything that composed — best-effort, never half-written);
// advisory findings inform hygiene tooling (L-35) without failing the flatten.
struct ComposeDiagnostic
{
    std::string code;    // stable dotted identifier, e.g. "compose.cycle"
    std::string message; // human/AI-readable explanation
    std::string file;    // the authored file the finding points into
    std::string pointer; // JSON pointer of the offending value inside `file` (may be empty)
    bool blocking = false;
};

// The scene-root entity (L-35): scene-level state is singleton components on it. Inert by default
// when the scene is instanced as a sub-scene; `composable` opts it into the parent flatten. Bakes
// are derived artifacts and never authored (the ctx:scene schema has no baked member — an authored
// one fails validation upstream of composition).
struct RootEntity
{
    bool present = false;
    std::string id;          // explicit stable id, or empty -> addressable as kSceneRootId
    bool composable = false; // opt-in to composing under an instancing parent
    serializer::JsonValue value; // the authored root object (components + notes, as written)
    std::string pointer;         // "/root"
};

// One entity of the scene's id-keyed `entities` collection (only entries with a VALID stable id
// are carried — id-less/invalid entries are excluded with a diagnostic; they cannot participate
// in composed identity).
struct AuthoredEntity
{
    std::string id;
    serializer::JsonValue value; // the whole authored entity object (id + name + components + …)
    std::string pointer;         // "/entities/<index>"
};

// One instancing entry of the `instances` collection: this scene composes `scene` under the
// stable id `id` (the first segment of override id-paths addressing into that subtree).
struct SceneInstance
{
    std::string id;
    std::string scene;   // project-root-relative path of the instanced scene file
    std::string pointer; // "/instances/<index>"
};

// The three override kinds (L-35): per-field value overrides plus the two explicit structural
// overrides.
enum class OverrideKind
{
    field,  // `pointer` + `value`: set one field inside the addressed entity
    add,    // `add`: compose an extra entity under the addressed instance subtree
    remove, // `remove: true`: remove the addressed entity (or whole instance subtree)
};

// One override entry. `path` is the L-35 id-path, addressed from an instance id of the OWNING
// scene inward: [instanceId, …, entityId] for field/remove (remove may also stop at an instance
// id to remove the whole subtree), [instanceId, …] for add (the instance subtree the entity joins).
struct OverrideEntry
{
    std::vector<std::string> path;
    OverrideKind kind = OverrideKind::field;
    std::string field_pointer;   // kind == field: JSON pointer into the target entity
    serializer::JsonValue value; // kind == field: the overriding value; kind == add: the entity
    std::string pointer;         // "/overrides/<index>" in the owning file
};

// The composition-relevant content of one authored scene document, fully owning its data (the
// derivation graph retains SceneDocs across passes; the parse tree they were built from does not
// outlive the ingest).
struct SceneDoc
{
    std::string path; // the document's project-root-relative path (the derivation node key)
    RootEntity root;
    std::vector<AuthoredEntity> entities;
    std::vector<SceneInstance> instances;
    std::vector<OverrideEntry> overrides;
    // Intra-file findings from building this view: compose.missing_id / compose.invalid_id /
    // compose.duplicate_id / compose.override_malformed. Duplicate ids are BLOCKING (the id-keyed
    // collection invariant, L-33/R-FILE-012); the rest are advisory with the entry excluded.
    std::vector<ComposeDiagnostic> diagnostics;

    // True when this scene uses composition itself: it instances sub-scenes, carries overrides,
    // or opts its root entity into parent flattens.
    [[nodiscard]] bool participates_in_composition() const noexcept
    {
        return !instances.empty() || !overrides.empty() || (root.present && root.composable);
    }
};

// True iff the parsed document binds to the scene kind ("$schema" == "ctx:scene"). Unbound
// documents (the M1 fixtures) and other kinds never enter composition.
[[nodiscard]] bool is_scene_document(const serializer::JsonValue& root);

// Build the composition view of a parsed scene document. nullopt when `root` is not a scene
// document (see is_scene_document); otherwise a SceneDoc whose collections carry the valid
// entries and whose `diagnostics` records everything excluded, per the rules above.
[[nodiscard]] std::optional<SceneDoc> build_scene_doc(std::string_view path,
                                                      const serializer::JsonValue& root);

} // namespace context::editor::compose

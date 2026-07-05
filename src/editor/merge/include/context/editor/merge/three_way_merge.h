// Schema-aware structural three-way merge over authored JSON (R-FILE-012) — the convergence
// primitive for worktree-per-agent parallelism (L-26/L-50). Field-path granular, id-based merge
// identity (L-33): the same intra-file id is the same entity (field-merged); the same id ADDED on
// both sides is a structural conflict, never silently unified. NEVER last-writer-wins, NEVER text
// conflict markers — an unresolvable divergence becomes a machine-readable Conflict instead.

#pragma once

#include "context/editor/merge/conflict.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace context::editor::merge
{

// The installed-schema floor (L-37): the highest schema version this engine/package set understands,
// per registered file kind ("$schema" id -> version) and per component type ("<ns>:<type>" ->
// schemaVersion, the L-32 componentVersions map). A merge input whose stamps EXCEED the floor cannot
// be field-merged (the driver cannot parse data its schemas do not know), so it is the R-FILE-012(a)
// whole-file `newer_stamped` conflict class — never a parse error. An empty floor disables the check
// (structural merge proceeds unconditionally); an unlisted kind/component is not judged.
struct SchemaFloor
{
    std::map<std::string, std::int64_t> kind_versions;      // "$schema" id -> max supported "version"
    std::map<std::string, std::int64_t> component_versions; // "<ns>:<type>" -> max supported schemaVersion
};

struct MergeOptions
{
    SchemaFloor floor;                       // newer-stamped whole-file detection (L-37)
    bool detect_meta_guid = true;            // detect the L-36 meta-guid whole-file class from root `guid`
};

struct MergeResult
{
    bool clean = false;              // true iff no conflicts (fully auto-merged)
    bool whole_file = false;         // true iff a whole-file class fired (merged == a copy of ours)
    serializer::JsonValue merged;    // merged tree: auto-merged where possible, OURS where conflicting
    std::vector<Conflict> conflicts; // one entry per unresolved conflict (non-empty iff !clean)
};

// Structurally merge three PARSED authored documents. `base` is the common ancestor; `ours` the
// current branch; `theirs` the incoming branch. The merged tree keeps every auto-mergeable change
// from both sides (disjoint field edits, id-keyed element field-merges, one-sided adds/removes);
// each divergence that cannot auto-resolve is recorded as a Conflict AND left at the OURS value in
// `merged` (a deterministic, valid-JSON placeholder — resolve-conflict rewrites it), never a silent
// last-writer-wins and never a text marker.
[[nodiscard]] MergeResult merge_documents(const serializer::JsonValue& base,
                                          const serializer::JsonValue& ours,
                                          const serializer::JsonValue& theirs,
                                          const MergeOptions& options = {});

// Deep structural equality used throughout the merge: objects are order-INSENSITIVE (the canonical
// form sorts keys, so authored member order is not identity), arrays are order-SENSITIVE, scalars
// match exactly including the integer/unsigned/number domain the canonical form pins (json_tree.h).
[[nodiscard]] bool json_equal(const serializer::JsonValue& a,
                              const serializer::JsonValue& b) noexcept;

// True iff `array` is an id-keyed collection: a non-empty array whose every element is an object
// carrying a string `id` member (the L-33 arrays-of-objects-with-`id` form). Such arrays merge by id
// identity; every other array is an opaque ordered value (3-way by equality, never index-merged).
[[nodiscard]] bool is_id_keyed_array(const serializer::JsonValue& array) noexcept;

} // namespace context::editor::merge

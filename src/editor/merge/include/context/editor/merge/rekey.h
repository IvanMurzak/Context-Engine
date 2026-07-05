// The post-merge convergence gate (R-FILE-012(c)): the duplicate-intra-file-id diagnostic + the
// re-key operation (mint a fresh id, rewrite in-file references). The L-26 worktree merge workflow
// runs `context validate` after a merge to prove structural convergence; a duplicate intra-file id
// — the residue of an id add/add or a raw copy — is re-keyed so identity stays file-scoped-unique.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::merge
{

// One duplicate-intra-file-id finding: a stable id carried by more than one object in a single file.
struct DuplicateId
{
    std::string id;
    std::vector<std::string> pointers; // RFC 6901 pointer to each object carrying the id (size >= 2)
};

// Scan a parsed document for entities sharing an intra-file id (L-33 file-scoped uniqueness). An
// "entity" is any object with a string `id` member of stable-id form (compose::is_stable_id). Only
// ids appearing on 2+ objects are returned; deterministic order (first-seen id, then pointer order).
// This is the `context validate` duplicate-intra-file-id diagnostic (reported as merge.duplicate_id).
[[nodiscard]] std::vector<DuplicateId> find_duplicate_ids(const serializer::JsonValue& root);

struct RekeyResult
{
    bool ok = false;
    std::string old_id;
    std::string new_id;
    std::uint64_t references_rewritten = 0;
    std::string error; // human message when ok == false (empty on success)
};

// Re-key the object at `pointer` (RFC 6901): replace its `id` member with `new_id` — minted fresh
// (compose::mint_stable_id) when `new_id` is empty — and rewrite the in-file references that
// UNAMBIGUOUSLY resolve to it. A reference is a `{"$entity": "<id>"}` value (L-34); it is rewritten
// only when, after the re-key, NO other object in the file still carries the old id (otherwise the
// old id is still live on the remaining holder and the reference is left with it — the correct split
// of a duplicate). Fails (ok=false, `error` set) when `pointer` does not resolve to an object with a
// stable `id`, or when `new_id` is a non-empty non-stable-id string.
[[nodiscard]] RekeyResult rekey_entity(serializer::JsonValue& root, std::string_view pointer,
                                       std::string new_id = {});

} // namespace context::editor::merge

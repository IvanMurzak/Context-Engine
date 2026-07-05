// The machine-readable conflict vocabulary a three-way merge produces (R-FILE-012). One `Conflict`
// per divergence the structural merge cannot auto-resolve; the CLI projects a list of these into the
// R-CLI-008 result envelope as `conflicts: [{path, base, ours, theirs}]`.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <optional>
#include <string>

namespace context::editor::merge
{

// The class of a conflict — every documented R-FILE-012 conflict shape has a case here, and the
// R-QA-011 corpus commits one fixture per class so each is provably reachable.
enum class ConflictClass
{
    field,          // a leaf/subtree diverged at field-path granularity (both sides changed it)
    id_add_add,     // the SAME intra-file id was added on both sides vs base — a structural
                    // conflict, NEVER silently unified (L-33 / R-FILE-012(b))
    delete_modify,  // one side removed an id/field the other side modified
    binary_sidecar, // whole-file: a binary sidecar differs on both sides — never content-merged,
                    // whole-file ours/theirs (L-33 / R-FILE-012(d))
    meta_guid,      // whole-file: both sides minted a DIFFERENT GUID for the same asset meta —
                    // whole-asset ours/theirs, identity is never field-blended (L-36 / R-FILE-012(d))
    newer_stamped,  // whole-file: an input carries payloads stamped newer than the installed
                    // schemas — a named whole-file class, NEVER a parse error (L-37 / R-FILE-012(a))
};

// The stable enum spelling (for messages / annotations).
[[nodiscard]] const char* to_string(ConflictClass klass) noexcept;

// The R-CLI-008 catalog code a conflict of this class carries (error_catalog.cpp owns the rows).
[[nodiscard]] const char* catalog_code(ConflictClass klass) noexcept;

// True for the three whole-file classes: the merge refuses at file granularity (merged output = ours
// by default) rather than field-merging content it must not blend.
[[nodiscard]] bool is_whole_file(ConflictClass klass) noexcept;

// One conflict entry.
//
//   `path`  — an RFC 6901 field-path into the MERGED document (id-path granularity for id-keyed
//             elements: `id` annotates the addressed element). "" is the whole document (the
//             whole-file classes). `context resolve-conflict --path <path>` resolves exactly this.
//   `id`    — the stable intra-file id of the addressed element, when the conflict sits inside an
//             id-keyed array (L-33); empty otherwise. The stable id-path surface R-FILE-012(a) names.
//   base/ours/theirs — each side's value, PRESENT-or-ABSENT: an absent optional means the value did
//             not exist on that side (a field added on both sides has no `base`; a delete/modify has
//             an absent side). The CLI omits absent sides from the envelope so absence is unambiguous.
struct Conflict
{
    std::string path;
    ConflictClass klass = ConflictClass::field;
    std::string id; // stable id of the addressed element (id-keyed arrays); empty otherwise
    std::optional<serializer::JsonValue> base;
    std::optional<serializer::JsonValue> ours;
    std::optional<serializer::JsonValue> theirs;
};

} // namespace context::editor::merge

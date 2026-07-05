// Applying a conflict resolution to a merged document (R-FILE-012): the `context resolve-conflict`
// engine half. Wraps the L-35 RFC 6901 pointer machinery (compose) so the CLI resolves a conflict
// entry without re-implementing pointer traversal or reaching across into compose.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <optional>
#include <string>
#include <string_view>

namespace context::editor::merge
{

struct ApplyResult
{
    bool ok = false;
    std::string error;
};

// Apply one conflict resolution at an RFC 6901 `pointer` in the merged document:
//   - a PRESENT `value` SETs it (creating intermediate object members as needed — L-35 override
//     semantics: --take ours/theirs writes the chosen side's value, --value writes an arbitrary one);
//   - an ABSENT `value` REMOVEs the addressed member/element (the chosen side did not have it — e.g.
//     --take the side that deleted the field).
// Fails (ok=false, `error` set) on a malformed pointer, a set that cannot place the value, or a
// removal whose pointer does not resolve.
[[nodiscard]] ApplyResult apply_resolution(serializer::JsonValue& root, std::string_view pointer,
                                           const std::optional<serializer::JsonValue>& value);

} // namespace context::editor::merge

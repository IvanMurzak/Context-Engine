// RFC 6901 JSON-pointer resolution + override application over the serializer's JSON tree —
// the pointer half of the L-35 per-field override entries (the composed WRITE path reuses these).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::compose
{

// Split an RFC 6901 pointer into unescaped reference tokens. A valid non-empty pointer starts
// with '/'; "~0"/"~1" unescape to '~'/'/'. Returns false (tokens untouched) for a malformed
// pointer: empty, not starting with '/', or containing an invalid '~' escape. The whole-document
// pointer "" is deliberately REJECTED here — a per-field override replacing the entire entity
// wholesale is a modeling error (author the entity instead), so compose treats it as malformed.
[[nodiscard]] bool parse_json_pointer(std::string_view pointer, std::vector<std::string>& tokens);

// Resolve `pointer` against `root`. nullptr when the pointer is malformed or any token fails to
// resolve (missing member, non-index token against an array, index out of range, or a scalar in
// the middle of the path). Array tokens must be canonical base-10 indices (no leading '+'/zeros,
// except "0" itself); the RFC's "-" append token never resolves on the read path.
[[nodiscard]] const serializer::JsonValue* resolve_json_pointer(const serializer::JsonValue& root,
                                                                std::string_view pointer);

// Apply an override: set the value `pointer` addresses inside `root` to a copy of `value`.
// Traversal rules (deterministic, mirroring resolve):
//   - object member present  -> descend / replace at the leaf;
//   - object member ABSENT   -> created (an override may introduce a new field/component, L-35) —
//                                intermediate created members are objects;
//   - array index in range   -> descend / replace at the leaf;
//   - array index out of range, "-" append, non-index token against an array, or a scalar in the
//     middle of the path -> false, `root` untouched (the caller surfaces compose.orphan_override —
//     overrides never grow arrays or retype containers on the read path).
[[nodiscard]] bool set_json_pointer(serializer::JsonValue& root, std::string_view pointer,
                                    const serializer::JsonValue& value);

} // namespace context::editor::compose

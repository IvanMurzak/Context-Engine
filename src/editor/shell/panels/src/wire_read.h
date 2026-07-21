// Shared wire-read primitives for the Shell panel feeds (the json_number_read.h pattern): the
// MECHANICAL member accessors over contract::Json, TU-shared so the feeds do not each re-declare
// them. Parse POLICY — which nodes to skip, what defaults apply, which shapes say nothing — stays
// per-feed (the ProblemsFeed tolerance discipline); only the mechanics live here. contract::Json's
// at() is TOTAL (a shared null for a missing key or a non-object), so each helper reads the member
// ONCE — no contains()+at() double scan inside the per-node/per-field wire-parse loops.

#pragma once

#include "context/editor/contract/json.h"

#include <string>

namespace context::editor::shell::panels
{

// The string member `key`, or empty when absent / not a string.
[[nodiscard]] inline std::string read_string(const contract::Json& object, const std::string& key)
{
    const contract::Json& value = object.at(key);
    return value.is_string() ? value.as_string() : std::string();
}

// The boolean member `key`; false when absent / not a boolean (as_bool is total).
[[nodiscard]] inline bool read_bool(const contract::Json& object, const std::string& key)
{
    return object.at(key).as_bool();
}

} // namespace context::editor::shell::panels

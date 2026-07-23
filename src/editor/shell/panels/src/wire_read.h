// Shared wire-read primitives for the Shell panel feeds (the json_number_read.h pattern): the
// MECHANICAL member accessors over contract::Json, TU-shared so the feeds do not each re-declare
// them. Parse POLICY — which nodes to skip, what defaults apply, which shapes say nothing — stays
// per-feed (the ProblemsFeed tolerance discipline); only the mechanics live here. contract::Json's
// at() is TOTAL (a shared null for a missing key or a non-object), so each helper reads the member
// ONCE — no contains()+at() double scan inside the per-node/per-field wire-parse loops.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
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

// A non-negative integer member as a u64; 0 when absent, not a number, or out of range.
//
// RANGE-CHECKED ON THE DOUBLE, before any integral cast, and that is the whole reason this is not a
// one-liner: `as_int()` is a `static_cast<std::int64_t>` of the stored double, and casting a double
// outside int64's range is UNDEFINED BEHAVIOUR — which the blocking `sanitize (ASan+UBSan, ubuntu)`
// leg reports as `float-cast-overflow`. The wire is untrusted (`Json::parse` accepts `1e300`
// happily), so a guard applied AFTER the cast would be guarding after the UB had already happened.
// The ceiling is 2^53, past which a double cannot represent consecutive integers anyway.
// (problems_feed.cpp's `read_u32` / `read_generation` apply the same discipline with their own
// ranges and fallbacks — the POLICY stays per-feed, only these mechanics are shared.)
[[nodiscard]] inline std::uint64_t read_u64(const contract::Json& object, const std::string& key)
{
    const contract::Json& value = object.at(key);
    if (!value.is_number())
    {
        return 0;
    }
    const double raw = value.as_number();
    if (!(raw >= 0.0 && raw <= 9007199254740992.0))
    {
        return 0;
    }
    return static_cast<std::uint64_t>(raw);
}

// The `data` member of an R-CLI-008 success envelope, or the reply itself when it is already bare.
// The daemon always answers `{ok, data, generationAfter, warnings}` (dispatcher.cpp's
// envelope_to_response), but tolerating the bare shape keeps a reader readable against a hand-built
// reply in a test without the parser having to be told which it is. Every feed unwrapping a reply
// starts with this hop; the feed-SPECIFIC hop that may follow (`sceneTree`, `inspector`) stays at
// its own call site, because which key to look for is policy.
[[nodiscard]] inline const contract::Json& envelope_data(const contract::Json& reply)
{
    const contract::Json& data = reply.at("data");
    return data.is_object() ? data : reply;
}

} // namespace context::editor::shell::panels

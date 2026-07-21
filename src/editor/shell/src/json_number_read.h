// The ONE range-guarded numeric read over contract::Json for the Shell's untrusted inputs
// (M9 e05d3, unifying the three readers that had each re-derived — or MISSED — the guard:
// editor_state.cpp's read_i32/read_u32, editor_state_bridge.cpp's read_pixel).
//
// WHY THE GUARD IS LOAD-BEARING. `Json::as_int()` is a `static_cast<int64_t>` of the stored double,
// and casting a double outside int64's range is UNDEFINED BEHAVIOUR — which the blocking
// `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`. Both inputs are untrusted
// (`Json::parse` accepts `1e300` happily): region rects arrive from the RENDERER wire, and the
// editor-state file is hand-editable/corruptible on disk. So the range check runs on the DOUBLE,
// BEFORE any integral cast — guarding after the cast would be guarding after the UB had already
// happened. A NaN fails every comparison and so also falls to the caller's fallback.
//
// Internal to context_editor_shell's own TUs (an src/-local header, not part of the public surface).

#pragma once

#include "context/editor/contract/json.h"

#include <optional>

namespace context::editor::shell::detail
{

// The member `key` of `obj` as a double, iff `obj` is an object carrying a NUMBER at `key` whose
// value lies inside [lo, hi]. nullopt otherwise — absent, non-object, non-number, NaN, or out of
// range all read the same way: "no usable number", and the CALLER decides the fallback. The caller
// casts the returned double to its integral type; the bounds it passes must fit that type, which is
// what makes the cast defined.
[[nodiscard]] inline std::optional<double> number_in_range(const contract::Json& obj, const char* key,
                                                           double lo, double hi)
{
    if (!obj.is_object() || !obj.contains(key))
    {
        return std::nullopt;
    }
    const contract::Json& value = obj.at(key);
    if (!value.is_number())
    {
        return std::nullopt;
    }
    const double raw = value.as_number();
    if (!(raw >= lo && raw <= hi))
    {
        return std::nullopt;
    }
    return raw;
}

} // namespace context::editor::shell::detail

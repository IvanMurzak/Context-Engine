// Path normalization + structural project-root jail (R-SEC-008).

#pragma once

#include <string>
#include <string_view>

namespace context::editor::filesync
{

// Lexically normalize a path: unify '\\' to '/', collapse '.' and empty segments, resolve '..'
// segments (without going above an absolute root), and drop a trailing '/'. Purely lexical — it never
// touches the filesystem, so it operates over the injectable FileStore seam and over stored intent-log
// paths alike.
[[nodiscard]] std::string normalize_path(std::string_view path);

// True iff `path` normalizes to a location at or beneath `root`. This is the structural path jail
// (R-SEC-008): a resumed or forged intent-log write can never escape the project root regardless of
// its HMAC. NOTE: this is the LOGICAL jail over the injectable seam. The fully TOCTOU-safe variant
// (open with O_NOFOLLOW / openat relative to a jail-root fd, then re-realpath after open) is the
// native FileStore impl's responsibility — documented here, out of the M1 seam's scope.
[[nodiscard]] bool is_inside_jail(std::string_view root, std::string_view path);

} // namespace context::editor::filesync

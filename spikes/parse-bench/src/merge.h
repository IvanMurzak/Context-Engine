// parse-bench spike — three-way STRUCTURAL merge at field-path granularity
// (`context merge-file`-class, R-FILE-012).
//
// Semantics measured (the load-bearing subset of R-FILE-012):
//   * field-path granularity: objects merge per key, recursively;
//   * unchanged-side wins: ours==base -> theirs, theirs==base -> ours;
//   * id-keyed collections (L-33): arrays whose elements are objects carrying a
//     string "id" merge BY ID, not by index — same id on both sides field-merges;
//     the same id ADDED on both sides relative to base is a structural conflict;
//   * conflicts are recorded in a machine-readable envelope (path, never text
//     markers — the merged output stays valid JSON); the merged tree carries the
//     "ours" value at conflicted paths, mirroring an envelope-returning driver
//     that leaves resolution to `context resolve-conflict`;
//   * plain scalar arrays are atomic values (a vector field conflicts as a unit).
#pragma once

#include <string>
#include <vector>

#include "json_value.h"

namespace ctx {

struct Conflict {
    std::string path;  // JSON-pointer-ish field path
};

struct MergeResult {
    JsonValue merged;
    std::vector<Conflict> conflicts;
};

// base/ours/theirs may be null pointers (= key absent on that side).
MergeResult merge3(const JsonValue* base, const JsonValue* ours, const JsonValue* theirs);

}  // namespace ctx

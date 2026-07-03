// Atomic write-temp-then-rename (R-FILE-004): per-file atomicity is the unit.

#pragma once

#include <string>
#include <string_view>

namespace context::editor::filesync
{

class FileStore;

// The temp path atomic_write() stages into for (path, unique). Exposed so the intent-log resume and
// tests can reason about the residue a crash leaves behind.
[[nodiscard]] std::string atomic_temp_path(std::string_view path, std::string_view unique);

// True when `name` is atomic-write staging residue produced by atomic_temp_path — i.e. it ends in the
// ".tmp" marker or carries the ".tmp.<unique>" infix. A precise marker check, NOT a bare ".tmp"
// substring: a legitimately-authored file such as "deploy.tmpl.yaml" must never be mistaken for
// staging residue and skipped by reconciliation / recovery.
[[nodiscard]] bool is_atomic_temp_name(std::string_view name);

// Atomically write `data` to `path`: stage into a sibling temp file, fsync it, then rename it over
// `path`, then fsync `path`. A concurrent reader of `path` observes either the complete OLD content
// or the complete NEW content — never a torn/partial write. Returns true on durable success. If the
// process "crashes" (SimulatedCrash) between the temp write and the rename, `path` is left untouched
// and the temp file is the only residue. `unique` disambiguates the temp name so concurrent writers
// (and intent-log resume) do not collide.
bool atomic_write(FileStore& fs, std::string_view path, std::string_view data,
                  std::string_view unique = "");

} // namespace context::editor::filesync

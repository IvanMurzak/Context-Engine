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

// True when `name` has the shape of atomic-write staging residue produced by atomic_temp_path — i.e.
// it ends in the ".tmp" marker (the unique-less form) or carries a ".tmp." infix (the ".tmp.<unique>"
// form). This is a purely LEXICAL marker check: it deliberately does NOT match a bare ".tmp" substring,
// so an authored file such as "deploy.tmpl.yaml" is never mistaken for residue. It DOES conservatively
// over-match an authored file that happens to share the shape (e.g. "config.tmp.bak") — biasing toward
// treating a possibly-residual name as engine-internal is the safe choice; a fully precise decision
// would need temp-registry / filesystem state, which is out of this lexical seam's scope.
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

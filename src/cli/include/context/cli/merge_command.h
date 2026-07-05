// The `context` merge family (M2 wave 4, R-FILE-012): merge-file / resolve-conflict / re-key /
// validate. Implemented over the src/editor/merge/ engine; each returns the uniform R-CLI-008
// envelope. `merge-file --driver` is the git merge-driver entry point (L-27: git invokes the driver,
// never the reverse). See merge_command.cpp.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// `context merge-file <base> <ours> <theirs> [<pathname>] [--output F] [--driver]` — schema-aware
// structural three-way merge. Writes the merged file (canonical JSON) and returns the R-CLI-008
// conflict envelope (`data.conflicts: [{path, base, ours, theirs}]`). `--driver` is git's merge
// driver mode (result written to the ours/`%A` path, non-zero exit on unresolved conflicts, a
// `<pathname>.ctxconflicts.json` sidecar dropped for the resolve loop).
[[nodiscard]] editor::contract::Envelope
run_merge_file(const std::map<std::string, std::string>& params,
               const std::map<std::string, std::string>& flags);

// `context resolve-conflict <file> --path P --take ours|theirs | --value <json>` — apply one
// resolution against the merged file (and its conflict sidecar), loop-wise until the sidecar empties.
[[nodiscard]] editor::contract::Envelope
run_resolve_conflict(const std::map<std::string, std::string>& params,
                     const std::map<std::string, std::string>& flags);

// `context re-key <file> --at <pointer> | --id <id>` — mint a fresh stable id for a duplicated
// entity and rewrite its unambiguous in-file references (the R-FILE-012(c) convergence remedy).
[[nodiscard]] editor::contract::Envelope
run_rekey(const std::map<std::string, std::string>& params,
          const std::map<std::string, std::string>& flags);

// `context validate [path]` — the post-merge convergence gate: report the duplicate-intra-file-id
// diagnostic (merge.duplicate_id) across the target file or project tree.
[[nodiscard]] editor::contract::Envelope
run_validate(const std::map<std::string, std::string>& params,
             const std::map<std::string, std::string>& flags);

} // namespace context::cli

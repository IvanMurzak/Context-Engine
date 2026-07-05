// `context set …` (composed WRITE path) + `context query --overrides …` (advisory hygiene) — the
// CLI side of the M2 composition write surface (R-CLI-006 / L-35).
//
// `context set <scene> <value> --pointer <p> --id-path <a/b/c> [--edit-template | --at-instance
// <a/b>]` writes a value onto a composed entity: an override entry in the OUTERMOST (root)
// instancing scene by default, the defining template with `--edit-template`, or a mid-level
// instancing scene with `--at-instance`. The write goes through filesync's R-FILE-004 atomic
// write-temp-then-rename (with the R-SEC-008 jail); `--if-match <raw-hash>` is the CAS guard. The
// result envelope reports the file + JSON-pointer actually written and BOTH labelled resulting
// hashes (raw-byte + canonical-content), per R-CLI-006.
//
// `context query --overrides diverged|redundant <scene>` lists the scene's advisory override-hygiene
// findings — never auto-pruned. This is a one-shot read over the project's scene files (the plain
// operational `query` verb stays daemon-served); the CLI intercepts the `--overrides` form.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>
#include <vector>

namespace context::cli
{

// `context set` — positionals are [<scene>, <value>]; flags carry pointer / id-path / target mode /
// project / if-match / dry-run. Returns the R-CLI-008 result envelope (never throws for user input).
[[nodiscard]] editor::contract::Envelope run_set(const std::vector<std::string>& positionals,
                                                 const std::map<std::string, std::string>& flags);

// `context query --overrides <mode> <scene>` — advisory override hygiene. `mode` is the value of the
// `--overrides` flag (diverged|redundant); the scene is positionals[0]. Returns the result envelope.
[[nodiscard]] editor::contract::Envelope
run_override_query(const std::vector<std::string>& positionals,
                   const std::map<std::string, std::string>& flags);

} // namespace context::cli

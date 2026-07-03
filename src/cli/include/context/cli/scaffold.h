// Scaffold: `context new`'s runnable default template (R-QA-006 MUST half).
//
// R-QA-006 requires `context new`'s DEFAULT template to yield a RUNNABLE skeleton — a scene,
// a camera, and a startable session such that the first query/step after `context new` succeeds
// without error. This module writes that template to disk and then PROVES it is runnable by loading
// it into a real context_kernel session, populating the World from the scene, and stepping the
// Scheduler once — the concrete point at which the CLI consumes the microkernel. The atomic file
// writes are plain std::filesystem here; they route through filesync's atomic IO once integrated.

#pragma once

#include "context/editor/contract/envelope.h"

#include <string>
#include <vector>

namespace context::cli
{

// The list of template names `context new` accepts. M1 ships only the runnable default.
[[nodiscard]] const std::vector<std::string>& template_names();
[[nodiscard]] bool is_known_template(const std::string& name);

// A dry-run description of what scaffolding `directory` with `template_name` WOULD write (no I/O).
[[nodiscard]] editor::contract::Json scaffold_plan(const std::string& directory,
                                                   const std::string& template_name);

// Write the template into `directory` (creating it + subdirs), then verify it is RUNNABLE by
// booting a context_kernel session over the scaffolded scene and stepping once. On success the
// envelope's data reports {directory, files[], entities, cameras, ticks}; on any failure
// (bad template, write error, non-runnable scaffold) it carries the matching R-CLI-008 code.
[[nodiscard]] editor::contract::Envelope scaffold_project(const std::string& directory,
                                                          const std::string& template_name);

// Load an already-scaffolded project directory into a fresh kernel session and step it once,
// returning an ok envelope with {entities, cameras, ticks} when the first query/step succeeds. This
// is the "startable session" proof, factored out so a test can run it against a scaffold.
[[nodiscard]] editor::contract::Envelope verify_runnable(const std::string& directory);

} // namespace context::cli

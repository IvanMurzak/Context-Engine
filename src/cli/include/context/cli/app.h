// App: the `context` CLI front end — parses the R-CLI-007 verb grammar, dispatches against the
// single registry (R-CLI-009), and returns the uniform R-CLI-008 result envelope.
//
// Grammar: `context [<ns>:]<noun> <verb> [--flags] [positional args]`. Global verbs (describe, new)
// take the one-token `context <verb>` form; noun-scoped verbs (package add) take two. The core flag
// set (R-CLI-007) is honored by every verb. The App does NO I/O beyond what a verb needs;
// main() prints the returned envelope as JSON and exits with envelope.exit_code().

#pragma once

#include "context/editor/contract/envelope.h"

#include <string>
#include <vector>

namespace context::cli
{

// Parse + dispatch the argument vector (everything AFTER the program name). Returns the result
// envelope; never throws for user-input errors (they become failure envelopes with catalog codes).
[[nodiscard]] editor::contract::Envelope run(const std::vector<std::string>& args);

// Convenience for main(): adapt argc/argv, run(), print the envelope JSON to stdout, and return the
// process exit code.
[[nodiscard]] int main_cli(int argc, char** argv);

} // namespace context::cli

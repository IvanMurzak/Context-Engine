// `context session <verb>` — the headless session-control CLI backend (R-QA-005 / L-54, issue #74).
//
// Drives the deterministic headless session (src/runtime/session/) over a persisted session-state
// file: new / step / seed / inject / hash / record. Each is a one-shot invocation that loads the
// state, applies the verb, and (for mutating verbs) writes it back — so the separate CLI calls
// compose into a stateful session. The result envelope reports the monotonic simTick (R-CLI-016) and
// the hierarchical state hash. Hash values are emitted as hex strings because the R-CLI-008 envelope
// DOM stores numbers as doubles (which cannot hold a full 64-bit hash); the on-disk state file +
// replay artifact keep exact 64-bit integers through the serializer DOM.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Dispatch `context session <verb> <state> [--flags]`. `verb` is one of new/step/seed/inject/hash/
// record; `bound` carries the positional `state` path; `flags` the parsed flags.
[[nodiscard]] editor::contract::Envelope run_session(const std::string& verb,
                                                     const std::map<std::string, std::string>& bound,
                                                     const std::map<std::string, std::string>& flags);

} // namespace context::cli

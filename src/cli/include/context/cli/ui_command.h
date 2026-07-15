// `context ui <verb>` — the headless runtime-UI drive/assert CLI backend (M7 T5 / a5, R-UI-006 /
// R-CLI-008/009, issue #223).
//
// The "driven/asserted headless via CLI" exit leg: one-shot verbs over a UI-scene file (the ctx:ui-hud
// few-shot form the M7 T4 context.ui surface authors) built onto the pure-stdlib context_ui package —
// dump the retained tree + computed rects, query/address a node by author name, send a synthetic event
// (the UI->state action path runs on dispatch), and assert a tree fact fail-closed. Each is a one-shot
// invocation that loads the scene, runs the headless layout pass, applies the verb, and reports the
// R-CLI-008 envelope; the scene file is never mutated (a drive-and-observe surface, unlike the session
// state file). No GPU, no daemon — fully CI-assertable.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Dispatch `context ui <verb> <scene> [<node>|<event>] [--flags]`. `verb` is one of
// dump/query/send/assert; `bound` carries the positional `scene` (+ `node`/`event`) paths; `flags` the
// parsed flags.
[[nodiscard]] editor::contract::Envelope run_ui(const std::string& verb,
                                                const std::map<std::string, std::string>& bound,
                                                const std::map<std::string, std::string>& flags);

} // namespace context::cli

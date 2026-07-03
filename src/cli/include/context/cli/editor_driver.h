// The `context editor …` CLI-local operational command family — a headless in-process driver over the
// composed EditorKernel (context_editorkernel).
//
// This is deliberately NOT a contract registry verb (it is not part of the CLI ≡ RPC ≡ MCP surface,
// R-CLI-009) — it is an operational/diagnostic command, the CLI analogue of a self-check, that boots a
// composed EditorKernel in-process and drives the M1 attach path end-to-end so the whole loop is
// runnable from a terminal:
//   context editor smoke [--project <dir>]
// boots the daemon, attaches a client over the handshake, performs a CLI-verb edit (through filesync
// atomic-IO) and a raw edit, and reports the derived World headless — returning the R-CLI-008 envelope.
//
// The cross-process contract verbs (the reserved file-rewriter `set` over the bridge) stay reserved
// until the native FileStore + bridge transport land; this command exercises the SAME composed public
// API in one process today.

#pragma once

#include "context/editor/contract/envelope.h"

#include <string>
#include <vector>

namespace context::cli
{

// `args` are the tokens AFTER the leading `editor` selector (e.g. {"smoke", "--project", "/tmp/p"}).
// Returns the result envelope; never throws for user-input errors (they become failure envelopes with
// catalog codes).
[[nodiscard]] editor::contract::Envelope run_editor(const std::vector<std::string>& args);

} // namespace context::cli

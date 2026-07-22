// `context attach` — the cross-process client that attaches to a running daemon OVER THE WIRE
// (R-BRIDGE-002 / R-CLI-010). A CLI-local operational command (the cross-process analogue of
// `context editor smoke`, which drives the SAME composed loop in one process).
//
//   context attach --project <dir> [--set-path <p>] [--set-content <c>] [--out <file>] [--shutdown]
//   context attach --project <dir> --editor-session
//                  [--editor-select <id,id,…>] [--editor-select-mode <replace|add|toggle|remove>]
//                  [--editor-play <play|pause|stop|step>] [--editor-ticks <n>] [--shutdown]
//
// discovers the daemon via `<dir>/.editor/instance.json` (R-ARCH-005), connects over the loopback
// transport, performs the capability-negotiation handshake, then — as a SEPARATE process from the
// daemon — issues a file-rewriter `edit` and a derived-World `query` (read-your-writes barrier), and
// reports the result envelope. `--shutdown` asks the daemon to exit after the drive (clean teardown).
//
// M9 e08a — the EDITOR SESSION mode (D7 tier 1): with any `--editor-*` flag the edit/query drive is
// replaced by the daemon session-state drive, which is what makes the CLI a first-class SECOND
// CLIENT of the human's session (`context daemon` holds selection / cameras / play; this reads and
// drives them over the same contract any agent uses). The reply reports `clientId` — this
// connection's echo-suppression identity, the value the `session` topic stamps as `origin`.

#pragma once

#include "context/editor/contract/envelope.h"

#include <string>
#include <vector>

namespace context::cli
{

// `args` are the tokens AFTER the leading `attach` selector. Returns the result envelope (ok only when
// the full cross-process attach → edit → query drive succeeded and the derived world reflected the
// edit); never throws for user-input / transport errors (they become failure envelopes).
[[nodiscard]] editor::contract::Envelope run_attach(const std::vector<std::string>& args);

} // namespace context::cli

// `context fetch` — the R-CLI-017 large-result fetch: retrieve an oversized result payload from a
// RUNNING daemon by its transport-portable opaque handle, over the SAME channel the handle came
// from (`resource.read { handle, range }` chunks, reassembled client-side).
//
//   context fetch <handle> --project <dir> [--range <offset>:<length>] [--out <file>]
//
// `<handle>` is the `data.largeResult.handle` URI a prior oversized response returned. Discovery,
// connect, and the attach handshake reuse the shared wire plumbing (wire_client.h). Without
// --range, every chunk is fetched and reassembled: the result envelope's data is the ORIGINAL
// (previously spooled) result — exactly what the daemon would have returned inline. With --range,
// exactly one resource.read is issued and the raw read result (offset/length/total/eof + chunkHex)
// is returned instead. This is the registry-backed `resource read` verb's CLI backing; `fetch` is
// its registry-owned alias (R-CLI-017 naming).

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// `handle_uri` — the opaque resource URI (required). `flags` — the resolved verb flags from the
// registry dispatch (`project` required; `range` optional "offset:length"; `out` optional result
// sink). Never throws for user-input / transport errors (they become failure envelopes).
[[nodiscard]] editor::contract::Envelope
run_fetch(const std::string& handle_uri, const std::map<std::string, std::string>& flags);

} // namespace context::cli

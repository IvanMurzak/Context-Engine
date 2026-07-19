# Daemon multi-client fan-in + attach-token auth (M9 e01, D19/D20)

How this repository implements the M9 design decisions **D19** (bounded N-client concurrent fan-in)
and **D20** (attach-token authentication) in the EditorKernel daemon. Companion to the owner's design
authority (`.claude/design/context-engine/m9-editor/` §§01/05/08 and `R-BRIDGE-*`/`R-SEC-002`/`L-50`);
this file records the implementation, not the design.

Seams: `src/editor/bridge/` (transport, dispatcher, event stream, scope) +
`src/editor/editorkernel/kernel_server.cpp` (the serve loop) + `src/cli/src/daemon_command.cpp`
(the `context daemon` flags). Backed by `editorkernel-test_kernel_server`,
`bridge-test_dispatcher`, `bridge-test_transport`, `bridge-test_event_stream`.

## D19 — concurrent fan-in (replaces the serial M1 transport)

The M1 transport served **one client at a time to disconnect**. M9 accepts **N concurrent
connections**, but **the mutation model stays single-threaded (L-50)** — concurrency lives at the
transport, never the write queue:

- **One thread per connection.** `KernelServer::serve()` accepts up to `max_connections()` clients;
  each gets a fresh `bridge::Session` + one thread that owns its handle for BOTH reads and writes. It
  interleaves a bounded **timed read** (`TransportConnection::read_frame_timed` — POSIX `poll`,
  event-driven with no added request latency; Windows `PeekNamedPipe`+short sleep) with flushing its
  outbound queue. A single connection thread never does a concurrent read + write on one OS handle —
  which would deadlock a synchronous Windows named-pipe file object.
- **One dispatch mutex serializes everything.** Every `dispatcher().handle()` + the large-result
  spool + the event fan-out run under ONE mutex, so requests from N clients **serialize correctly**
  (interleaved edits can never race). This is the L-50 write-serialization guarantee, preserved.
- **Event push fan-out.** After each dispatch, newly-published events (`files`/`derivation`/
  `diagnostics`/`session`/`clients`/`log`) are fanned out to every subscribed connection's bounded
  outbound queue and pushed as JSON-RPC **notification** frames (`{"method":"event","params":{subId,
  event}}`). Only a client that has `subscribe`d receives pushed frames — a one-shot request/response
  client (today's CLI) is byte-for-byte unaffected, which is what keeps D19 additive.
- **The `clients` topic fires on attach AND detach** (an observer sees who is attached).
- **Bounds (config, sane defaults).** `--max-clients` (default 16) caps concurrent connections; the
  (N+1)th attach is refused **`daemon.busy`** (transient — a slot frees on detach), never served.
  `--max-frame-budget` (default 256) bounds each connection's queued *event* frames: a slow client
  that stops reading overflows its budget, its events are dropped, and it is sent an **`event.gap`**
  re-snapshot marker (R-BRIDGE-008) — the daemon **never stalls** on a slow client. Responses are
  never dropped.

### Slow-client gap contract

A subscribed client that stops reading its socket cannot back-pressure the daemon: `enqueue_event`
drops past the per-connection budget and records `gap_owed`; the next fan-out delivers a single
`event.gap` frame once the queue drains. The client's correct response is to re-snapshot (re-`subscribe`).
The subscription's own bounded queue (`event_stream.h`) is the second-level backstop (`reset_sub_gap`
after signalling).

## D20 — attach-token authentication, sequenced behind a compat flag

The handshake gains an **OPTIONAL `token` field** (`ClientHandshake::token`) — additive under the
frozen `protocolMajor = 1` (a client that omits it is wire-identical to a pre-D20 client). The daemon
writes a per-instance token into `.editor/instance.json` (0600 on POSIX; owner-SID DACL on Windows —
see below) and can verify a client's token against it at attach; a wrong/missing token is refused
with **`attach.denied`** (permission class) via the uniform error catalog. Enforcement lives in the
**dispatcher** (`Dispatcher::configure_attach_auth` → checked in `attach()`, BEFORE protocol
negotiation so an unauthenticated caller learns nothing about the surface), never an adapter.

### The C-F1 sequencing — and its completion in e02 (enforcement is now DEFAULT ON)

Attach-token enforcement is a **behavioral tightening** (not additive for a tokenless client), so it
was sequenced honestly per **C-F1** rather than flipped in one step:

- **e01: enforcement DEFAULT OFF.** `context daemon` verified nothing unless `--require-attach-token`
  was passed; the token rode the wire but was never gated on, leaving the M1 ambient-OS-guard trust
  model (POSIX 0600 socket + instance file; Windows owner-SID pipe DACL) as the boundary. That kept
  e01 non-breaking for the then-tokenless CLI.
- **e02: the CLI migrated onto the client SDK** (`context_client`), which discovers
  `.editor/instance.json` and presents the token on every attach — and, in the SAME task, the
  **enforcement default flipped ON**. This is why the flip is safe rather than a flag day: the CLI was
  the only existing client, no external releases exist, and it migrated in the same change that
  tightened the daemon.

**Current behavior (since e02).** `context daemon` refuses any attach whose `token` is absent or does
not match the one it published, with `attach.denied`. A rogue local process that cannot read the
0600/DACL-protected `instance.json` therefore cannot attach at all — the file's confidentiality is now
load-bearing, not merely advisory.

**Escape hatch: `--no-require-attach-token`.** It restores the pre-e02 posture (token carried, never
gated; ambient OS guard only). It exists for bisecting an auth-suspected regression and for an
out-of-tree client not yet on the SDK — **not** as a supported deployment mode. The legacy
`--require-attach-token` is still accepted as a no-op affirmation of the default so existing scripts
keep working; if both are passed, the explicit opt-out wins.

The live assertion is `client-test_client_e2e` (`src/editor/client/tests/test_client_e2e.cpp`): against
a REAL daemon it proves a tokenless attach and a wrong-token attach are both refused as daemon-side
rejections, while the discovered token attaches cleanly.

### Windows named-pipe owner-SID DACL (closing the documented gap)

The M1 Windows pipe was created with the default DACL. M9 builds a security descriptor whose sole ACE
grants the current process user's SID full access (`src/editor/bridge/src/transport.cpp`,
`make_owner_only_security`) and applies it to **every** pipe instance, so another OS user's process
cannot open the pipe. POSIX keeps the 0600 socket + instance file. `TransportServer::endpoint_owner_restricted()`
asserts owner-only access (the Windows DACL ACE walk / the POSIX 0600 mode) and is exercised by
`bridge-test_transport`.

## Registry / contract impact

None to the verb surface: the `token` handshake field is additive, and the `event`/`event.gap` push
frames are JSON-RPC notifications, not registry verbs — so R-CLI-013 parity is unchanged (no new verb,
no verb removed/renamed). Two additive error-catalog codes were promoted (`attach.denied`,
`daemon.busy`), which the additive-only R-CLI-008 catalog gate permits.

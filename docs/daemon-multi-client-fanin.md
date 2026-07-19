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

### The compat flag is DEFAULT OFF in e01 — and the e02 flip plan

Attach-token enforcement is a **behavioral tightening** (not additive for a tokenless client), so it
is sequenced honestly per **C-F1**:

- **e01 (this task): enforcement DEFAULT OFF.** `context daemon` runs with verification **off** unless
  `--require-attach-token` is passed. With it off, the token is carried on the wire but never gated
  on — the v1 ambient-OS-guard trust model (POSIX 0600 socket + instance file; Windows owner-SID pipe
  DACL) is the boundary, exactly as M1. This keeps e01 non-breaking for the existing tokenless CLI
  (the only client today; no external releases exist).
- **e02: the CLI migrates onto the client SDK** (`context_client`), which reads
  `.editor/instance.json` and sends the token on attach. **Once every first-party client sends a
  token, enforcement defaults ON** (drop the `--require-attach-token` opt-in; the daemon requires a
  valid token, and `--allow-anonymous`-style escape hatches, if any, become the opt-*out*). At that
  point a rogue local process that cannot read the 0600/DACL-protected `instance.json` cannot attach.

Until e02 flips it, operators who want the tightened posture early can pass `--require-attach-token`
today; every first-party client must then send `token` from `instance.json` or be refused
`attach.denied`.

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

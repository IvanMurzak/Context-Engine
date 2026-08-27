---
id: e01-daemon-fanin-auth
title: Daemon multi-client concurrent fan-in (D19) + attach-token auth with owner-SID DACL (D20), compat-flag sequenced
group: A
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 9
complexity: 8
security_critical: true
production_touching: false
model_hint: top
taskflow_refs: [01, 05, 08]
---

## Goal

Replace the serial single-connection M1 transport with bounded N-client concurrent fan-in
(D19) and enforce attach-token authentication (D20) — the prerequisite for GUI + CLI + agents
attached simultaneously. Enforcement lands behind a daemon compatibility flag, default OFF,
per the C-F1 sequencing (e02 flips it ON after the CLI migrates).

## Scope & seams

- **Fan-in** (`src/editor/bridge/`): `TransportServer` accepts N concurrent connections
  (today: one client served to disconnect — `transport.h:14-17`, `kernel_server.h:37`);
  per-connection `Session` carries scopes + ack cursors; ALL requests funnel into the existing
  single-threaded dispatch/write queue — **L-50 serialization preserved; concurrency lives at
  the transport, never the mutation model** (`kernel_server.h:70-82`).
- **Event push fan-out**: wire the existing per-subscriber bounded queues + gap markers
  (`event_stream.h:59-94`) to N live connections (push frames, not poll-only); ack-based
  retention vs slowest cursor stands; a stuck client hits the gap-marker path, never stalls
  the daemon.
- **`clients` topic fires** on attach/detach (already registered — `registry.cpp:956-1010`).
- **Bounds**: max connections + per-connection frame budgets (config with sane defaults).
- **Auth (D20)**: handshake gains an OPTIONAL `token` field — additive under frozen
  protocolMajor=1 (`handshake.h:40-44` today has none); daemon verifies against
  `.editor/instance.json`'s token (`daemon_command.cpp:95-122`); wrong/missing →
  `attach.denied` via the uniform error catalog. Enforcement behind a compat flag,
  **default OFF** in this task.
- **Windows named pipe owner-SID DACL** (closing the documented gap); POSIX keeps 0600
  socket + instance file.
- Scope checks stay dispatcher-first (`dispatcher.cpp:203` model) — untouched.

## Definition of Done

- [ ] T1: N concurrent clients — interleaved requests serialize correctly; fan-out reaches
      all subscribers; slow-client gap-marker path exercised; bounds enforced
- [ ] T1: auth accept + deny paths (`attach.denied`), flag OFF/ON behaviors both tested
- [ ] Windows: DACL asserted (owner-only access); POSIX: 0600 preserved
- [ ] Registry/parity CI green (handshake addition is additive; no verb changes)
- [ ] Compat-flag default OFF + the e02 flip plan documented in the daemon docs
- [ ] 3-OS CI green incl. sanitizer legs (TSan on the fan-in paths)

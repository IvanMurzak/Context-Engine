---
id: e02-client-sdk-boundary
title: context_client SDK (installed/exported) + subscription consumer + boundary CI (D10) + CLI migration; token enforcement defaults ON
group: A
sequence: 2
repo: "."
base_branch: "main"
depends_on: [e01-daemon-fanin-auth]
importance: 9
complexity: 8
security_critical: true   # flips attach-token enforcement default ON (C-F1 step 3)
production_touching: false
model_hint: top
taskflow_refs: [01, 02, 05]
---

## Goal

Create `context_client` — the first installed/exported client-SDK library (endpoint discovery,
attach, typed helpers, and the subscription consumer every live client needs) — migrate the
CLI onto it (one implementation), stand up the D10 boundary CI job, then flip attach-token
enforcement default ON (C-F1 completes).

## Scope & seams

- **New `src/editor/client/` → `context_client`** (C++ static lib): lift the CLI's
  `wire_client.h/cpp` plumbing; connect/attach (token, scopes, protocol negotiate); typed
  request helpers; links `context_bridge` (transport types) + `context_contract` only.
- **Subscription consumer** (the missing piece — 01 §3): maintain N topic subscriptions;
  snapshot-then-delta application (`event_stream.h:135-197` protocol); ack cursor management;
  gap → automatic re-snapshot; reconnect-with-backoff; incarnation-epoch handling (fresh
  snapshot on daemon restart).
- **CMake install/export** (A-F1): today root `src/CMakeLists.txt:284-288` installs only the
  `context-hello` demo + `context_kernel`; add `context_client` + contract headers to the
  R-VER-004 layout as exported artifacts.
- **Generated client schema**: build-time artifact generated from `describe` (registry is the
  single source of truth; hand-written typings prohibited — R-CLI-009 spirit). e05's JS client
  consumes it.
- **CLI migration**: `src/cli/` rides `context_client` (delete duplicated plumbing); all
  existing CLI tests stay green — the CLI is the only existing client, which is what makes
  the enforcement flip safe.
- **`editor-boundary` CI job** (D10): out-of-tree consumer build against INSTALLED artifacts
  + include-graph check forbidding kernel-internal headers. Starts with a minimal consumer
  smoke; the job's consumer target grows to the full editor as e04/e05 land (e17 asserts the
  final shape).
- **Token enforcement default ON** (C-F1 step 3): flip e01's compat flag; document the
  escape hatch.

## Definition of Done

- [ ] `context_client` installs/exports; out-of-tree consumer builds against it in the new
      `editor-boundary` CI job (include-graph check blocking)
- [ ] T1: subscription-consumer protocol — snapshot/delta, ack, gap→re-snapshot, reconnect
      backoff, incarnation epoch — all covered with wire mocks + against a live daemon
- [ ] CLI fully migrated; entire existing CLI test suite green; no duplicate wire plumbing
- [ ] Generated schema artifact produced from `describe` in the build; drift-checked in CI
- [ ] Attach-token enforcement default ON; tokenless attach denied (T1 assert)
- [ ] 3-OS CI green; fleet-manifest row for `editor-boundary` if registered as a gate

# `context_client` — the client SDK and the D10 boundary (M9 e02)

How this repository implements the M9 target architecture's first structural claim: **the editor is
an ordinary client**. It talks the public contract (JSON-RPC + events) and never links kernel
internals, which is what makes alternative editors possible by construction and keeps the daemon the
single authority.

Before e02 that claim had no teeth. There was **no client SDK at all** — the only client-side wire
code was the CLI's `wire_client.h/cpp`, a one-shot request/response helper with no subscription
support, and nothing was installed or exported, so an out-of-tree consumer would have had to copy it.
A boundary that is only described is a boundary that erodes.

## What ships

`src/editor/client/` → **`context_client`** (C++20 static library, the first installed/exported
artifact). It links `context_bridge` (transport types) and `context_contract`
(envelope/json/handshake/registry) — and nothing else.

| Header | Role |
|---|---|
| `instance.h` | R-ARCH-005 discovery: read `<project>/.editor/instance.json` (endpoint, D20 token, protocol major), retrying the daemon's publish race. A client never recomputes the endpoint. |
| `wire.h` | JSON-RPC 2.0 request framing + inbound-frame classification (`response` / `event` / `event.gap` / `unknown`), and the `WireChannel` seam. |
| `client.h` | `Client`: connect, the R-CLI-010 attach handshake (token + scopes + protocol negotiate), typed calls, reconnect. |
| `subscription.h` | `SubscriptionConsumer`: the R-CLI-015 client half — the piece every live client needs. |
| `schema.h` | The generated client schema, projected from the contract registry's `describe`. |

### Why there is a `WireChannel` seam

A live client receives two frame shapes on one connection: responses to its own requests and
server-pushed `event` / `event.gap` notifications (the D19 fan-out). So `bridge::TransportClient::request()`
— which assumes the next inbound frame *is* the response — is wrong for a subscribing client, and its
own header says so. `Client::call()` therefore sends, then reads until it sees **its** response id,
parking every event frame it passes on a queue the consumer drains.

`WireChannel` abstracts that wire so the consumer's protocol is drivable over a scripted mock
(`tests/mock_channel.h`) with no daemon, no sockets, and no timing races — and over the real
transport in the e2e test. Both are required: the mock proves the state machine, the live test proves
the mock and the daemon agree.

## The subscription consumer

Five behaviors, each of which a hand-rolled client gets subtly wrong:

1. **Snapshot-then-delta.** `subscribe` returns the current-state snapshot (plus, on a resume, the
   replayed catch-up). A fresh snapshot RESETS the cursor to its `lastSeq`: events at or below it are
   already represented by the snapshot, so applying them again double-counts.
2. **Ack cursor management.** Ring retention is defined relative to the **slowest acked cursor**
   (R-CLI-015), so a consumer that never acks silently pins the daemon's memory. Acks go out on a
   cadence (`ack_interval`) and at every recovery point; `flush_acks()` forces the remainder.
3. **Gap → automatic re-snapshot.** An `event.gap` notification means events were dropped for this
   connection. The only correct recovery is a fresh snapshot for **every** subscription — never
   "keep going from where we were".
4. **Reconnect with backoff.** A dropped wire re-dials on a bounded exponential backoff
   (`BackoffPolicy`, a pure function so it is testable without waiting), re-attaches with the same
   token/scopes, and resumes each subscription from its cursor. A `gapped:true` verdict on the resume
   falls back to the snapshot.
5. **Incarnation epoch.** A daemon restart mints a new `incarnationId` and restarts the seq space.
   Carrying a cursor across that boundary would silently swallow the new lifetime's entire stream, so
   an incarnation change forces a fresh snapshot — whether it surfaces mid-stream or across a
   reconnect.

`SubscriptionStats` exposes counters for each (`snapshots_taken`, `gaps_recovered`, `reconnects`,
`acks_sent`, `incarnation_changes`) so the protocol's behavior is assertable rather than inferred
from side effects.

## The generated client schema

Hand-written client typings are prohibited (R-CLI-009 spirit): the registry is the single source of
truth. `context_client_schema_gen` runs as a **build step** and emits
`context-client-schema.json` — a client-facing projection of `describe` carrying `protocol`,
`rpcMethods`, `eventTopics`, `eventEnvelope`, `subscription`, `errorCatalog`, `largeResult`,
`queryLanguage`, `fileKinds`, `componentTypes`, and `deprecationPolicy`. The CLI verb grammar and the
MCP tool surface are deliberately **excluded**: they describe other doors onto the same registry, and
carrying them would make every unrelated CLI change look like client-contract drift.

The artifact installs to `<prefix>/versions/<semver>/share/context/`, which is what e05's JS client
generates from. A committed copy lives at `src/editor/client/schema/context-client-schema.json` and
the `client-test_schema` ctest fails on any byte difference — **change the contract, rebuild, commit
the regenerated file**. That gate is the whole reason a generated artifact beats a transcribed one.

## Consuming it out of tree

```cmake
find_package(ContextClient REQUIRED)
target_link_libraries(my_editor PRIVATE Context::context_client)
```

`Context::context_client` is the entire public surface. The package config is hand-authored rather
than produced by `install(EXPORT)` on purpose: a static library records even its PRIVATE deps in its
interface, so `install(EXPORT)` would publish `context_schema` / `context_component` /
`context_warnings` as part of the package — exactly what D10 forbids. Those archives ARE installed
(a static library carries none of its dependencies' objects, so the whole link closure must reach the
final link) but their **headers are not**, so they are invisible to a consumer's include graph. That
asymmetry is the boundary: link what the implementation needs, expose only what the contract publishes.

Installed header modules — the published surface, in full:

- `context/editor/client/` — the SDK
- `context/editor/bridge/` — transport + event-stream types its headers name
- `context/editor/contract/` — envelope / json / handshake / registry
- `context/kernel/` — `event_bus.h` (the bridge forwards `kernel::LogEvent`)

## How the boundary is enforced (the `editor-boundary` CI job)

Blocking, per-PR, ubuntu (`docs/ci-fleet-manifest.json` → gate `editor-boundary`). Two halves:

1. **A real out-of-tree consumer.** `cmake --install` the exported closure to a staging prefix, then
   configure `src/editor/client/consumer/` **standalone** against it — `find_package(ContextClient)`
   with no path into the engine source tree — build it under the consumer's own
   `-Wall -Wextra -Werror`, and run it. A missing installed header, an incomplete link closure, or a
   public header that is not warning-clean for someone else fails on the PR that caused it.
2. **The include-graph check** (`tools/check_include_graph.py`). Walks the INSTALLED headers
   **transitively** and rejects any include that reaches a kernel-internal module or resolves to a
   header we did not ship. Transitivity is what makes it strong: a public header cannot quietly reach
   an internal one through intermediate hops, because every hop must itself be installed, and the
   installed set is the allowlist.

The job deliberately does **not** re-run the `client-*` ctests — they are a plain test family
auto-run by `ctest --preset dev` on all three `build` legs, so duplicating them would add a
hand-maintained `--target` list to keep in sync for no extra signal (the "Not Run = RED" tripwire).

The consumer target grows as e04/e05 land; e17 asserts its final shape.

## The CLI rides the same code

`context attach` and `context fetch` were migrated onto `context_client` in the same task, and the
duplicated plumbing (`src/cli/wire_client.{h,cpp}`) is gone — what remained of it, pure argv parsing,
is now `src/cli/args.h`. That matters twice over: it is what makes the CLI a meaningful proving
ground for the SDK (the whole existing CLI suite exercises it), and it is what made flipping D20
attach-token enforcement to **default ON** safe in the same change — the only existing client
migrated alongside it. See `docs/daemon-multi-client-fanin.md` § D20.

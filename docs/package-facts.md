# The package fact bus (editor-UX D4 + D5)

How two independently-authored packages exchange facts **without depending on each other**: package A
declares a topic in its own manifest and publishes a **state** onto it; package B declares an interest
in that topic and, once the operator has consented, receives every value. Neither names the other's
code, neither loads the other, and either can be uninstalled without the other noticing more than a
topic that stopped moving.

This is the tier-1 counterpart of [`editor-ui-bus.md`](editor-ui-bus.md). Package facts are **daemon**
facts on purpose: a fact on the window-local `editor.ui` bus is invisible to the CLI, to an agent and
to a second window, and the `editor.ui` built-in topic set stays closed at nine.

## The shape in one screen

```
a panel  --bridge.facts.publish-->  editor-core (packagefacts.ts)
                                      |
                                      v  panel.facts.publish        <- the ONE door
                                    Shell (package_facts.h)
                                      |  "did THAT package declare THAT topic?"  (events.publishes[])
                                      v  events.publish             <- on the package's own session
                                    daemon (bridge/event_stream.h)
                                      |  grammar . registry . RETAIN . DEDUP . reentrancy . bounds
                                      v
                                    every subscriber's own stream
                                      |
                                      v  panel.events.poll (unchanged, e13c-2)
                                    the consuming package's frames
```

**Delivery reuses the shipping path end to end** — the package's baseline daemon session, the bounded
per-package Shell buffer (`package_events.h`), `panel.events.poll`, `PackageEventPump`, and the loud
`dropped` / `gapped` pair. There is no new transport and no second poll surface. Publishing is a
request/response call; delivery is the subscription that already existed.

## D5 — a package fact is a STATE, not an edge

This is the load-bearing rule, and it is what stops the architecture shipping an unbounded loop.

Two packages hold **separate** baseline daemon sessions and therefore **different** `origin`s, so the
origin echo suppression that protects `selection` and `camera` does nothing at all for an A → B → A
mirror: every hop looks foreign to the next. The rule that actually breaks the cycle is the *other*
one the session state already relies on — **"a no-op publishes nothing"** — which needs daemon-side
state to compare against, and an arbitrary package topic had none.

1. **Last-value retention + dedup.** The daemon holds the current value per topic and refuses a
   repeat: `events.publish` answers `changed:false` and emits nothing — no seq, no ring entry, no
   subscriber delivery. A mirroring pair converges after exactly one round.
   The comparison is over a **canonical** (sorted-key) serialization, so a producer that builds its
   payload from an unordered container cannot defeat the dedup by accident.
2. **Snapshot-on-subscribe falls out of retention.** `subscribe`'s snapshot carries
   `packageFacts: [{topic, payload}, …]`, so a panel mounted mid-session reads current state instead
   of drawing nothing until the next change. No subscribe-then-separately-ask race.
3. **Reentrancy is refused, with a diagnostic.** A publish issued from inside an event handler is
   refused `package.fact_reentrant`, which turns the remaining loop shape into something an author can
   see and fix rather than a hang.

**The accepted cost, stated plainly:** a package **cannot** send pure edge events. "The button was
pressed twice" is not expressible — the second publish deduplicates against the first. Model an edge
as state (a counter, a token). This was taken knowingly as the price of a broadcast bus that cannot be
made to loop. `changed:false` is therefore a **success**, not a refusal; a publisher that treats it as
one will build a retry loop around the cycle breaker itself.

## Where each control lives, and why

The mechanism is split across two layers by exactly one question: **who has read a manifest.**

| Control | Layer | Why there |
|---|---|---|
| Topic grammar (`<pkg>.<name>`, ≥ 2 lowercase dotted segments) | daemon | Every contract-owned topic (`files`, `derivation`, `diagnostics`, `session`, `clients`, `log`) is ONE bare segment, so the two-segment floor makes forging a core fact structurally impossible — no deny-list to keep in step with `Registry::topics()`. |
| The topic **registry** (`events.declare`) | daemon | Deny-by-default: publishing never creates a topic, and `describe` enumerates what is registered (R-CLI-013 parity). |
| Retention / dedup / snapshot / reentrancy | daemon | The bus IS stream state; `subscribe`'s snapshot then gets rule 2 for free. |
| Bounds (256 topics, 128-byte names, 64 KiB per retained fact) | daemon | The surface is reachable from untrusted code in a process that stays up for days. |
| **"did THAT package declare THAT topic"** (`events.publishes[]`) | Shell | The daemon has never read a manifest and never will. |
| **The consented cross-package subscription** | Shell | The `~/.context/package-grants.json` document lives here. |

Both halves are real controls and neither is redundant: strip the Shell and any client could publish
on any registered topic; strip the daemon and a package could publish a `session` fact by spelling one.

### Why `panel.facts.publish` is its own route

`events.publish` is deliberately **not** on `panel_callable_daemon_methods()`. `bridge.call` forwards
its `method` **and** `params` verbatim, so an allowlist entry would let a panel publish on any
registered topic — another package's included — with the Shell's declaration check standing *beside*
the path rather than *on* it. R-SEC-007's "adapters are bypassable" applies to our own adapters too.
The allowlist stays closed and `panel.facts.publish` is the one door.

### The grant: one token, two clamps

Subscribing to **another** package's topic requires the `package_events` capability
(`extension.h`) in the same operator-owned document `package_grants.h` already owns — no new file, no
new consent surface. Like `ui_events` it maps to **no daemon scope**, so granting it can never widen a
package's daemon session.

Two clamps, both load-bearing:

- the **capability** must be granted (and the grant is already clamped to what the manifest declared);
- the **topic** must appear in that package's `events.subscribes[]`.

The token alone would let a consented package subscribe to a topic it never declared an interest in;
the manifest alone would make consent decorative. A package's **own** topics need no grant and no
prompt — its own fact is not somebody else's data.

### Why the grant needs the delivery filter to bind

`subscribe` with an **empty** topic list means *every* topic, and that request names nothing to refuse.
So the consent gate is applied **twice**:

- in `PackageSessionHost::forward`, refusing a `subscribe` that NAMES an unconsented topic (this is
  the half that produces a good diagnostic, `panel.daemon.topic_not_granted`); and
- in `PackageSessionHost::pump`, dropping any package-namespaced event the package is not entitled to
  **before** it reaches the buffer (this is the half that actually binds).

A policy-dropped fact is **not** a gap: `gapped` means "your cursor is worthless, re-snapshot", and a
package that was never entitled to a topic has lost nothing it could act on. Drops are counted
(`events_filtered()`) for a human, and are invisible to the package.

## The wire

| Method | Layer | Params | Answer |
|---|---|---|---|
| `events.declare` | daemon (operational, read/query) | `{topics: [...]}` | `{topics, registered}` — idempotent, all-or-nothing |
| `events.publish` | daemon (operational, read/query) | `{topic, payload}` | `{topic, changed, seq}` |
| `panel.facts.publish` | Shell router | `{packageId, topic, payload}` | the daemon's answer, verbatim |
| `bridge.facts.publish` | panel port | `{topic, payload}` | the daemon's answer, verbatim |

`protocolMajor` stays **1** — both daemon verbs are additive and `operational`, so neither joins the
frozen v1 surface.

**Both daemon verbs sit on the read/query baseline, and that is a decision.** A package's session
attaches at the deny-all baseline, so a higher classification would make the bus reachable only by a
package the operator had *also* granted `session_control` — the authority to drive play state and the
human's selection, bought in exchange for a broadcast fact.

### The refusals

| Code | Layer | Means |
|---|---|---|
| `panel.facts.topic_not_namespaced` | Shell | The topic is not under your own package id. |
| `panel.facts.topic_not_declared` | Shell | **Your manifest** does not claim it in `events.publishes[]`. |
| `panel.daemon.topic_not_granted` | Shell | You subscribed to another package's topic without consent. |
| `package.topic_invalid` | daemon | Bad grammar, or a contract-owned (single-segment) name. |
| `package.topic_undeclared` | daemon | **No package registered it on this daemon** (a load state, not a manifest one). |
| `package.topic_capacity` | daemon | The registry is full. |
| `package.fact_too_large` | daemon | Over the per-topic retained-payload ceiling; nothing retained. |
| `package.fact_reentrant` | daemon | Published from inside an event handler (D5 rule 3). |

The pairs are not redundant: `topic_not_declared` sends an author to their manifest,
`topic_undeclared` to their installation. Every code is relayed to the panel **verbatim**, through the
one `daemonRefusalCode` mapping `bridge.call` already uses.

## Mirror-note duty

`kPanelFactsPublishMethod` (`package_facts.h`) and `PANEL_FACTS_PUBLISH_METHOD` (`packagefacts.ts`)
are byte-compared out of the **built** bundle by `tools/check_webui_assets.py --panel-contract`
(ctest `webui-panel-contract`), alongside `panel.daemon.call`, `panel.events.poll` and
`package.grants.list`. They move in **one commit**; a stale bundle produces a confusing failure, so
rebuild `context_editor_webui` before reading the result.

## Known gaps, named rather than papered over

- **`tools/check_ui_bus_boundary.py` owes a deny-list entry for `panel.facts.publish`.** It already
  owed one for `panel.daemon.call`; this task adds the second daemon-reaching method and therefore
  widens exactly that hole. Closing it is `f1`, deliberately sequenced after this task, and it must be
  verified the way that checker was verified originally — by planting a forwarding path and watching
  the gate go red.
- **The fact bus is primary-window-only**, inheriting `PackageSessionHost`'s existing gap:
  `SecondaryWindowSurfaces` does not install the package session host, so a package panel torn out
  into a second window cannot publish. Fail-closed, and recorded in `package_sessions.h` control 4.
- **`PackageFactHost` reads `events.publishes[]` as a per-PACKAGE union**, not per contribution — the
  same shape (and the same stated limitation) as `declared_capabilities` in `package_grants.cpp`,
  because the grant document, the daemon session and the delivery buffer are all keyed by package.
- **The topic-declaration check is per package, and the `packageId` is editor-core's word** (the S2
  residual `package_sessions.h` records). Bounded: a mis-attributed panel could still only publish
  topics that package declared, so a mis-attribution stays a mis-attribution and never becomes an
  escalation.

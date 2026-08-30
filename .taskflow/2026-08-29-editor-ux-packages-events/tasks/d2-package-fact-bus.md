---
id: "d2-package-fact-bus"
title: "The package fact bus: publish verb, topic registry, consented subscription, last-value dedup (D4, D5)"
group: "D"
sequence: 2
repo: "."
base_branch: "main"
depends_on: ["c1-selection-subjects", "c2-manifest-v3"]
importance: 9
complexity: 9
security_critical: true
production_touching: false
model_hint: "top"
taskflow_refs: ["02-target-architecture.md", "05-selection-and-package-events.md", "08-compatibility-and-migration.md"]
---

## Goal

Implement D4 + D5: packages broadcast facts to each other **without depending on each other**, on
**daemon topics** (never the window-local `editor.ui` bus). A package publishes onto its own declared
topic; another package subscribes under an install-time consented grant. Today there is **no verb by
which a package can publish onto the daemon bus at all**, and cross-package subscription is refused
by explicit design (`packageui.ts:30-34`) — D4 answers that refusal's *security reason* with an
explicit grant rather than by removing the check.

**D5 is the load-bearing rule — the set's single most important sentence:** two packages hold
separate baseline daemon sessions and therefore **different origins**, so origin echo suppression
does **not** stop an A → B → A mirroring loop, and arbitrary package topics have no daemon-side
state to dedup against. Without D5 this ships an unbounded loop. So **a package fact is a STATE, not
an edge.**

## Scope & seams

- **New**:
  - a **publish verb** (operational), refused unless the topic is declared by *that* package in
    `events.publishes[]` (manifest v3, from `c2`) and correctly namespaced;
  - a **package-topic registry** — topics registered at install/load, introspectable through
    `describe` (R-CLI-013 CLI ≡ RPC ≡ MCP parity);
  - a **consented subscription grant** for another package's topic, riding the existing
    `package.grants.list` / `package.grants.decide` machinery (`packagegrants.ts` +
    `package_grants.h`): the `~/.context/` document no package can write, clamped to what the
    manifest declared, fail-closed in every direction, snapshot-at-boot. Deny-by-default.
- **D5 semantics**:
  1. **Last-value retention per topic** — the daemon holds the current value and **refuses to publish
     a repeat** (the identical rule already protecting `selection` and `camera`); a mirroring pair
     converges after one round.
  2. **Snapshot-on-subscribe** falls out of retention — a late subscriber immediately receives the
     retained value (the `editor.ui` bus's model; no subscribe-then-ask race).
  3. **Reentrancy refused** — publishing from inside an event handler is rejected **with a
     diagnostic**, turning the remaining loop shape into something an author can see and fix.
  - Accepted cost, do not "fix" it: a package **cannot send pure edge events** — the second identical
    publish deduplicates. Edge semantics must be modelled as state (a counter, a token).
- **Reused unchanged — no new transport**: the package's baseline daemon session, the bounded
  per-package Shell buffer (`package_events.h`), `panel.events.poll`, `PackageEventPump`
  (`packageevents.ts`), and the loud `dropped`/`gapped` pair.
- **Mirror-note duty** (`08` §2): any new verb constant crossing the TS/C++ boundary moves in one
  commit under the byte-compare discipline (`panel.events.poll` / `package.grants.list` / `drag.*`
  precedent).
- The `editor.ui` built-in topic set **stays closed at nine**; `packageui.ts`'s refusal of
  cross-package `editor.ui` subscription **stays**.
- Out of scope: the boundary checker's deny-list entry for the new daemon-reaching method — that is
  `f1`, deliberately sequenced after this task.

## Definition of Done

Every "X did not happen" claim has a sibling proving the path **can** produce X in the same fixture
family (the set's named gate):

- **Dedup**: publishing the same value twice delivers **once** — AND a sibling proving a *different*
  value delivers twice, so the first cannot pass by the topic being dead.
- **Snapshot**: a late subscriber receives the retained value — AND a sibling proving a topic with no
  publish yet delivers nothing.
- **Reentrancy**: a publish from inside a handler is refused **with its diagnostic asserted**, not
  merely absent.
- **The grant, both halves**: an ungranted cross-package subscribe is refused AND a granted one
  delivers — both, or neither proves the gate. Plus: a grant cannot exceed what the manifest declared
  (clamp test).
- **Publish authorization**: an undeclared topic and a mis-namespaced topic are both refused with
  diagnostics; a declared, namespaced one publishes.
- `describe` lists package topics (parity test); `dropped`/`gapped` still delivered on overflow/gap.
- TS/C++ verb constants moved in one commit; `webui-panel-contract` and the uibus boundary ctests
  green.
- PR body cites D4/D5 and states the edge-event cost as accepted.

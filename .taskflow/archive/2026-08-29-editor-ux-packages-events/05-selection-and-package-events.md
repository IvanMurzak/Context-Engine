# 05 — Typed selection and the package fact bus

Covers tasks `c1`, `d2`, `f1`. Read before touching `editor_session_state.*`, the `editor.*` verbs in
the contract registry, `session_feed.cpp`, `inspector_feed.cpp`, `uibus.ts`, `packageui.ts`, or
`packagegrants.ts`.

**This is the document answering the owner's question "how do independent packages work together
without depending on each other".** The short answer: the mechanism already exists and is shipping —
it needs a typed subject and a consented cross-package subscription.

---

## 1. What already works, so it is not rebuilt

The two-tier model (`docs/editor-ui-bus.md`, `docs/editor-session-state.md`):

- **Tier 1, the daemon**, topic `session` — the semantic answers to *"what is the human working on"*:
  selection, cameras, play state. Broadcast to **every** subscriber with no per-client filtering, so a
  second window, the CLI and a scripted agent all see the same facts.
- **Tier 2, the `editor.ui` bus** — window-local chrome (focus, layout, drag, theme, palette, …), which
  **never** reaches the daemon. Two ctests hold that boundary.

**Hierarchy → Inspector already runs on tier 1 today** (`session_feed.cpp:117,150`,
`inspector_feed.cpp`). The owner's example is therefore not new architecture; it is the existing
architecture extended to two more subjects and two more panels.

### The three rules that make tier 1 loop-safe

1. `origin` echo suppression — apply a foreign origin, drop your own. Ids are minted **per wire
   connection**.
2. **A no-op publishes nothing.**
3. The writer learns its own outcome from the **reply**, never from the fact.

**Know which rule does the work.** Rule 2 is the cycle breaker; rule 1 only stops a client re-applying
its own echo. This distinction is what `d2` turns on.

## 2. Typed selection (`c1`) — D1, D2, D3

### Wire

```
editor select --subject <kind> --ids <id…> --mode replace|add|toggle|remove
editor selection-get [--subject <kind>]      →  { ids, selections: [ { subject, ids }, … ] }
editor selection-focus-get                   →  { subject }

fact  selection-changed { subject, ids, mode, origin }
fact  selection-focus   { subject, origin }
```

`subject` is **optional, defaulting to `entity`** — so this is an **additive** change and
`protocolMajor 1` does not move.

**The reply is additive as well** (`D1` REVISED 2026-08-29). `editor.selection-get` answers
`{ids: […]}` today (`kernel_server.cpp:964-968`) and has a live reader in
`attach_command.cpp:157`, which embeds it as the attach observer's `selection` member. Replacing that
object with a bare array would have broken every such reader **silently** — a missing member reads as
absent, not as an error — while `08 §4` asserted the opposite. So `ids` **stays**, carrying the
`entity` selection as it does now, and the typed view arrives as a NEW `selections` member: an
**array of objects carrying their key**, never a map-keyed object, matching the convention the camera
array already follows and keeping the persisted file diffable. `--subject` narrows what `selections`
(and `ids`) report; it never changes the reply's shape.

The one cost, stated plainly: `ids` is redundant with `selections[subject=="entity"].ids` from the day
this lands, and stays so until a major moves. That was accepted over a silent break.

### Independent selections, one focus

Selecting a file does **not** clear the entity selection (D1). `selection-focus` (D3) is the arbiter:
it says which live selection the human is actually working on. It is a **tier-1 fact** deliberately —
by the two-tier model's own test, *"what is the human working on"* is exactly what an agent needs, so
deciding it from tier-2 panel focus would make the answer invisible to the CLI, to agents and to a
second window.

Consumers: Inspector renders the focused subject; Scene highlights only `entity`; Files highlights only
`file`.

### The subject vocabulary is open (D2)

`entity`, `file`, `asset` are contract-owned. A package declares `<pkg>.<kind>` in
`selection.subjects[]` (manifest v3, see `04` §2) and the registry validates the namespacing with the
same discipline `validatePackageTopic` and `validatePackageCommandId` apply. An unknown subject on the
wire is **refused**, not coerced — the same reasoning as `parse_selection_mode`, whose header states
that a silent fallback to `replace` would mutate more than the caller asked.

### The silent hazard this task must close in the same PR

`session_feed.cpp:111-124` is the **sole** consumer of the `selection-changed` fact, and it applies it
**unconditionally**. Without a `subject == "entity"` filter, a file selection is fed to
`SceneTreePanel::apply_selection` as L-35 entity id-paths.

⚠ `inspector_feed.cpp` is **not** a second filter site — a claim an earlier draft of this document
made and the review disproved. The Inspector never subscribes to the fact: it is driven by
`SceneTreePanel::add_selection_listener`, wired at `builtin_panels.cpp:667-690`, so filtering
`session_feed` protects it transitively. The Inspector's own share of this task is D3 — rendering the
**focused** subject, which is a new `selection-focus` consumer beside that listener, not a filter.

**It fails silently** — the scene tree simply shows nothing selected, which is indistinguishable from
a correct empty result. So the test is written the falsifiable way round: publish a `file` fact and
assert the scene tree did **not** move, with a sibling asserting an `entity` fact **does** move it.
One direction alone proves nothing.

### `.editor/session.json` v1 → v2

Today the loader has **no migration branch**, and it is important to be exact about what that means,
because the obvious guess is wrong. `editor_session_state.cpp:262-266` refuses a **future** version, a
non-number, or `< 1` — nothing else. An **older** version is not refused at all: every member is read
under an `if (doc.contains(…))` guard (`:269-289`), which is the additive absorption the constant's own
header describes (`:26-28`, *"bumped only for a shape change the loader cannot absorb additively"*).

So the un-migrated failure mode is **not** a quarantine — it is worse. A v1 file passes the version
check, the loader looks for `selections`, finds nothing, and the user's selection is **silently
dropped with no diagnostic at all**. That is what the migration branch exists to prevent:
`selection: {ids}` → `selections: [{subject: "entity", ids}]` is lossless. Add the branch; keep the
future-version and malformed cases exactly as they are.

⚠ **This drives the test's shape.** "Assert a v1 file is not quarantined" is **vacuous** — it passes
with the migration deleted, because v1 was never quarantined. The falsifiable assertion is that a v1
file's **selection survives** into `selections`, which fails the moment the branch is removed.

## 3. The package fact bus (`d2`) — D4, D5

### What is new

| Piece | Detail |
|---|---|
| A publish verb | Operational, refused unless the topic is declared by *that* package in `events.publishes[]` and correctly namespaced |
| A package-topic registry | Topics registered at install/load, introspectable through `describe` so R-CLI-013 CLI ≡ RPC ≡ MCP parity holds |
| A consented subscription grant | Subscribing to **another** package's topic rides the existing `package.grants.list` / `package.grants.decide` machinery — the `~/.context/` document no package can write, clamped to what the manifest declared, fail-closed in every direction |

### What is reused unchanged

The whole delivery path: the package's baseline daemon session, the **bounded per-package Shell
buffer** (`package_events.h`), `panel.events.poll`, `PackageEventPump` (`packageevents.ts`), and the
loud `dropped` / `gapped` pair that tells a panel its cursor is worthless. No new transport, no second
poll surface.

### D5 — a package fact is a STATE, and this is the cycle breaker

> Two packages hold **separate** baseline daemon sessions and therefore **different** origins. Origin
> echo suppression does **not** stop an A → B → A mirroring loop. Arbitrary package topics have no
> daemon-side state to dedup against. Without D5 this architecture ships an unbounded loop.

So:

1. **Last-value retention per topic.** The daemon holds the current value and **refuses to publish a
   repeat** — the identical rule that already protects `selection` and `camera`. A mirroring pair
   converges after one round instead of spinning.
2. **Snapshot-on-subscribe falls out of it.** A panel opened later immediately receives the retained
   value, matching the `editor.ui` bus's model, so "subscribe, then separately ask for current state"
   — the race that model exists to remove — does not reappear here.
3. **Reentrancy is refused.** Publishing from inside an event handler is rejected with a diagnostic.
   This turns the remaining loop shape into something a package author can see and fix, rather than a
   hang.

**The accepted cost, stated plainly:** a package **cannot send pure edge events**. "The button was
pressed twice" is not expressible — the second publish deduplicates against the first. A package
needing edge semantics must model it as state (a counter, a token). This was accepted knowingly as the
price of a broadcast bus that cannot be made to loop.

### Why not the `editor.ui` bus

It is window-local. Facts on it are invisible to the CLI, to agents, and to a second window unless
explicitly mirrored, and routing semantic facts there would erode the D7 boundary two ctests exist to
defend. `packageui.ts:30-34` refuses cross-package subscription there for a *security* reason — the
other package never agreed — and D4 answers that reason properly, with an explicit grant, rather than
by removing the check.

### Test shape

Every claim of the form "X did not happen" needs a sibling proving the path **can** produce X in the
same fixture family. Specifically:

- dedup: publishing the same value twice delivers **once** — and a sibling proving a *different* value
  delivers twice, so the first test cannot pass by the topic being dead;
- snapshot: a late subscriber receives the retained value — and a sibling proving a topic with no
  publish yet delivers nothing;
- reentrancy: a publish from a handler is refused **with its diagnostic**, not merely absent;
- the grant: an ungranted cross-package subscribe is refused, and a granted one delivers — both, or
  neither proves the gate.

## 4. The boundary deny-list the docs already owe (`f1`)

`docs/editor-ui-bus.md` records this as an open gap, in its own words: a daemon-forwarding method
`panel.daemon.call` **now exists**, the premise that a mirror sink *could not* reach the daemon is
gone, `check_ui_bus_boundary.py` owes it a deny-list entry, and *"this is not yet implemented — so
today the rule is enforced by review rather than by the gate."*

`d2` adds a **second** daemon-reaching method (the publish verb), which widens exactly that hole. So
the deny-list entry stops being tidy-up and becomes part of this set: name both methods in
`tools/check_ui_bus_boundary.py`, and verify it the way that checker was verified originally — by
**planting a forwarding path and watching the gate go red**. A boundary test that would still pass with
a violation in place is worse than none.

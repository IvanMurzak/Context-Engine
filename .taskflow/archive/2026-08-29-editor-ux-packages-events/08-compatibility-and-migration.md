# 08 — Compatibility and migration

**Read this before changing any version number.** Three separate versioned surfaces move in this set,
they move for different reasons, and two of them have a documented "treat the unexpected as corrupt"
rule that a careless bump would trigger against our own users.

---

## The three surfaces

| Surface | Move | Kind | Task |
|---|---|---|---|
| The R-EDIT-001 extension contract, `kContractMajor` | **2 → 3** | **BREAKING** | `c2` |
| The panel wire vocabulary (`panel.*` methods) | additive: `instanceId` | additive, but **gated** | `c3` |
| `.editor/session.json`, `kSessionFileVersion` | **1 → 2** | **migrating** | `c1` |

The `editor.*` contract verbs — `editor.select`, `editor.selection-get`, the new
`editor.selection-focus-get` — are **additive only**, and `protocolMajor 1` **does not move**. That is
the whole reason D1 made `subject` an optional parameter defaulting to `entity`.

⚠ **Additive means the REPLY too, and this had to be corrected** (`D1` REVISED 2026-08-29). An earlier
draft specified `selection-get`'s reply as a bare array `[{subject, ids}, …]`, replacing today's
`{ids: […]}` (`kernel_server.cpp:964-968`). That is a **breaking** reply change — it silently breaks
every reader, `attach_command.cpp:157` included, since a missing member reads as absent rather than as
an error — and it would have made this section's own claim false. The reply now **keeps `ids`** and
**adds `selections: [{subject, ids}, …]`**. `protocolMajor 1` genuinely does not move. The cost is one
redundant member until a major; the alternative was a silent break.

---

## 1. `kContractMajor` 2 → 3 (breaking, `c2`)

**The compatibility window is exactly one major.** `extension.h` states it: a contribution declaring a
different major is refused by the registry, so this bump refuses every v2 contribution the instant it
lands. There is no overlap period and none can be created without changing that rule.

That is **safe today and only today**: there are no out-of-repo consumers. It is the identical
reasoning that made the 1 → 2 bump safe, and that bump left the discipline to follow:

> enumerate **every** in-repo consumer and move them in the same change — the four CMake targets that
> link `context_gui_contract` plus their tests, harnesses and fixtures — each referencing the constant
> **symbolically** rather than hardcoding a literal.

Re-run that enumeration; do not trust the 1 → 2 list, which is a year old.

**What changes shape:** `dock.singleton` is **removed** (not deprecated) and replaced by
`instances: {mode, max}`; `path`, `selection.subjects[]` and `events.{publishes,subscribes}[]` are
added. See `04` §2.

**After v1 ships this same change costs a deprecation cycle.** That is the argument D6 turned on, and
it is why the bump is in this set rather than the next one.

## 2. The panel wire vocabulary (additive, `c3`)

`instanceId` joins `panel.render` / `panel.command` / `panel.gesture` / `panel.state.get` /
`panel.state.set`.

**The gate is the thing to plan around.** `tools/check_webui_assets.py --panel-contract` (ctest
`webui-panel-contract`) byte-compares the TS vocabulary against the C++ constants **by reading the
values out of the BUILT bundle**. So:

- the TS and C++ constants move in the **same commit**, or the gate reds;
- because it reads the built bundle, a stale bundle produces a confusing failure — rebuild before
  reading the result.

This discipline exists because a rename on either side otherwise unbinds the panel surface
**silently**: the renderer calls a method the Shell no longer routes and the editor comes up empty with
no build error anywhere. The same mirror-note pattern applies to `panel.events.poll`,
`package.grants.list` and the `drag.*` pair — `d2` adds a publish verb and inherits the duty.

## 3. `.editor/session.json` 1 → 2 (migrating, `c1`)

**Today there is no migration branch, and the gap does not fail the way it looks like it should.**
`editor_session_state.cpp:262-266` treats a **future** version, a non-number, or `< 1` as **corrupt**:
quarantine the file aside (`session.corrupt.json`, then `session.corrupt-1.json`, `-2`, … so evidence
is never clobbered — `:328-341`), load defaults, and announce loudly with an
`editor.session_state_invalid` diagnostic plus a stderr line. The daemon still boots and serves.

An **older** version hits none of that. It passes the version check untouched, and every member is
then read under an `if (doc.contains(…))` guard (`:269-289`) — the additive absorption
`kSessionFileVersion`'s own header states at `:26-28`. So without a migration branch a v1 file is
**silently accepted with its selection dropped**: the loader looks for `selections`, finds nothing,
and reports nothing. Not a false alarm — no alarm at all, which on a user's persisted session is the
worse of the two.

So:

```
v1:  "selection":  { "ids": ["a", "b"] }
v2:  "selections": [ { "subject": "entity", "ids": ["a", "b"] } ]
     "selectionFocus": { "subject": "entity" }
```

- A `version: 1` document is **migrated**, losslessly, to the v2 shape — which is what stops the
  silent drop above. (It is not quarantined either, but note that it never would have been: the
  quarantine path only ever fires forward, so "v1 is not quarantined" is **not** an assertion that can
  fail and must not be written as the test.)
- **Everything else stays exactly as it is**: a future version, a non-number, a malformed document all
  keep the quarantine-plus-defaults-plus-loud-diagnostic path untouched.
- Cameras keep their array-of-objects-carrying-their-key encoding, and `selections` adopts the same —
  never map-keyed, so the file stays diffable.
- Play state remains **unpersisted** (a restarted daemon holds no live session; restoring `playing`
  would be a lie about L-51 provenance).

**Test both directions, and a third:** a v1 file migrates and **its selection survives into
`selections`** — the falsifiable half, which reddens the moment the branch is deleted; a v2 file
round-trips; a `version: 99` file is still quarantined with the diagnostic. The third is what stops the
migration branch from accidentally swallowing the corrupt path.

## 4. What does NOT move, stated so nobody bumps it defensively

- **`protocolMajor` stays 1.** Every `editor.*` change here is additive. The deprecation policy
  (`docs/deprecation-policy.md`) governs it and this set does not invoke it.
- **The `editor.ui` built-in topic set stays closed at nine.** D4 routes package facts through the
  **daemon**, not this bus, precisely so this set adds no member here. Adding one is a deliberate act
  with an authority; this set has no such authority.
- **The `GestureVerb` set stays at four.** `begin`/`extend`/`commit`/`cancel`; the Shell refuses a
  fifth.
- **The three `write-notice` kind tokens stay three.** They are `drop`, `refusal` and `abandoned`
  (`write_notice.h:108-110`), byte-compared against their TS mirrors `WRITE_NOTICE_KIND_*` by
  `tools/check_webui_assets.py` (`:413-422`). `e2`'s refusal UX uses the existing **`refusal`** (and
  `drop` where a value moved under it) and does not mint a fourth. ⚠ `bad` / `wait` are **not** kind
  tokens — they are the notification TONE `notifications.ts:142` already derives from the kind
  (`drop`/`abandoned` → `wait`, otherwise → `bad`), so `e2` gets the tone for free and must not send
  it on the wire.

## 5. Ordering consequence

`c2` (breaking manifest) must land **before** `c3` (runtime) and `d1` (menu), and `c1` (selection) must
land before `e1`/`e3` consume subjects. Wave A and wave B touch none of these surfaces and are
therefore free to run alongside — which is why they are first: the owner sees the visible fixes without
waiting on the contract work.

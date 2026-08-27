---
id: e08d-boot-when-context-wiring
title: editor (08d) — wire `boot.ts` when-context to the real `DaemonSessionState` (retire `STUB_SESSION_STATE`)
group: C
sequence: 23
repo: "."
base_branch: "main"
depends_on: [e08b-panel-state-rewiring, e06b-theme-engine]
importance: 7
complexity: 3
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [05, 04]
split_from: e08b-panel-state-rewiring   # TD ruling 2026-07-22 — DoD line 3 reassigned, see below
---

> **Carved out of [`e08b`](e08b-panel-state-rewiring.md)'s DoD line 3** (TD ruling 2026-07-22).
> Group C. e08b delivered the real `DaemonSessionState` source in `when.ts` **with tests**, but could
> not land the live wiring: the file that consumes it — `src/editor/webui/core/src/boot.ts` — was owned
> by the concurrently-running [`e06b`](e06b-theme-engine.md) under the wave's file-scope rule, and
> e06b was still open when e08b reached its gate.
>
> **This is a genuine defect in `main`, not cosmetic residue.** `boot.ts:364` actively resolves
> when-context from `STUB_SESSION_STATE`, so browser-side **`playState` is frozen at `edit` for the
> whole session** and `DaemonSessionState` is reachable only from tests. Any `when` clause keyed on
> play state is therefore wrong in the live editor. e08b's refine explicitly refused to claim the DoD
> box, which is why this task exists instead of a green checkmark.
>
> ⚠ Do NOT dispatch before **e06b has merged** — that is the whole reason the work was deferred.

## Goal

Retire `STUB_SESSION_STATE`: make `boot.ts` resolve e07's when-context from the real
`DaemonSessionState` that e08b landed, so `playState` (and every other session-derived `when` token)
reflects daemon truth in the live editor rather than a frozen placeholder.

## Scope & seams

- **`src/editor/webui/core/src/boot.ts`**: swap the when-context source from `STUB_SESSION_STATE` to
  the real `DaemonSessionState` (e08b, `when.ts`), wired to the bridge like the other live sources.
  The follow-up steps e08b journalled are the starting point, not the whole set — re-derive against
  the merged tree, since e06b will have changed `boot.ts` underneath.
- **Delete `STUB_SESSION_STATE`** and its doc comment once nothing references it. A retained "just to
  compile" placeholder is exactly how a temporary stub becomes permanent — if something still needs
  it, that is a finding to report, not a reason to keep it.
- **Prove the wiring, don't assert it** (both sibling runs this wave shipped a confident claim that
  measurement falsified): a test must fail if the source is reverted to the stub. e08b's own
  structural test is the model — it asserts a second client's state reaches the *rendered* output with
  `writes_issued() == 0`.
- Keep e07's when-eval + palette tests passing unchanged — evaluation SEMANTICS do not change, only
  the source. That invariant is the point of the task.
- Out of scope: the `editor.ui` bus ([`e08c`](e08c-editor-ui-bus.md)), anything in the C++ Shell
  (e08b already landed it), the daemon-restart staleness gap (tracked separately — see Links).

## Standing lessons (carry forward)

1. A spec's ripple list is a starting point, never the whole set.
2. Read CI before reviewing, even on a NORMAL entry.
3. A passing sibling only exonerates a suspected flake if that leg registers + runs the affected code.
4. A local browser probe inherits the DEV HOST's ambient media state (`prefers-color-scheme`,
   reduced-motion, locale, DPI); a CI runner has none — force the CI condition before trusting a
   local green (`--blink-settings=preferredColorScheme=1|0`). This cost e06b a full CI round.
5. Beware a test mock more capable than the real thing (e08a's `granted_scopes()` hid for 6 tasks).

## Definition of Done

- [ ] `boot.ts` resolves when-context from the real `DaemonSessionState`; **`STUB_SESSION_STATE` is
      DELETED**, not merely bypassed
- [ ] Live `playState` tracks daemon truth (no longer frozen at `edit`); a test FAILS if the source is
      reverted to a stub
- [ ] e07's existing when-eval + palette tests pass **unchanged**
- [ ] `docs/editor-session-state.md`'s consumer table matches reality (e08b corrected an overclaim
      there once already — do not reintroduce one)
- [ ] Tests same PR (R-QA-013); 3-OS CI green

## Links

- Carved out of: [[e08b-panel-state-rewiring]] · blocked by: [[e06b-theme-engine]]
- Related: [[e08a-daemon-session-state]] (the daemon surface), [[e08c-editor-ui-bus]]
- Separate follow-up filed from the same refine: the session surface has **no `play-state` GET verb**
  (`select | selection-get | camera-set | cameras-get | play | pause | stop | step`), so after a daemon
  restart the rendered play state goes stale with no honest repair. Resetting to `edit` on re-attach
  was rejected — a dropped wire to a *surviving* daemon would then falsely assert "no live session".

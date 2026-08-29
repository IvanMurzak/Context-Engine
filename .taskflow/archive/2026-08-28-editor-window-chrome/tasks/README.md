# Task specs — editor window chrome

Cut 2026-08-28 from the reviewed-by-owner set (taskflow-review **skipped by explicit owner
decision**, recorded in ROADMAP progress log). Specs are **immutable**: no `status` field ever;
live state lives ONLY in `../ROADMAP.md`, written only by `taskflow-execute` after verification.

## Planning-id → spec map

| Planning (ROADMAP draft) | Spec | Group |
|---|---|---|
| c01 chrome contract | [`a1-chrome-contract.md`](a1-chrome-contract.md) | A |
| c02 strips scaffold | [`a2-strips-scaffold.md`](a2-strips-scaffold.md) | A |
| c03 Windows frameless | [`b1-windows-frameless.md`](b1-windows-frameless.md) | B |
| c04 macOS hybrid | [`c1-macos-hybrid.md`](c1-macos-hybrid.md) | C |
| c05 play-bar strip | [`d1-playbar-strip.md`](d1-playbar-strip.md) | D |
| c08 statusbar content | [`d2-statusbar.md`](d2-statusbar.md) | D |
| c07 menu system | [`d3-menu-system.md`](d3-menu-system.md) | D |
| c06 dock-panel retirement | [`e1-playbar-dock-retirement.md`](e1-playbar-dock-retirement.md) | E |
| c09 secondary-window chrome | [`f1-secondary-window-chrome.md`](f1-secondary-window-chrome.md) | F |
| c10 verification + closeout | [`g1-verification-closeout.md`](g1-verification-closeout.md) | G |

## Groups = conflict domains

- **A (foundation, serial)** — the cross-layer contract (bridge headers, four window backends,
  the four-site region vocabulary, ten smokes) then the webui strip scaffold that consumes it.
  Both touch `editorstate.ts` and the smokes; serial by construction.
- **B (Windows native)** — `win32_window.cpp` / `window.cpp` / `window.h` only.
- **C (macOS native)** — `cocoa_window.mm` only. B ∥ C are disjoint backends.
- **D (webui chrome content, serial)** — d1 play bar, d2 statusbar, d3 menu. All three append to
  `app.css` / `boot.ts` / `commands.ts`, and d1+d3 both add bridge surfaces that edit all ten
  smoke files — one conflict domain, run ascending. d3 additionally waits on C (NSMenu half).
- **E (panel retirement)** — C++ roster/a11y/help + frozen-gate test amendments; disjoint from D's
  webui/bridge files, needs only d1's strip to exist.
- **F (secondary windows)** — strip gating + factory-window chrome; after B and C.
- **G (closeout)** — verification, docs, handoff; after everything.

## Interim-honesty staging (binding across A/B/C)

`chrome.state.mode` always reports what the backend actually DOES, not the target table: every
backend ships `"system"` in a1; b1 flips win32 → `"custom"` and c1 flips cocoa → `"hybrid"` in
the same PR that makes it true; x11 stays `"system"` (D6). This avoids double chrome (web controls
over a stock OS titlebar) between waves. a2 implements and DOM-tests all three modes by injecting
`chrome.state` values, independent of the live backend.

## Standing gates (every task — from ../ROADMAP.md)

1. Isolated worktree, PR, full 42-check CI green before merge, plant-verified tests both halves
   (R-QA-013).
2. **Ten-smoke rule**: any new boot-time bridge surface is installed in all ten live CEF smokes in
   the same PR (`window_bridge.h:5-10`) or `bridge.refused() == 0` reds them. Surfaces in this
   set: a1 (`chrome.state`, `window.minimize`, `window.toggle-maximize`, `window.focus`),
   d1 (`session.control`), d3 (`menu.publish`).
3. **Vocabulary mirrors move together**: `RegionKind` tokens change in all four sites in one
   commit (`input.h` / `editor_state_bridge.h` / `editorstate.ts` / `webui-panel-contract` gate).
4. **Frozen-gate amendments (e1) are owner-visible**: PR body enumerates every amended m5/m85
   gate, e06d five-gate-partition precedent cited.
5. Pure decision functions are tested on all three OS legs via existing `editor-shell-test_*`
   families — no ci.yml `--target` edits for plain families; any new CEF smoke registration pays
   the "Not Run = RED" bookkeeping.

## Model policy

Complexity 1–4 → `fast`, 5–7 → `mid`, 8–10 → `top` (none of these tasks is security-critical or
production-touching). Re-scored at cut time: a1/a2/c1/d1 raised from the draft board's cx 7 to 8
(multi-backend + ten-smoke + four-mirror-site blast radius; c1 is native ObjC++ verifiable only
through CI) so tier and score agree.

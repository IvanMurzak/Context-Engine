# 09 — Verification & CI

## 1. The three tiers (D9)

| Tier | What | Where | Blocking? |
|---|---|---|---|
| **T1 — headless logic** | uitree/panel models, state contracts, command registry resolution, keymap/when evaluation, theme schema validation, subscription-consumer protocol (gap/re-snapshot), rebase-or-drop over wire mocks, a11y audit + focus order | 3-OS default matrix (no CEF), as today | BLOCKING |
| **T2 — windowed, CDP + command-driven** | packaged-shape app boots → real window → panels render → commands execute (palette-driven) → tear-out window opens (+ degradation fallback: create-fail → floating group, loudly) → panel rehomes → cross-window selection propagation asserts → theme switch + watched-theme-file hot reload → concurrent-CAS rebase-or-drop against the live daemon (scripted second `context_client`) → crash-recovery drill → latency instrumentation asserts (R-HUX-011) → golden screenshots (C-F10) | Linux (xvfb, the existing `xvfb-run` pattern `ci.yml:1785-1788`) + macOS (GH runner, windowed OK) + Windows (see §3) | BLOCKING (per-OS caveats in §3) |
| **T3 — real OS input** | synthesized OS-level mouse/keyboard: cross-window tab drag, focus arbitration, viewport camera/gizmo drags, IME/text entry | Interactive-session Windows runner + local owner box; mac/Linux = release-checklist manual pass | Windows continuous; mac/Linux honest checklist |

Design property that makes T2 cheap: because ALL interaction routes through the command
registry (D8) and events are observable (D7), most "interactive" flows are drivable as
command sequences + event/layout assertions over CDP — no pixel-poking. CDP is in-box with CEF.

## 2. New CI jobs & gates

- `editor-app-smoke` (3-OS; task home **e16** — C-F5): build shell + editor-core, boot the
  PACKAGED shape (sandbox ON; accelerated-OSR tripwire with automatic software degrade —
  B-F5), run the T2 scenario script, upload screenshots. **Registered as its own
  `docs/ci-fleet-manifest.json` gate row** (the existing `editor-cef-smoke` has none — the
  m5-exit fold-in is not repeated; 01 §5).
  ⚠ **Amended 2026-07-19** (owner rejected the wgpu-native fork — 03 §3): the accelerated-OSR
  tripwire is now meaningful on **macOS only**, which is the sole OS shipping an accelerated
  import (stock native accessors). **Windows** ships CPU-upload and **Linux** software upload,
  so on those legs the job asserts the *software* path is the one actually taken — the accel↔
  software seam is retained but its Windows branch is disabled, and a leg silently flipping to
  accelerated there would itself be the regression. The automatic-software-degrade assertion is
  unchanged on every OS. This job also inherits the sandbox-ON + `shared_texture_enabled`
  assertions orphaned by `s2`'s supersession (07 §2).
- `editor-boundary` : out-of-tree consumer build of the editor against INSTALLED
  `context_client`/contract artifacts + include-graph check (D10).
- `editor-visual-regression`: golden screenshots per panel × {Dark, Light} × key states,
  SSIM-diffed (reuse the proven web-golden harness pattern + its Chrome teardown lessons);
  bless-flow documented (intentional re-golden = reviewed diff).
- `m9-exit-*` blocking ctests (final task), wired per the tripwire discipline: added to the
  job's `--target` list AND the named `ctest -R "^m9-exit-"` step AND excluded from the general
  step (`ci.yml:151` pattern), one gate per exit clause, fleet-manifest rows for each.
- Sanitizer legs: T1 additions ride the existing ASan/UBSan/TSan wiring; wall-clock budgets in
  editor tests are sanitizer-aware from day one (`CONTEXT_TSAN_BUILD` lesson).
- CEF payload cost: `editor-app-smoke` reuses the pin-keyed CEF cache (`ci.yml:1734-1738`);
  the editor target is NOT added to the a12 build-time bench target list in M9 (explicitly out,
  to keep the budget meaningful; revisit post-M9).

## 3. Windows honesty (Session 0)

The self-hosted Windows runners run as services (Session 0): native-GPU windowed presents and
real window interaction are NOT reliable there (existing carve-outs `offscreen_main.cpp:75-84`,
`editor_host.cpp:376-385`). Therefore:

- T2-Windows runs the windowed boot in the best mode the session allows (software-OSR,
  headless-composited present asserts, CDP-driven UI) — what Session 0 cannot express is
  explicitly listed in the gate doc.
- T3 requires an **interactive-session Windows runner** (auto-logon, unlocked desktop) — a new
  runner class registered in the fleet manifest (`runner_classes` pattern), provisioned on the
  owner box; until provisioned its gates are advisory-until-provisioned (the a21/ops1 precedent)
  plus a release-checklist manual pass. Provisioning is a small owner action, not hardware spend.

## 4. Latency & a11y gates

- R-HUX-011 budgets measured from the REAL instrumented path (input→commit→derive→paint
  timestamps, 03 §6) in T2; thresholds committed in a budget JSON (band-checked like existing
  benches); regressions block.
- R-A11Y-001: existing scan extends to Dockview chrome + iframe panels (04 §6); keyboard-only
  navigation test per panel remains blocking on Linux T2 (existing pattern `ci.yml:1769-1777`).

## 5. M9 exit gate (D16) — clauses

1. All `m9-exit-*` ctests green on the 3-OS matrix (each design MUST maps to exactly one gate).
2. Signed installers for Win/mac/Linux exist and clean-host boot-smoke (07 §3).
3. T3 Windows suite green on the interactive runner (or advisory + manual pass if unprovisioned,
   stated in the gate record).
4. Visual-regression goldens blessed in both themes; a11y + latency gates green.
5. Demo external package panel installs and docks end-to-end (04 §5).
6. **Owner visual sign-off**: screenshot/video tour of every panel, both themes, multi-window —
  the taste gate (06 §5).
7. **Step-budget walkthrough** (10-user-workflows): every budget recounted on the shipped
   build and recorded in the gate record; overruns are blocking findings unless owner-waived
   (C-F8).

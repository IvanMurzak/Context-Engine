---
id: e16-a11y-latency-visualreg-ci
title: a11y extension + R-HUX-011 latency gates + visual-regression harness + the blocking T2 editor-app-smoke job + fleet rows
group: E
sequence: 2
repo: "."
base_branch: "main"
depends_on: [e05-editor-core-foundation, e06-tokens-theme-engine, e09-wire-writes-undo, e11-viewports-picking-gizmos]
importance: 8
complexity: 7
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [09, 04, 06]
---

## Goal

Build the verification net that keeps the editor correct and beautiful over time: the
blocking 3-OS `editor-app-smoke` T2 job (packaged shape, own fleet-manifest row — C-F5), the
golden-screenshot visual-regression job, the a11y extension to Dockview chrome + iframe
panels, and the R-HUX-011 latency budgets made blocking from the real instrumented path.

Runs after e15 in the group-E lane, so the packaged sandbox-ON shape exists for the smoke.

## Scope & seams

- **`editor-app-smoke`** (3-OS, blocking): build shell + editor-core → boot the PACKAGED
  shape (sandbox ON; accelerated-OSR tripwire with automatic software degrade — B-F5) → run
  the T2 scenario script (09 §1 list: boot → panels render → palette-driven commands →
  tear-out + create-fail degradation → rehome → cross-window selection → theme switch +
  watched-file hot reload → concurrent-CAS drill vs a scripted second `context_client` →
  crash-recovery drill → latency asserts → screenshots) → upload artifacts. Linux xvfb
  (`ci.yml:1785-1788` pattern); Windows in Session-0-honest mode with the un-expressible
  list stated in the gate doc (09 §3); macOS windowed.
  **Own `docs/ci-fleet-manifest.json` gate row** — the `editor-cef-smoke` fold-in is NOT
  repeated (01 §5).
- **`editor-visual-regression`**: deterministic CDP goldens per panel × {Dark, Light} × key
  states (incl. empty/loading/error states — 04 §6), SSIM-diffed; reuse the proven
  web-golden harness pattern INCLUDING its Chrome process-group teardown lessons; documented
  bless flow (intentional re-golden = reviewed diff).
- **a11y extension** (R-A11Y-001): Dockview chrome (tab strips arrow-navigable, drop zones,
  floating controls) enters the scan + keyboard map; iframe panels get an axe-style scan +
  keyboard-only navigation test; Linux-blocking per the existing pattern
  (`ci.yml:1769-1777`); uitree panel scan continues mechanically off the promoted roster.
- **Latency gates** (R-HUX-011 made blocking): budgets measured from the REAL instrumented
  path (input→commit→derive→paint timestamps — 03 §6) in T2; thresholds committed in a
  budget JSON, band-checked like existing benches; **sanitizer-aware from day one**
  (`CONTEXT_TSAN_BUILD` widen lesson).
- **CI hygiene**: reuse the pin-keyed CEF cache (`ci.yml:1734-1738`); editor target NOT
  added to the a12 build-time bench list (explicit, revisit post-M9); new-gate wiring per
  the tripwire discipline (built by the job's target list + named ctest step + excluded from
  the general step).

## Definition of Done

- [ ] `editor-app-smoke` green on 3 OSes, blocking, with its own fleet-manifest row
      (`tools/check_fleet_manifest.py` passes)
- [ ] Visual-regression job green; goldens blessed for both themes; a deliberate pixel
      change demonstrably fails until re-blessed
- [ ] a11y: Dockview chrome + iframe scans blocking on Linux T2; keyboard-only tests pass
- [ ] Latency budgets enforced; an injected regression demonstrably blocks; sanitizer legs
      unaffected (widened budgets applied)
- [ ] Accel tripwire demonstrably degrades to software-OSR on simulated failure (B-F5)
- [ ] All new jobs registered per the fleet/tripwire discipline; 3-OS CI green

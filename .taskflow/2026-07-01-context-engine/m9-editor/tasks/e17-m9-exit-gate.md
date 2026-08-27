---
id: e17-m9-exit-gate
title: m9-exit-* blocking gates + fleet rows + T3 provisioning + step-budget walkthrough + owner visual sign-off (D16)
group: E
sequence: 3
repo: "."
base_branch: "main"
depends_on: [s1-dockview-cef-spike, s2-wgpu-shared-texture-spike, d1-visual-direction-mockups, e01-daemon-fanin-auth, e02-client-sdk-boundary, e03-present-texture-import, e04-window-shell-windows, e05-editor-core-foundation, e06-tokens-theme-engine, e07-commands-palette-keymap, e08-session-state-ui-bus, e09-wire-writes-undo, e10-multiwindow-tearout, e11-viewports-picking-gizmos, e12-macos-linux-shells, e13-package-panels, e14-welcome-lifecycle, e15-packaging-installers, e16-a11y-latency-visualreg-ci]
importance: 9
complexity: 6
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [09, 10, 07]
---

## Goal

Close M9 honestly: one blocking `m9-exit-*` ctest per exit clause wired per the tripwire
discipline, the T3 real-OS-input tier stood up (runner class registered; advisory until the
owner provisions), the step-budget walkthrough recounted on the shipped build, and the owner
visual sign-off tour — engineering-complete per D16 (public release stays a separate owner
call).

## Scope & seams

- **`m9-exit-*` ctests** (09 §5 clauses → one gate each):
  1. exit-suite green 3-OS (the umbrella wiring itself);
  2. signed installers exist + clean-host boot-smoke (consumes e15 artifacts);
  3. T3 Windows suite (or advisory + manual pass if unprovisioned — stated in the gate
     record);
  4. visual-regression goldens blessed both themes + a11y + latency gates green;
  5. demo external package installs and docks end-to-end (e13's hello-panel);
  6. owner visual sign-off recorded (screenshot/video tour: every panel, both themes,
     multi-window — the taste gate);
  7. step-budget walkthrough (10-user-workflows) recounted on the shipped build; overruns =
     blocking findings unless owner-waived (C-F8).
- **Wiring discipline** (M6 lesson, `ci.yml:151` pattern): each gate ADDED to the job's
  `--target` list AND the named `ctest -R "^m9-exit-"` step AND excluded from the general
  step; fleet-manifest row per gate.
- **T3 tier** (09 §3): interactive-session Windows runner CLASS registered in the fleet
  manifest (`runner_classes` pattern); suite = OS-synthesized input (cross-window tab drag,
  focus arbitration, viewport camera/gizmo drags, IME/text entry); provisioning = small
  owner action (a21/ops1 precedent) — until then gates are advisory + release-checklist
  manual pass; mac/Linux = honest manual checklist, executed and recorded.
- **Gate record**: a committed record capturing clause status, Session-0/T3 honesty notes,
  budget counts, waivers, and the owner sign-off evidence.

## Definition of Done

- [ ] All `m9-exit-*` ctests green on the 3-OS matrix; fleet rows validated; tripwire
      wiring verified (a gate cannot silently Not-Run)
- [ ] T3 runner class registered; suite implemented; status honest (green on the
      provisioned runner, or advisory + manual pass recorded)
- [ ] Step-budget walkthrough recorded — every 10-doc budget counted on the shipped build;
      overruns dispositioned (fixed or owner-waived, in writing)
- [ ] mac/Linux manual T3 checklist executed and recorded
- [ ] **Owner visual sign-off recorded** (the D16 taste gate)
- [ ] Gate record committed; engine ROADMAP/ledger updated; M9 declared
      engineering-complete (public release = separate owner call)

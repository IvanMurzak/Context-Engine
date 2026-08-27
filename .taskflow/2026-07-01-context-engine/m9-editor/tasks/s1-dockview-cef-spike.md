---
id: s1-dockview-cef-spike
title: Dockview-in-CEF ratification spike — docking under CSP, iframes, tear-out flow, isolation probe, supply-chain review
group: C
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 7
complexity: 7
security_critical: true   # output feeds the npm supply-chain allowlist gate
production_touching: false
model_hint: top           # mid by complexity, bumped for security
taskflow_refs: [02, 04, 08]
---

## Goal

Ratify D2 (Dockview v7 as the docking engine) with real evidence under real conditions —
CEF 149, strict CSP, custom scheme, sandboxed-iframe panel content — and produce the exact
pinned package set plus the npm supply-chain review for the owner approval gate. Throwaway
code; ratified DECISIONS are the deliverable (repo spike charter).

## Scope & seams

- Throwaway probe under `spikes/dockview-cef/` (reuse the `spikes/cef-compositing/` CEF 149
  harness patterns; CEF via existing `tools/cef-prebuilt.json` + `fetch_cef.py`).
- Probe matrix (each = explicit PASS/FAIL in FINDINGS.md):
  1. Docking core: splits/tabs/floating groups under a strict no-inline-script CSP served from
     a custom scheme (not `file://`).
  2. Sandboxed-iframe panel content inside Dockview panels (`sandbox="allow-scripts"`, opaque
     origin) — layout, resize, focus behavior.
  3. `toJSON()` serialize / restore fidelity (incl. floating groups).
  4. The PanelHost tear-out flow (B-F2): serialize → destroy → recreate in a SECOND
     browser/window with a Dockview root seeded from state. Dockview's popout API is
     deliberately unused — confirm v7 rejects non-http(s) popout URLs (evidence for the
     design's rationale).
  5. Per-extension process-isolation probe (B-F6): Chromium `IsolateSandboxedIframes`
     behavior for distinct `context-ext://` origins under CEF 149 — feature default, NOT a CEF
     contract; record what actually happens.
  6. a11y scan of Dockview chrome (tabs, drop zones) with the existing `tools/a11y_scan.py`
     approach — feeds the e16 scan scope.
- Name the exact **v7 package set** (B-F9): `dockview` core + `dockview-modules` (incl. the
  a11y module), exact pinned versions.
- **npm supply-chain review** (08 §3): maintenance health, transitive dependency tree,
  licenses vs the deny-by-default allowlist — filed as the owner-gate artifact.
- Fallback evidence if any probe fails: Golden Layout → Lumino (D2 order).

## Definition of Done

- [ ] `spikes/dockview-cef/FINDINGS.md` with the 6-probe PASS/FAIL matrix + measured notes
- [ ] Exact v7 package set + pinned versions named (or fallback recommendation with evidence)
- [ ] Supply-chain review document ready for the owner npm-allowlist gate (08 §3)
- [ ] Ratify-or-fallback recommendation recorded; ROADMAP progress log updated by the TD
- [ ] Spike code clearly marked throwaway; no production targets link it; CI untouched or
      spike-job-isolated

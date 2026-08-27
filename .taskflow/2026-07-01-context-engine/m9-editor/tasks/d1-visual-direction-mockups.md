---
id: d1-visual-direction-mockups
title: Visual direction mockups — monochrome-glow-ui port proposals, live HTML, owner pick (O1)
group: D
sequence: 1
repo: "."
base_branch: "main"
depends_on: []
importance: 6
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [06, 02]
---

## Goal

Produce live-HTML editor mockups (not images) porting the owner's shipped monochrome-glow-ui
design system to the Context Editor, presenting concrete options for the one signature
flourish (aurora placement — O1), so the owner can pick the visual direction at the d1 gate
before e06 hardcodes token values.

## Scope & seams

- Home: `m9-editor/mockups/` inside this taskflow (relocated 2026-08-27 from the `software`
  superproject's `.claude/design/context-engine/m9-editor/mockups/` into this repo's own
  `.taskflow/2026-07-01-context-engine/m9-editor/mockups/` — design collateral, not engine
  code). Self-contained HTML/CSS, openable locally in a browser.
- Source of truth to port: `cloud/packages/design/` — `tokens.css`, `components.css`,
  `tailwind-preset.cjs` ("monochrome-glow-ui", 17/17 shipped).
- Mock at minimum: full editor window (dock chrome + tab strips per Dockview anatomy), Scene
  viewport panel + Inspector + Scene tree + Problems + play bar, welcome screen — in **both**
  Dark and Light per the 06 §2 values (canvas #000000/#fafafa families, status hues, Geist +
  Geist Mono, 1px borders no shadows, 3px selection ink bar).
- **Aurora options** (O1): variant A = Play button only; variant B = Play button + welcome
  primary CTA. Animated conic blur-halo; show the reduced-motion static fallback.
- Viewport palette group proposal (axes X/Y/Z, grid major/minor, selection outline, gizmo
  hover/active) — the legal chroma exception (D12).
- Deliberately NO engine code, no tokens package — e06 consumes the picked direction.

## Definition of Done

- [ ] Live HTML mockups render locally, both themes, covering the surfaces listed above
- [ ] Aurora placement variants A/B presented; reduced-motion fallback shown
- [ ] Viewport palette proposal included
- [ ] **Owner picked the direction + answered O1** (recorded in ROADMAP progress log — this
      is the d1 human-approval gate)
- [ ] Picked values handed to e06 (token value sheet referenced from the mockup folder)

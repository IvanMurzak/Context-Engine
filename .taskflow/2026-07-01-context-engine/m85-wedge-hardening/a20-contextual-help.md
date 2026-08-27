---
id: a20-contextual-help
title: In-editor contextual help (R-HUX-010)
group: a
sequence: 6
repo: "."
base_branch: "main"
depends_on: []
importance: 5
complexity: 4
security_critical: false
production_touching: false
model_hint: fast
taskflow_refs: [R-HUX-010, R-CLI-013, R-HUX-004, ROADMAP §1-M8.5 trailing-GUI bucket]
---
## Goal
Contextual help in the editor GUI, generated from the live contract — not hand-written parallel
docs that rot.

## Scope & seams
- Help affordance per panel/verb driven by `context describe` output + per-verb `--help`
  (R-CLI-013/R-HUX-004) and the schema `notes`/`x-ctx-units` vocabulary; getting-started
  pointers to the human-onboarding samples (R-QA-006).
- Thin GUI layer only — no new content pipeline; a11y coverage in the same PR.

## Definition of Done
- [ ] Help opens in-context for the shipped panels; verb help matches live introspection
      (generated, asserted by test).
- [ ] Works offline (no network fetch); a11y scan green.

---
id: a5-cli-verbs
title: CLI drive/assert verbs (context ui …) ≡ RPC ≡ MCP + the ui.* error-catalog domain
group: A
sequence: 5
repo: "."
base_branch: "main"
depends_on: []
importance: 8
complexity: 5
security_critical: false
production_touching: false
model_hint: mid
taskflow_refs: [T5]
---
## Goal
The "driven/asserted headless via CLI" exit leg: one-shot verbs generated from the ONE R-CLI-009
registry — `ui dump` (tree+rects JSON), `ui query <node>`, `ui send <event>`
(click/focus/key/text), `ui assert`. Mints the `ui.*` error-catalog domain (append-only tail of
`src/editor/contract/src/error_catalog.cpp` — the one shared single-lane-safe anchor).

## Scope & seams
`src/cli/`, `src/editor/contract/` (registry + catalog), `src/packages/ui/` (introspection
shims), `samples/` (corpus exercise of the new verbs).

Namespace note (R-CLI-007): the bare `ui` noun follows the as-built first-party precedent
(engine registry mints `session`/`determinism` nouns bare; `<ns>:` is for package-ADD-contributed
verbs) — record this one-line rationale in the registry comment.

## Definition of Done
- [ ] CLI ≡ RPC ≡ MCP parity test green (R-CLI-009); error-catalog additive-only check green
      (R-CLI-008).
- [ ] New `ui.*` verbs registered `stable` + `implemented` (the samples-corpus breadth gate only
      covers stable∧implemented∧non-exempt verbs — do NOT ship them non-stable/exempt) and
      exercised by the samples corpus IN THE SAME PR (the gate reds otherwise).
- [ ] General CI step green on all 3 legs.

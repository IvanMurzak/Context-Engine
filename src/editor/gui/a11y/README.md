# `gui/a11y/` — editor-UI accessibility enforcement harness (M5-F6)

Hardens the F0b a11y-harness *hook* into the real per-panel enforcement gate that
**R-A11Y-001** requires: an automated semantic/ARIA scan **plus** a keyboard-only navigation
test, run **per editor panel in CI**, with a coverage manifest every panel registers into.

## What it is

- **`context_gui_a11y`** — a pure, headless C++ library over `context_gui_uitree` (no CEF, no
  browser). It scans a panel's UI-logic tree for the R-A11Y-001 findings (`missing-name`,
  `orphan-command`, `unreachable-command`, `duplicate-id`), captures the keyboard focus order,
  and renders the panel's semantic HTML — emitting a deterministic JSON report.
- **`context_gui_a11y_scan`** — the harness executable (ctest `gui-a11y-scan`): scans every
  **registered** panel, asserts each is a11y-clean, and exits non-zero on any violation. Because
  it is CEF-free it runs on the **default 3-OS `build` matrix** and the local dev gate.
- **`tools/a11y_scan.py`** — the CI gate (modelled on `tools/golden_compare.py`). It consumes the
  harness JSON report and, per panel, re-audits the **rendered DOM string** (an axe-core-class
  semantic/ARIA scan over the emitted HTML — no browser needed), verifies the keyboard path, and
  **cross-checks the coverage manifest** against the scanned panels. It runs in the
  `editor-cef-smoke` CI job, **blocking on the Linux leg**.

## Why headless (the design decision)

`uitree::render_html()` emits each panel's semantic HTML deterministically from the exact
UI-logic tree the CEF host renders — so auditing the tree audits the DOM the host paints (the
same artifact on both sides of the CEF boundary; see `gui/uitree/builtin.h`). Running the scan +
keyboard-nav over the headless tree therefore needs **no CEF, no GPU, no browser**, runs on every
OS on the default matrix, and is fully deterministic — strictly better than driving axe-core
inside a booted CEF renderer for the observer-grade M5 surface. The task's "prefer the headless
`context_gui_uitree` where possible" is taken to its conclusion: everything runs headless.

## Coverage contract (three append-only shared anchors)

A panel is **covered** only when it appears in **all three**:

1. **The built-in roster** — its `Contribution` in `gui/contract/src/builtin_roster.cpp`. Since
   M9 e05b this is the single source of truth for *which panels exist*.
2. **`panel_factories()`** in `registry.cpp` — binds the roster id to a headless factory so the
   harness can instantiate + audit it. **`registered_panels()` is DERIVED** from the roster ∩ this
   table and is not hand-edited: it can never name a panel the roster does not declare.
3. **`coverage.manifest.jsonl`** — one JSON object per line declaring the panel must be scanned.

Two guards fail CI if the anchors disagree (a panel factory-bound but not rostered, rostered but
unbound, or declared but not scanned):

- **`gui-a11y-coverage`** — the STANDING C++ contract guard (`tests/test_coverage.cpp`). Asserts
  roster == factories == scanned == `coverage.manifest.jsonl` in **both** directions, plus the
  roster's ORDER. Because it is CEF-free it runs on the **default 3-OS `build` matrix** and the local
  dev gate, so the drift is caught in the panel's OWN PR — not only on the CEF-gated
  `editor-cef-smoke` leg. It hardcodes no panel names, so it never goes stale as panels land in later
  milestones.
- **`tools/a11y_scan.py`** — the CEF-side DOM re-scan gate; it additionally cross-checks the manifest
  against the *scanned* panels (blocking on the `editor-cef-smoke` Linux leg).

`coverage.manifest.jsonl` is the single source of truth (there are no per-panel `coverage/*.json`
fragment files — the pre-manifest migration ones were removed). It is append-only and marked
`merge=union` in `.gitattributes`, so concurrent fan-out waves append their panel without conflicts.

### Adding a panel — register a11y WITH the panel (same PR)

a11y registration is part of a panel's OWN landing wave, NOT a trailing harness wave — the
process fix for the M5-F4 gap, where the Problems panel landed before the harness and shipped
uncovered. In the SAME PR that lands a panel:

1. Append its `Contribution` to the built-in roster (`gui/contract/src/builtin_roster.cpp`) — the
   manifest-v2 entry (id, title, icon, dock defaults, `state.schema_version` >= 1, capabilities).
   **Do this first**: a factory with no roster entry is dropped by the derivation and fails the
   coverage ctest.
2. Append one `factories.emplace_back(<Panel>::kContributionId, <factory>)` line to
   `panel_factories()` in `registry.cpp` (take the id from the panel class's own `kContributionId`
   constant, never a re-typed literal), and link the panel's library into `context_gui_a11y`
   (a11y `CMakeLists.txt`).
3. Append one `{"id": "<id>", "title": "...", "owner": "<task>", "requires": ["semantic-scan", "keyboard-nav"]}`
   line to `coverage.manifest.jsonl`.
4. Append its `PanelHelp` entry to `help::panel_topics()` (`gui/help/src/help_model.cpp`) — guarded
   by a DIFFERENT ctest (`gui-help-contextual`), so skipping it passes the a11y guard and still reds
   the build.
5. Ship the panel's own tests in the same PR (R-QA-013). The `gui-a11y-scan` + `gui-a11y-coverage`
   ctests + the CI gate then enforce a11y on the new panel automatically — no CI-workflow edit needed.

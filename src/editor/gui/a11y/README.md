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

## Coverage contract (two append-only shared anchors)

A panel is **covered** only when it appears in **both**:

1. **`registered_panels()`** in `registry.cpp` — maps the panel id to a headless factory so the
   harness can instantiate + audit it.
2. **`coverage.manifest.jsonl`** — one JSON object per line declaring the panel must be scanned.

Two guards fail CI if the two anchors disagree (a panel registered but undeclared, or declared but
not registered/scanned):

- **`gui-a11y-coverage`** — the STANDING C++ contract guard (`tests/test_coverage.cpp`). Reads
  `coverage.manifest.jsonl` and `registered_panels()` and asserts they name the SAME set. Because it
  is CEF-free it runs on the **default 3-OS `build` matrix** and the local dev gate, so the drift is
  caught in the panel's OWN PR — not only on the CEF-gated `editor-cef-smoke` leg. It hardcodes no
  panel names, so it never goes stale as panels land in later milestones.
- **`tools/a11y_scan.py`** — the CEF-side DOM re-scan gate; it additionally cross-checks the manifest
  against the *scanned* panels (blocking on the `editor-cef-smoke` Linux leg).

`coverage.manifest.jsonl` is the single source of truth (there are no per-panel `coverage/*.json`
fragment files — the pre-manifest migration ones were removed). It is append-only and marked
`merge=union` in `.gitattributes`, so concurrent fan-out waves append their panel without conflicts.

### Adding a panel — register a11y WITH the panel (same PR)

a11y registration is part of a panel's OWN landing wave, NOT a trailing harness wave — the
process fix for the M5-F4 gap, where the Problems panel landed before the harness and shipped
uncovered. In the SAME PR that lands a panel:

1. Append one `RegisteredPanel{"<id>", &<factory>}` line to `registered_panels()` in `registry.cpp`.
2. Append one `{"id": "<id>", "title": "...", "owner": "<task>", "requires": ["semantic-scan", "keyboard-nav"]}`
   line to `coverage.manifest.jsonl`.
3. Link the panel's library into `context_gui_a11y` (a11y `CMakeLists.txt`).
4. Ship the panel's own tests in the same PR (R-QA-013). The `gui-a11y-scan` + `gui-a11y-coverage`
   ctests + the CI gate then enforce a11y on the new panel automatically — no CI-workflow edit needed.

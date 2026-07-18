# `src/editor/gui/help/` — in-editor contextual help (M8.5 a20, R-HUX-010)

The **Help & getting-started** editor surface. Its defining property: **help is GENERATED from the
live contract, never hand-written parallel docs that rot.** Per-verb help is a pure projection of the
ONE registry (`src/editor/contract/`), so it is exactly what `context describe` and
`context <verb> --help` emit (R-CLI-013 / R-HUX-004). The getting-started references point at the
R-QA-006 human-onboarding samples. **Thin layer** — no new content pipeline, no new dependency, no
network fetch (offline by construction); pure C++ over `context_gui_uitree` + `context_contract`, so
it builds + tests on the default 3-OS matrix with **no CEF**.

## Pieces

- **`help_model.{h,cpp}` (`context_gui_help`)** — the generation core:
  - `render_verb_help(verb, core_flags)` / `verb_help(command)` / `all_verb_help()` — per-verb help
    projected field-by-field from `contract::VerbSpec` (+ the R-CLI-007 core flags). The
    `gui-help-test_help_model` ctest cross-checks every field against `contract::verb_describe_json()`
    (the literal `--help` output), so the help cannot drift from introspection.
  - `getting_started_samples()` — the R-QA-006 human-onboarding sample references. `gui-help-getting-started`
    asserts every referenced `samples/**` directory exists (rots-if-drift; no dangling links).
  - `panel_topics()` / `contextual_help(panel_id)` — per-panel contextual help. The related-command
    CONTENT is generated from the registry; only the thin panel→verbs mapping is authored.
- **`help_panel.{h,cpp}` (`HelpPanel`, id `builtin.help`)** — the headless getting-started/help panel:
  a labelled region with a keyboard-navigable getting-started sample list and a per-panel help list,
  each topic carrying the LIVE verb summary read from the registry. a11y-conformant by construction.

## Extension point (register-with-the-panel)

When a new editor panel lands, add one `PanelHelp` entry to `panel_topics()` (help_model.cpp) — the
same discipline as registering a11y coverage. The `gui-help-contextual` ctest cross-checks
`panel_topics()` against `a11y::registered_panels()`, so a panel added without a help topic (or a
topic naming a phantom panel) reddens the CEF-free default build. `gui-help-test_help_model` likewise
reddens if a `related_commands` entry names a verb no longer in the registry.

## Tests (ctest family `gui-help-*`, default 3-OS `build` matrix — no `.github/**` edit)

- `gui-help-test_help_model` — per-verb help == live introspection; corpus == stable+implemented set.
- `gui-help-test_help_panel` — the panel renders the samples + topics + a live verb summary; deterministic.
- `gui-help-test_a11y` — the Help panel is a11y-clean + fully keyboard-navigable.
- `gui-help-getting-started` — every getting-started sample path is a real committed directory.
- `gui-help-contextual` — help topics name exactly the registered panels (coverage completeness).

The Help panel is also scanned by the standing `gui-a11y-scan` / `gui-a11y-coverage` gates via its
`registry.cpp` + `coverage.manifest.jsonl` entries (the append-only a11y anchors).

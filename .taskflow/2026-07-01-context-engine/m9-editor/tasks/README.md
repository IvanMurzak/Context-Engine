# M9 tasks — decomposition index

> Decomposed 2026-07-18 by `/design-tasks` from design v1.1 (adversarially reviewed, D1–D22
> locked). **Specs here are IMMUTABLE** — no status fields, ever. ALL live state (status,
> run id, PR, dates) lives in exactly one place: [`../ROADMAP.md`](../ROADMAP.md) status board.

## Id convention

Task ids keep the design's established planning ids (`s1`, `s2`, `d1`, `e01`–`e17`) — every
cross-reference in docs 01–10, the review findings (A-F*/B-F*/C-F*), and the ROADMAP uses them.
The merge-conflict **group** is carried in frontmatter (`group:`), NOT in the id prefix.

## Coefficient legend

- **importance (1–10)** — what breaks if this task is wrong/missing. 10 = milestone fails or
  security is compromised.
- **complexity (1–10)** — architectural depth + cross-cutting surface + risk. 8+ = correctness
  cliffs (concurrency, GPU interop, crypto/signing, cross-process protocols).

## Model-selection rubric

| Rule | Tier |
|---|---|
| complexity ≥ 8 | **top** (Fable-tier) |
| complexity 5–7 | **mid** (Sonnet-tier) |
| complexity ≤ 4 | **fast** (Haiku-tier) |
| `security_critical` or production-touching | bump one tier up |

⚠ Context-Engine dispatch: **run non-conflicting tasks in PARALLEL** — owner directive
2026-07-19, which **supersedes** the earlier single-lane rule (and the owner lifted the TD's
3-run cap the same day). Tasks whose merge-conflict **groups differ** may run concurrently,
subject to `depends_on`; tasks **within** one group stay strictly sequential. Proven in practice:
e05a ∥ e05b landed clean with no conflict and no lost submodule-pointer race.
**No per-step model overrides** still stands — `model_hint` informs the owner's session-model
choice for each dispatch, not per-step pipeline overrides.

## Groups (merge-conflict domains)

A group = one merge-conflict domain. Tasks within a group run **strictly sequentially in the
listed order** (overlapping file sets); different groups **do** run in parallel subject to
`depends_on` (owner directive 2026-07-19). This is a live concurrency plan, not just dependency
documentation.

| Group | Domain (files) | Sequence |
|---|---|---|
| **A** | daemon/contract/client C++ — `src/editor/bridge/`, `src/editor/contract/`, `src/editor/client/` (new), `src/cli/`, root CMake install, panel write/undo seams, `src/editor/gui/` panel state seams | e01 → e02 → e08a → e08b → e09 (e09 re-ordered last: it now needs e10) |
| **B** | render + native shell C++ — `src/render/` (RHI, render world), `src/editor/shell/` (new), `tools/*-prebuilt.json` pins, wgpu spike | s2 → e03 → e04 → e11 → e12 → e14 |
| **C** | editor-core web layer — `src/editor/webui/` (new), Dockview spike, hydration, panel host | s1 → (e05a ∥ e05b) → e05c → e05d1 → e05d2 → e05d3 → e05d4 → e07a-d → e06a → e06b → e06c → e06d → e08c → e08d → e10 → e13 |
| **D** | design collateral — mockups in this taskflow folder (this repo's `.taskflow/`) | d1 |
| **E** | packaging + CI + gates — `cmake/ContextCef.cmake`, `ci.yml`, `release-sign.yml`, fleet manifest, installer stage | e15 → e16 → e17 |

Cross-group ordering is expressed ONLY via `depends_on` in each spec's frontmatter. Gate tasks
ending consumable groups: **e02** (client SDK artifact + boundary CI, gates C/A consumers),
**e04** (first shell window, gates C), **e13** (package-panel contract, gates e15), **e17**
(milestone exit gate).

## Task table

| id | title | grp | imp | cx | sec | model |
|---|---|---|---|---|---|---|
| [s1](s1-dockview-cef-spike.md) | Dockview-in-CEF ratification spike | C | 7 | 7 | ✔ | top |
| [s2](s2-wgpu-shared-texture-spike.md) | Patched wgpu-native shared-texture spike | B | 8 | 9 | ✔ | top |
| [d1](d1-visual-direction-mockups.md) | Visual direction mockups (owner pick) | D | 6 | 5 | — | mid |
| [e01](e01-daemon-fanin-auth.md) | Daemon multi-client fan-in + attach auth | A | 9 | 8 | ✔ | top |
| [e02](e02-client-sdk-boundary.md) | context_client SDK + boundary CI + CLI migration | A | 9 | 8 | ✔ | top |
| [e03](e03-present-texture-import.md) | Present path + external-texture import + composite | B | 8 | 8 | — | top |
| [e04](e04-window-shell-windows.md) | Window shell v1 (Windows) | B | 9 | 8 | — | top |
| ~~[e05](e05-editor-core-foundation.md)~~ | ⛔ **SUPERSEDED** → e05a–e05d (decomposed 2026-07-20) | C | 9 | 8 | — | top |
| [e05a](e05a-webui-workspace-toolchain.md) | webui workspace + dockview + esbuild + JS codegen | C | 9 | 7 | — | top |
| [e05b](e05b-manifest-roster-state-contract.md) | manifest v2 + roster + a11y regen + D6 + render_html | C | 9 | 8 | — | top |
| [e05c](e05c-app-scheme-ipc-bridge.md) | context-editor:// scheme + resource handler + IPC bridge | C | 9 | 8 | ✔ | top |
| ~~[e05d](e05d-panelhost-hydration-layout.md)~~ | ⛔ **SUPERSEDED** → e05d1–e05d4 (decomposed 2026-07-20) | C | 9 | 8 | — | top |
| [e05d1](e05d1-panelhost-hydration-runtime.md) | PanelHost over Dockview + hydration runtime v1 | C | 9 | 8 | — | top |
| [e05d2](e05d2-layout-persistence-region-maps.md) | Layout persistence + region maps end-to-end | C | 8 | 7 | — | mid |
| [e05d3](e05d3-shell-boundary-refactor.md) | D10 boundary refactor + live scenetree/inspector | C | 9 | 9 | ✔ | top |
| [e05d4](e05d4-t2-boot-dock-restore-smoke.md) | T2 boot→dock→restore CEF smoke + ci.yml wiring | C | 8 | 7 | — | mid |
| [e06](e06-tokens-theme-engine.md) | Tokens package + theme engine + Settings panel | C | 7 | 6 | — | mid |
| [e07](e07-commands-palette-keymap.md) | Command registry + palette + keymap | C | 8 | 7 | — | mid |
| ~~[e08](e08-session-state-ui-bus.md)~~ | ⛔ **SUPERSEDED** → e08a–e08c (decomposed 2026-07-22) | A | 8 | 7 | — | mid |
| [e08a](e08a-daemon-session-state.md) | `editor` verbs + session topics + session.json + parity CI | A | 8 | 7 | — | mid |
| [e08b](e08b-panel-state-rewiring.md) | Rewire scene tree + playbar + when-context onto daemon state | A | 8 | 7 | — | mid |
| [e08c](e08c-editor-ui-bus.md) | `editor.ui` local bus (D7 tier 2) | C | 8 | 6 | — | mid |
| [e08d](e08d-boot-when-context-wiring.md) | Wire boot.ts when-context to real DaemonSessionState | C | 7 | 3 | — | mid |
| [e09](e09-wire-writes-undo.md) | Writes over RPC + undo + session-file split | A | 9 | 8 | — | top |
| [e10](e10-multiwindow-tearout.md) | Multi-window: tear-out, rehome, cross-window drag | C | 8 | 8 | — | top |
| [e11](e11-viewports-picking-gizmos.md) | Viewports: cameras, picking, gizmos | B | 9 | 8 | — | top |
| [e12](e12-macos-linux-shells.md) | macOS + Linux shell backends | B | 7 | 7 | — | mid |
| [e13](e13-package-panels.md) | Package panels end-to-end + demo package | C | 8 | 7 | ✔ | top |
| [e14](e14-welcome-lifecycle.md) | Welcome screen + daemon lifecycle + arbitration | B | 7 | 6 | — | mid |
| [e15](e15-packaging-installers.md) | Packaging: sandbox ON, installers, signing | E | 9 | 7 | ✔ | top |
| [e16](e16-a11y-latency-visualreg-ci.md) | a11y + latency gates + visual regression + T2 CI | E | 8 | 7 | — | mid |
| [e17](e17-m9-exit-gate.md) | m9-exit gates + T3 + owner sign-off | E | 9 | 6 | — | mid |

Spread: 12 top / 8 mid / 0 fast · importance median 8 · complexity median 7.

## Live state

Waves, ready-set, status, run/PR links → [`../ROADMAP.md`](../ROADMAP.md) (the single source
of task state; the board is computed-ready from `needs` + ✅, never stored here).

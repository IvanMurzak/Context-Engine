# M9 — Interactive Editor Application (Context Engine)

> **Status:** DESIGN v1.1 — drafted 2026-07-18 from a live owner design session + 4 ground-truth
> explorations of the engine repo (`IvanMurzak/Context-Engine` @ `4b7456f`, post-M8.5);
> **adversarially reviewed 2026-07-18** (3 parallel reviewers: code ground-truth / external
> specs / consistency — 2 P0 external corrections + consistency fixes applied; no owner
> decision revised). **Tasks designed 2026-07-18** — 20 immutable specs in `tasks/`
> ([index](tasks/README.md)); live state on the [ROADMAP status board](ROADMAP.md).
> Next lifecycle step: owner GO → `/design-implement`.
> **v1.2 — reconciled 2026-07-19** against owner rulings taken during Wave 0/1 execution. Four
> rulings propagated into the docs: **(1)** the patched `wgpu-native` fork is **REJECTED** →
> `s2` SUPERSEDED, Windows OSR ships CPU-upload, macOS keeps stock-accessor acceleration
> (01, 02, 03, 07, 08, 09, this file); **(2)** **O1 RESOLVED** — the signature flourish is
> **Pulse of Work**, not the aurora, which was rejected outright (02, 06, 10, D12 below);
> **(3)** dependency edges amended — `s2→e03` void, `e05→e09` added (ROADMAP);
> **(4)** npm allowlist cleared for **`dockview-core@7.0.2` only**, version-pinned (02, 04, 08,
> D2 below). Superseded passages are struck or bannered in place, never deleted — the original
> reasoning is retained as history. ⚠ **[`ROADMAP.md`](ROADMAP.md) is the authoritative ledger**
> for implementation state and owner rulings; where a doc and the ROADMAP disagree, the ROADMAP
> wins and the doc is a reconcile bug — report it.
> **This folder is the design authority for M9.** The engine-wide design authority remains
> `../core/` (L-1…L-62, R-*); this design **adds** M9 and must never contradict a lock —
> where it extends one, the extension is explicit and named: R-EDIT-001 (panel manifest/state),
> R-UI-007, R-HUX-003 (welcome subset only), R-HUX-004 (palette ships), R-HUX-011 (budgets made
> blocking), and the L-20 session-file mapping (split into daemon-/editor-owned files — 03 §1).

## Problem

M0–M8.5 delivered the v1 wedge: headless file-authoritative EditorKernel daemon, full CLI/RPC/MCP
surface (41 registered verbs, 37 implemented; protocolMajor=1), deterministic sim, packed-build pipeline, and the **GUI panel
LOGIC** (9 panels, CEF substrate, OSR mode-selector) — all CI-verified headlessly. But there is
**no interactive editor**: `editor_host` boots CEF windowless/off-screen; no native OS window, no
input routing, no docking, no web UI layer, no theme system, no packaged app. The recorded lead
v1.x item (ROADMAP.md §"v1.x ledger", 2026-07-18 honesty note) is exactly this gap.

M9 builds the **Context Editor**: a standalone, signed desktop application (Windows/macOS/Linux)
with VS Code-grade windowing — dockable/tearable panels across N native OS windows — N interactive
2D/3D viewports with selection/gizmos, a package-extensible panel ecosystem, a token-based theme
system (Dark/Light + custom themes), and the monochrome-glow-ui aesthetic, all as an **ordinary
client** of the public daemon contract.

## Locked decisions

| # | Decision | Who / date |
|---|---|---|
| D1 | **One milestone (M9) to full VS Code-level windowing** — docking + tear-out into separate OS windows ship together; no phased releases | Owner 2026-07-18 |
| D2 | **Docking engine = Dockview** (MIT, vanilla TS, serialization + floating built in; pin v7.x, exact package set named at s1 — ~~v7 split core/modules~~). Scope clarified by review (B-F2): Dockview handles **in-window** docking/floating only; OS-window tear-out is a first-class PanelHost/Shell mechanism (its popout API is deliberately unused). Fallbacks: Golden Layout → Lumino. ✅ **s1 RATIFIED + package set named 2026-07-19: exactly ONE package — `dockview-core@7.0.2`** (MIT, **0 runtime deps**), NOT the core+`dockview-modules` set assumed at design time; owner-approved into the production allowlist, **version-pinned** (any bump past 7.0.2 or extra `dockview-*` package re-triggers the 08 §3 gate). Fallback NOT triggered | Owner 2026-07-18 |
| D3 | **Code home = Context-Engine repo**, package-structured, with a CI-enforced "public contract only" boundary (see D10) | Owner 2026-07-18 |
| D4 | **Optimality mandate**: choose for user experience + long-term reliability + maintainability; initial build cost is explicitly NOT a criterion | Owner 2026-07-18 |
| D5 | **N simultaneous viewports**, two types (Scene = editor camera, Game = runtime camera), any viewport in any window; bounded only by GPU memory | TD under D4 |
| D6 | **Strict panel-state contract**: `getState()/restoreState()` versioned JSON; panels are pure functions of (project state via bridge + state blob); **no retainContext** — rehome/tear-out/crash-recovery/layout-restore all ride one mechanism | TD under D4 |
| D7 | **Two-tier events**: semantic human state (selection, camera, play state) becomes **daemon session state** with events on daemon topics (agents can see it); UI-chrome facts (focus/layout/drag/hover) stay on an editor-local `editor.ui` bus with the same envelope model; UI-chrome is NOT published to the daemon | TD under D4 |
| D8 | **All interaction through one command registry** with `when`-contexts (VS Code model); shortcuts = command bindings, per-user canonical-JSON keymap, hot-reloaded; no ad-hoc key handlers | TD under D4 |
| D9 | **Three-tier verification**: T1 headless logic (3-OS, blocking) · T2 windowed CDP + command-driven smokes (3-OS, blocking; xvfb on Linux) · T3 real-OS-input suite (continuous on interactive Windows runner only; mac/Linux = release checklist) — stated honestly in the exit gate | TD under D4 |
| D10 | **Boundary enforcement is link-level**: editor targets build as an out-of-tree consumer against installed/exported client SDK artifacts in a dedicated CI job; include-graph check; npm side depends only on published client packages | TD under D4 |
| D11 | **Themes = data (JSON tokens), never CSS/code**; semantic tokens cover colors, typography, shape, elevation, motion, iconography; Dark (default) + Light + high-contrast built-in; custom themes via user file (watched, hot-reload) AND package contribution; `prefers-reduced-motion` always wins | Owner (requirement) + TD (mechanism) 2026-07-18 |
| D12 | **Aesthetic = inherit "monochrome-glow-ui"** (the owner's shipped ai-pipeline design system: true-black/off-white flat surfaces, 1px borders never shadows, Geist/Geist Mono, 5 reserved status hues, ~~aurora halo used sparingly~~); editor adds a viewport palette token group (axes/grid/selection) as the legal chroma exception. ⚠ **AMENDED 2026-07-19 — the aurora clause only.** The owner reviewed live mockups and **rejected the aurora** (rotating conic halo) outright; the signature flourish is **Pulse of Work** — glow colour + rhythm mirror the Play button's live state, reusing the 5 reserved status hues, **zero new colour tokens** (O1 RESOLVED). Everything else in D12 stands. Spec: [`mockups/TOKENS.md` §5](mockups/TOKENS.md); summary in [06 §2](06-theme-design-system.md) | Owner 2026-07-18 (aurora clause amended 2026-07-19) |
| D13 | **Launch without a project → mini-welcome screen** (recent list + open folder + new-from-template over `context new`); full launcher stays v2 (R-HUX-003) | Owner 2026-07-18 |
| D14 | **One release train with the engine** — editor version = engine version (honest under frozen protocolMajor hard-fail); independent editor cadence arrives with the second released protocol | Owner 2026-07-18 |
| D15 | **One project per editor process**; all N windows of a process belong to one project/daemon; second project = second process (VS Code model) | Owner 2026-07-18 |
| D16 | **M9 exit = engineering-complete** (gates + signed installers + owner visual sign-off); the public release moment is a separate owner decision (Context Sim precedent) | Owner 2026-07-18 |
| D17 | **Editor UI architecture = uitree-as-contract + TS hydration**: the C++ headless UI-logic tree stays the logic/a11y/testability authority; a new TS **editor-core** web app (Dockview shell + panel host) hydrates panel HTML and dispatches commands/gestures over the bridge; third-party panels bring their own web content in sandboxed iframes | TD (ground-truth-driven) |
| D18 | **The editor app is a wire client only**: it never embeds EditorKernel in-process; it spawns the daemon as a child when absent and attaches over RPC — the "ordinary client" guarantee is physical | TD under D4 |
| D19 | **Daemon multi-client concurrent fan-in is in-scope M9** (today: serial single-connection, M1 model) — prerequisite for GUI+CLI+agents attached simultaneously | TD (ground-truth-driven) |
| D20 | **Attach-token auth enforced in M9** (+ Windows named-pipe owner-SID DACL); today the token is written but never verified | TD (ground-truth-driven) |
| D21 | **Linux windowing scope v1 = X11 (+ XWayland)**; native Wayland post-M9 (CEF Linux accel is gated to Mesa/X11-ozone anyway per L-41) | TD |
| D22 | **GUI writes route over the daemon RPC (`edit`/`edit-batch`)** — the in-process compose/filesync gateway shortcut is retired for the editor app; `OverrideWriteGateway` gets a wire implementation | TD (ground-truth-driven) |

## Design in one paragraph

The Context Editor is a three-layer application that talks ONLY the public contract. The **Shell**
(C++) owns native OS windows (N of them), the per-window compositor (viewport render targets +
the CEF OSR texture composited into each window's swapchain — **accelerated on macOS via stock
native accessors; CPU-upload on Windows and Linux**, amended 2026-07-19, 03 §3), the OS input
pump, and CEF hosting. The **editor-core** (TS web app, per-window CEF browser) owns Dockview docking, the panel
host, the command registry/palette/keymap, the theme engine, and the client bridge (subscription
consumer with snapshot/gap recovery). **Panels** — built-in and third-party alike — live on the
R-EDIT-001 contribution contract extended with a panel manifest + strict state contract; built-in
panel content hydrates from the existing C++ uitree logic, third-party content runs in sandboxed,
capability-scoped iframes that receive theme tokens. Selection/camera/play state move into daemon
session state (visible to AI agents on daemon topics); UI chrome stays editor-local. The app ships
as a signed installer per OS on the engine release train, with CEF sandbox ON via the bootstrap
launch model.

## Documents

| File | Contents |
|---|---|
| `01-current-architecture.md` | Ground truth (file:line, engine @ `4b7456f`): what exists, what is absent, seam index |
| `02-target-architecture.md` | Layering, roles, package map, decisions rationale, open questions |
| `03-shell-window-compositor-input.md` | Native windows, swapchain present, OSR interop, per-window compositor, input pump, multi-window |
| `04-editor-web-app-docking-panels.md` | editor-core TS app, Dockview, hydration runtime, panel manifest + state contract, tear-out/rehome |
| `05-contract-events-commands.md` | Daemon fan-in, client SDK, subscription helper, selection-as-session-state, `editor.ui` bus, command registry/keymap |
| `06-theme-design-system.md` | Token schema, theme engine, monochrome-glow-ui port, custom themes, visual regression |
| `07-packaging-distribution.md` | Sandbox/bootstrap, installers, signing, welcome screen, versioning, daemon lifecycle |
| `08-security.md` | Threat table, token enforcement, iframe/extension sandbox, scope model |
| `09-verification-ci.md` | T1/T2/T3 tiers, CI jobs, fleet-manifest rows, a11y, latency budgets, golden-screenshot harness |
| `10-user-workflows.md` | UX contract per persona with counted step budgets (release gates) |
| `ROADMAP.md` | **Implementation ledger** — waves, dependency edges, status board, gates, progress log |
| `tasks/` | **Task decomposition** (20 immutable specs + [index](tasks/README.md)): groups, coefficients, model hints, DoD per task |

## Glossary

- **Shell** — the native C++ application layer: windows, compositor, input pump, CEF host.
- **editor-core** — the TS web application running inside each window's CEF browser: docking,
  panel host, commands, themes, bridge client.
- **uitree** — the existing headless C++ UI-logic tree (`UiNode`/`Panel`/`render_html`), the
  logic + a11y authority for built-in panels.
- **Hydration** — binding uitree-rendered HTML to live DOM behavior in editor-core (commands,
  gestures) without moving panel logic out of C++.
- **Contribution / panel manifest** — the R-EDIT-001 descriptor a panel registers with, extended
  in M9 (dock defaults, state schema version, capabilities, theme contributions).
- **Rehome** — destroy-and-recreate of a panel in another window/dock position via its state
  contract (tear-out, drag between windows, layout restore).
- **Handoff / OSR** — CEF off-screen-rendering output (shared texture or software buffer) handed
  to the Shell compositor per the L-41 per-platform tree.
- **Tokens / theme** — semantic design variables (colors/typography/shape/motion/…) and the JSON
  document mapping them to values; themes are data, never code.
- **T1/T2/T3** — the three verification tiers (headless / windowed-CDP / real-OS-input).

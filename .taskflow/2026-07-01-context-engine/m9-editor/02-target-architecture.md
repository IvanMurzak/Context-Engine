# 02 — Target architecture

## 1. Principles

1. **Ordinary client, physically.** The editor app talks ONLY the public contract (JSON-RPC +
   events; authored mutations go through the daemon's `edit` verbs — D22 — so the daemon
   performs the file writes). It never links kernel internals (D10/D18). This is what makes alternative
   editors possible by construction and keeps the daemon the single authority.
2. **Existing law is load-bearing.** L-19/L-20 (files vs session state), L-30 (rebase-or-drop),
   R-EDIT-001 (all built-in panels ON the extension contract), R-A11Y-001, R-HUX-011 budgets,
   L-41 compositing tree. M9 extends; it does not re-litigate.
3. **Logic stays headless-testable.** The uitree/panel-model tier remains the behavior authority
   (T1 CI without CEF). The web layer renders and routes; it does not own editing logic (D17).
4. **One mechanism per concern.** Panel state serialize/rehydrate covers tear-out + layout
   restore + crash recovery (D6). The command registry covers palette + shortcuts + tests (D8).
   The token system covers theming + branding + visual regression (D11/D12).
5. **Built-in ≡ third-party.** Every built-in panel registers through the same manifest, state
   contract, bridge scopes, and theme tokens a third-party package panel uses (R-EDIT-001 v1
   hardening clause, now made concrete).

## 2. Layering

```
┌─ Context Editor app (one process = one project, D15) ─────────────────────────┐
│  SHELL (C++)                                                                  │
│   window manager (N native windows) · message/input pump · per-window         │
│   compositor (viewport RTs + CEF-OSR → swapchain) · CEF host (browser per     │
│   window; OnBeforePopup interception) · daemon lifecycle (spawn/attach)       │
│      │ hosts one editor-core instance per window                              │
│  EDITOR-CORE (TS web app in CEF)                                              │
│   Dockview docking + panel host · hydration runtime (uitree HTML → live DOM)  │
│   command registry + palette + keymap · theme engine (tokens) ·               │
│   bridge client (RPC + subscription consumer) · editor.ui event bus           │
│      │ hosts panels                                                           │
│  PANELS — all on the extended R-EDIT-001 contract                             │
│   built-in: hydrated from C++ uitree models (logic stays C++)                 │
│   third-party: sandboxed iframes, capability-scoped bridge, theme tokens      │
└──────── mutations via edit RPC (D22) ───── JSON-RPC + events ────────────────┘
                                       ▼
              EditorKernel daemon — performs the file writes → project files
```

**Trust zones** (detailed in 08): Shell = trusted native; editor-core = trusted first-party web
(no Node, strict CSP); built-in panel content = same zone as editor-core; third-party panels =
sandboxed iframes with capability-scoped bridge (R-EDIT-001 sandbox clauses).

## 3. Package map (in Context-Engine repo, D3)

| Unit | Language/kind | Contents | Links against |
|---|---|---|---|
| `src/editor/client/` → `context_client` | C++ static lib, **installed/exported** | wire plumbing (lifted from `wire_client`), subscription consumer (snapshot/delta/gap/re-snapshot, ack cursors, reconnect), typed envelope helpers | `context_bridge` (transport types), `context_contract` |
| `src/editor/shell/` → `context_editor` (exe) | C++ | window manager, input pump, per-window compositor, CEF host, daemon lifecycle, welcome screen host | `context_client`, `context_render` present API, CEF |
| `src/editor/webui/core/` → `@context-engine/editor-core` | TS (npm workspace, net-new) | Dockview shell, panel host + hydration, commands/palette/keymap, theme engine, bridge JS client (over a shell-provided IPC pipe) | published client schema (generated from `describe`); **npm: exactly one runtime dep — `dockview-core@7.0.2`** (MIT, 0 transitive deps; ratified by s1, owner-approved 2026-07-19, **version-pinned** — see 08 §3) |
| `src/editor/webui/tokens/` → `@context-engine/editor-tokens` | JSON + CSS | token schema, built-in Dark/Light/HC themes (monochrome-glow-ui port), viewport palette group | — |
| existing `src/editor/gui/*` | C++ | unchanged roles; `ExtensionRegistry` promoted to the real panel roster; panel models gain state-contract methods | — |
| engine-side additions | C++ | daemon fan-in (bridge), token auth, session-state verbs + topics, `ISurface/ISwapchain` + external-texture import + Camera/View (render). ⚠ **Amended 2026-07-19:** external-texture import ships **macOS-only** (stock native accessors, no fork); **Windows import is deferred** — CPU-upload ships, seam retained (03 §3) | — |

Boundary gates (D10): a CI job builds `context_editor` + editor-core against **installed**
`context_client`/`context_contract` artifacts as an out-of-tree consumer; include-graph check
forbids kernel-internal headers; editor-core's npm deps = published client packages only.

## 4. Core models

- **Window** = native OS window owning: a swapchain (via `ISurface/ISwapchain`), one CEF browser
  (editor-core instance), a compositor instance, an input pump binding. Window 0 is primary
  (holds app menu/welcome); all windows are peers for docking.
- **Layout tree** = Dockview state per window + window placement records; persisted (debounced +
  on-exit) in the editor-owned `.editor/editor-state.json` (session state per the L-20
  refinement — ownership split in 03 §1).
- **Panel instance** = (contribution id, instance id, state blob). Created/destroyed by the panel
  host; rehomed by serialize→destroy→recreate (D6).
- **Viewport** = a panel instance whose content region is native-rendered: the Shell allocates a
  per-viewport render target (Camera/View in the render world — new), composites it UNDER the
  CEF layer inside the panel's content rect (CEF paints UI chrome + transparent hole).
  N viewports, Scene|Game type (D5).
- **Command** = registry entry `{id, title, category, when-context, handler}`; sources: contract
  verbs (generated projection — the R-HUX-004 palette path via `help_model`-style projection),
  editor-core commands, panel-contributed commands (manifest). Keybindings map keys→command+when
  (per-user JSON file, hot-reloaded).
- **Event tiers** (D7): daemon topics (files/derivation/diagnostics/session/clients/log + new
  selection/camera/play payload extensions — 05 §4) for semantic facts; `editor.ui` local bus (same envelope
  discipline: seq, snapshot-on-subscribe) for UI chrome. Facts, never commands.
- **Theme** = versioned JSON token document (06); active theme = user preference; delivery =
  CSS custom properties in editor-core + token push into panel iframes.

## 5. Key flows (summaries; full sequences in 03/04/05)

1. **Boot**: shell starts → discovers/spawns daemon (child, D18) → attaches via `context_client`
   (token, scopes) → creates window 0 → editor-core loads → subscribes topics → restores layout
   from `.editor/editor-state.json` → panels hydrate → `derivation.settled` renders first
   stable state.
   No project → welcome screen (D13).
2. **Edit via inspector**: DOM gesture → hydration runtime → panel model (C++ via bridge) →
   gesture commit → `commit_override_write` over RPC `edit` with `--if-match` (D22) → daemon
   derives → events fan out → all windows/panels update. Undo replays the same path (R-HUX-001).
3. **Selection**: click in viewport → shell picks (ray/pixel) → `editor select` verb (05 §4) →
   daemon session state updates → `session` topic event → scene tree/inspector/agents all see
   it (D7).
4. **Tear-out**: drag tab past window bounds (or tear-out command) → PanelHost serializes the
   panel (D6) → Shell creates a native window + fresh editor-core → panel recreates from state
   (Dockview's popout API deliberately unused — 04 §2).
5. **Third-party panel**: package installed → manifest contribution registered (deny-by-default)
   → panel listed in layout targets/palette → content loads in sandboxed iframe → bridge shim
   with granted scopes → tokens injected → it docks/tears like any built-in.
6. **Theme switch**: user picks theme (or edits a watched `*.theme.json`) → engine validates by
   schema → CSS vars swap + `editor.ui.theme-changed` → iframes re-tokened; motion honors
   reduced-motion (D11).

## 6. Explicit non-goals (M9)

- No engine-side undo subsystem (L-21 stands; session undo only, R-HUX-001 scope).
- No full launcher/project manager (R-HUX-003 v2); welcome screen only (D13).
- No native Wayland (D21), no iOS/Android editor, no remote (networked) daemon exposure.
- No third-party NATIVE editor plugins (R-SEC-001 v1 posture) — package panels are web-content
  only, sandboxed; the hostile-extension red-team remains v2 (R-EDIT-001 v1 hardening scope,
  though the mechanical clamps ship and are enforced for what ships).
- No custom-CSS themes (tokens only, D11). No multi-project process (D15).
- Editor GUI on a GPU-less host: app runs, panels work — the UI presents via the CPU present
  path (03 §2); viewport panels show a diagnostic placeholder (no offscreen render available) —
  the headless-first law is not bent for M9.

## 7. Open questions (owner-facing)

| # | Question | Default until answered |
|---|---|---|
| ~~O1~~ | ~~Aurora-glow placement (the one signature flourish): Play button only, or also primary CTAs in welcome screen?~~ | ✅ **RESOLVED 2026-07-19 — and the question itself was the wrong one.** The owner reviewed live mockups and **rejected the aurora outright** (the rotating halo treatment, at BOTH placements), then picked a different flourish: **Pulse of Work** — see below |
| O2 | Editor app product name shown to users ("Context Editor"?) | "Context Editor" |
| O3 | Auto-update mechanism in v1 (none / notify-only / full auto-update)? | Notify-only banner reading GitHub Releases; full auto-update post-M9 |

**O1 resolution — the signature flourish is "Pulse of Work" (owner, 2026-07-19).** Not a
placement choice but a different treatment: the glow's **colour and rhythm mirror the Play
button's real state**, reusing the already-reserved status hues — so the signature flourish and
the status signal become one thing, at the cost of **zero new colour tokens**. Guiding principle:
**animation speed tracks activity level** (active work fastest, at rest slowest). Full spec —
state→hue map, the five rhythms, and the bloom mechanism — lives in
[`mockups/TOKENS.md` §5](mockups/TOKENS.md); the aurora tokens (`aurora-a/b/c`) are retired to
historical reference. Consumed by e06; summarized in [06 §2](06-theme-design-system.md).

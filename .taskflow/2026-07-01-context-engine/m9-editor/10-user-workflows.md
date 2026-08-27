# 10 — User workflows (UX contract, counted)

Step budgets are RELEASE GATES, wired into the M9 exit gate as clause 7 (09 §5): exceeding one
at M9 exit = a blocking finding unless owner-waived. **"Step" = one interaction: a click, a
drag, or one contiguous typed entry** (C-F9); worst honest path named.

## Persona A — human game developer (primary)

| Flow | Budget | Path |
|---|---|---|
| Fresh install → editing a template project | ≤ 7 steps | run installer (1-2) → launch (1) → welcome: New from template (1) → pick template + location (2) → project name (1 typed entry; defaults from folder = 0 when accepted) → editor opens |
| Open recent project | 2 steps | launch (1) → recent item (1). File-association path: double-click project = 1 |
| Edit a component field | 3 steps | select entity (viewport or tree, 1) → focus field (1) → typed entry+commit (1); undo = Ctrl+Z (1) |
| Rearrange UI: dock a panel elsewhere | 1 drag | tab → drop zone (keyboard path exists via move-panel commands, ≤4 keys) |
| Tear a panel into its own OS window | 1 drag (or 1 command) | tab → outside window; or palette "Move panel to new window" |
| Open a second viewport (Scene) on another monitor | ≤ 4 steps | open palette (1) → typed query (1) → confirm (1) → tear/drag to monitor (1) |
| Switch theme | ≤ 3 steps | Settings/palette → theme picker → pick; live, no restart |
| Install a custom theme | ≤ 4 steps | put file in themes dir (1) → switch-theme flow (≤ 3); hot-reloads on edit (0) |
| Enter play mode and pause | 1 + 1 | Play (~~aurora~~ **Pulse-of-Work** button — amended 2026-07-19, 06 §2: the flourish's colour+rhythm mirror the button's live state) → Pause; loud play-mode indicator (L-51) |
| See and fix an error | ≤ 5 steps | status badge click (1) → Problems row (1) → click-to-navigate selects the entity (1) → focus field (1) → typed entry+commit (1) |
| Worst path (honest): first-ever launch on macOS unsigned-dev build | +2 (Gatekeeper) — release builds are notarized so this is dev-only |

## Persona B — package developer

| Flow | Budget | Path |
|---|---|---|
| Scaffold a panel extension | ≤ 3 steps | `context new --template extension-panel` → manifest + hello iframe ready |
| See own panel in the editor | ≤ 3 steps | `context package add ./my-ext` (1) → consent prompt shows requested scopes (1) → panel appears in palette/dock targets (1) |
| Iterate on panel UI | 0 extra | iframe content reload command; state contract preserves panel state across reloads |
| Ship a theme with the package | manifest entry | `themes:[…]`; appears in picker under package name |

## Persona C — AI agent (contract parity check)

| Flow | Guarantee |
|---|---|
| Observe what the human selected / looks at | `session`-topic selection/camera events + `editor selection get` (05 §4) — no GUI needed |
| Co-edit while human edits | same write path, CAS + rebase-or-drop; human sees drop-loudly notices (wait hue) |
| Drive the editor UI itself (optional) | every UI capability is a command; commands are introspectable — the palette surface ≡ scriptable surface |
| Verify UI state in CI | T2 CDP + command-driven assertions (09) — the agent-facing story and the test story are the same surface |

## Non-negotiable UX invariants

- No Save button anywhere (L-20); gesture-end commits + session undo (R-HUX-001).
- Every pointer capability has a keyboard/command path (R-A11Y-001, R-CLI-001 structural).
- Destructive/lossy moments (gesture drop, daemon lost, panel crash) are LOUD (wait/bad hues),
  never silent (L-30 discipline surfaced in UI).
- Play-mode state is unmistakable (L-51 indicator); runtime mutations never touch files.
- The editor never blocks on derivation: provisional states render with `stability` styling
  (R-HUX-005 / R-BRIDGE-008).

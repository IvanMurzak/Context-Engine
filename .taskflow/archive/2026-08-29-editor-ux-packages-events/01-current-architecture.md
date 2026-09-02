# 01 — Current architecture: verified behaviour and change seams

Everything below was read out of the tree at `main` on 2026-08-29. Claims carry `file:line`. Where a
claim is an inference rather than a read, it says so.

---

## 1. The CEF host implements 5 of `CefRenderHandler`'s 17 members

`ShellCefClient` (`src/editor/shell/cef/src/cef_shell.cpp:653`) derives `CefClient`,
`CefRenderHandler`, `CefLifeSpanHandler`, `CefLoadHandler`, `CefDisplayHandler` and
`CefRequestHandler`. It does **not** derive `CefContextMenuHandler` or `CefDragHandler`.

Of `CefRenderHandler`'s 17 virtuals (enumerated from the pinned SDK header at
`src/build/editor/_cef/x86_64-pc-windows-msvc/include/cef_render_handler.h`), the host overrides
**five**:

| Member | State | Consequence of the gap |
|---|---|---|
| `GetViewRect` | ✅ `cef_shell.cpp:762` — reports DIP | |
| `GetScreenInfo` | ✅ `:770` — `device_scale_factor` + the per-platform DIP/device rect split | |
| `OnPopupShow` | ✅ `:792` | |
| `OnPopupSize` | ✅ `:802` | |
| `OnPaint` | ✅ `:814` | |
| `OnAcceleratedPaint` | ⛔ deliberate — owner ruling 2026-07-19, recorded in `cef_shell.h` | none |
| **`GetScreenPoint`** | ❌ **not implemented** | The default returns `false`, so CEF treats **view** coordinates as **screen** coordinates. This is the reported context-menu offset, exactly |
| `GetRootScreenRect` | ❌ | Same family — it is the other input to menu/popup placement |
| **`StartDragging`** | ❌ | Its default returns `false`, which the header defines as **"abort the drag operation"**. Every HTML5 drag in the editor is *actively refused* |
| `UpdateDragCursor` | ❌ | No drag feedback cursor |
| `OnImeCompositionRangeChanged` | ❌ | Composition-based text input has no candidate-window placement |
| `OnVirtualKeyboardRequested` | ❌ | |
| `OnTextSelectionChanged` | ❌ | |
| `GetAccessibilityHandler` | ❌ | **In OSR, native accessibility is not automatic.** The `gui-a11y-*` gates assert the C++ models and the DOM and are honestly green; an OS-level screen reader nonetheless sees nothing in this window. Notable for a repo with R-A11Y-001 and a blocking a11y gate |
| `OnScrollOffsetChanged`, `GetTouchHandleSize`, `OnTouchHandleStateChanged` | ❌ | Not needed today; must still be *decided*, not merely absent |

`CefBrowserHost`'s windowless-only drag family — `DragTargetDragEnter` / `DragTargetDragOver` /
`DragTargetDragLeave` / `DragTargetDrop` (`cef_browser.h:889-927`) / `DragSourceEndedAt` /
`DragSourceSystemDragEnded` (`:930-946`), each documented *"only used when window rendering is
disabled"* — has **no call site anywhere in `src/`**.

**This is the set's central finding**: the OSR contract was adopted partially and never walked as a
list. Task `a0` closes that.

## 2. The popup layer is composited in the wrong coordinate space

`OnPopupSize` delivers the rect in **DIP**; it is stored raw:

- `compositor.cpp:329` — `popup_rect_ = rect;`

and then used **directly as a destination rect in the physical-pixel backbuffer**:

- `compositor.cpp:539` — `draw_layer(*pass, popup_rect_, *popup_layer, popup_visible_rect_, popup_coded_size_, …)` (GPU path)
- `compositor.cpp:632-641` — the CPU present path clamps against `popup_rect_.size` and blits at `popup_rect_.origin`

while the popup **texture** arrives from `OnPaint` in physical pixels — a fact the file states about
itself at `compositor.cpp:581`: *"on a non-integral DPI scale the browser paints ceil(DIP × scale)"*.

At the owner's 150 % scale the popup is therefore drawn at 1/1.5 of its correct offset and cropped.
**One bug produces both reported symptoms**: the user clicks the *drawn* popup, CEF hit-tests against
the *true* DIP rect elsewhere, the click lands outside, and CEF closes the popup without selecting.
Keyboard selection works because the popup widget's focus is genuine.

It is invisible at scale 1.0, which is every CI leg (Session-0 runners), which is why
`editor-shell-test_compositor` is green and `docs/shell.md` §10 still carries the `PET_POPUP` row as
manual and unverified. **A regression test for this must run at a scale ≠ 1** — a test at 1.0 cannot
fail on it.

Note the pointer path is *not* implicated: `cef_shell.cpp:1256-1266` sends
`dispatch.logical_position` (DIP) and says so. Input is correct; the composite is not.

## 3. Dockview drag needs HTML5 DnD, which OSR has switched off

The pinned `dockview-core@7.0.2` bundle uses native HTML5 drag-and-drop — verified by inspecting the
staged artifact directly: 12 `draggable`, 1 `dragstart`, 3 `dragover`, 5 `dragend`, 11 `dataTransfer`,
3 `setDragImage`. Dockview's configuration is fine and floating groups are on
(`panelhost.ts:1034-1041`, `disableFloatingGroups: false`).

So tab drag, drop-to-split into any edge, and re-docking are all **already implemented by Dockview**
and will work the moment the drag events reach it. Nothing in `panelhost.ts` needs to change for them.

The existing **cross-window** drag (`src/editor/shell/include/context/editor/shell/cross_window_drag.h`,
`drag.ts`, ctest `editor-cef-smoke-shell-drag`) is a *different, Shell-mediated* mechanism and must keep
working; `drag.ts:19-21` records that in-window Dockview DnD is deliberately untouched by it.

## 4. Panel identity is the manifest id, so `singleton` is inert

- `panelhost.ts:808` — `readonly #panels = new Map<string, HostedPanel>()`, keyed by manifest id.
- `panelhost.ts:1091` — `if (this.#panels.has(manifest.id)) { return false; }` — this fires for
  **every** panel regardless of `dock.singleton`. The manifest flag is decorative today.
- `panelhost.ts:1113` / `:1122` — `open` reports success by the same key; `openById` is the e10b
  tear-out seed path.
- C++ `panel_host.h:168-254` keys **every** operation by `panel_id`: `provide`, `knows`, `hosts`,
  `touch`, `revision`, `render`, `invoke`, `gesture`, `get_state`, `restore_state`, `find`,
  `resolve_hosted`. A provider binds **one** model, so a second instance has nothing to render from.

`Contribution` / `DockDefaults` (`src/editor/gui/contract/include/context/editor/gui/contract/extension.h`)
carries `{default_zone, singleton, min_width, min_height}` — **no instance mode, no maximum, and no
`path`/`category`** for menu grouping. `kContractMajor` is **2**, and the compatibility window is
exactly one major (a contribution declaring another major is refused).

## 5. There is no surface that opens a panel by name

- The only open command is the hardcoded `view.panel.open.settings` (`menu.ts:76`).
- `menu.ts:290` — the **Panel** menu holds tear-out and move-panel actions.
- `menu.ts:305` — the **Window** menu holds minimize / maximize and a list of **OS windows**
  (`windowListEntries`, labels `"Window 1"`, `"Window 2"`). This is where the owner looked.
- A reusable fuzzy matcher already exists and is exported: `palette.ts:102`
  `fuzzyMatch(query, candidate)` — case-insensitive subsequence, scoring consecutive runs, word
  starts and an early first match, returning `{score, positions}` for highlighting.

## 6. The viewport panel is rostered, unhosted, and is a text summary

- `src/editor/gui/contract/src/builtin_roster.cpp:95` — `builtin.viewport`, zone `center`,
  **singleton `false`**, min 320×240, `read_query`.
- `src/editor/shell/panels/src/builtin_panels.cpp:555-577` — `hostable_panel_ids()` contains only
  `placeholder`, `builtin.problems`, `builtin.scene-tree`, `builtin.inspector`, `builtin.session.undo`.
  The viewport is therefore reported `hosted:false` and refused by `PanelHost.#mountable`.
- `src/editor/gui/viewport/include/.../viewport_model.h` — the model summarises the extracted snapshot
  (`drawables`, `directional_lights`, `point_lights`) plus a present outcome. It is an **observer
  summary, not an image**.

**But the compositor half of a real viewport already exists and is tested:**

- `compositor.h:85` — `ViewportLayer { id, content_rect (PHYSICAL px), content (ITextureView*), content_size }`.
- `compositor.cpp:341` `publish_viewports(...)`, `:494` the per-layer composite loop, drawn beneath the
  CEF layer through its transparent-hole regions.
- `input.h:64-92` — `ShellRegion { id, rect (PHYSICAL client px), kind }` and `RegionMap` with
  back-to-front hit-testing, `RegionKind::viewport` already in the vocabulary.

**Nothing in production calls `publish_viewports`** — the only callers are
`src/editor/shell/tests/test_compositor.cpp:240,331,371`. `input.h:88-91` says so outright: *"nothing
consumes it yet … This is the seam e11's viewport-content damage path is expected to read."*

So the missing work is the producer, the DOM hole, the rect report and the camera/picking — **not** the
composite and **not** the input routing.

## 7. There is no Files/Project panel, and the D10 boundary dictates how one must be built

No file-browser panel exists under `src/editor/gui/`. The data is daemon-side: `src/editor/assetdb/`
(`asset_database.h` — the bounded path/guid/kind index built lazily from sidecars; `move_asset` in the
R-FILE-004 dependency-safe order, idempotent, refusing occupied destinations) and `src/editor/filesync/`.

The D10 boundary gate (`context_assert_shell_boundary`, the `editor-boundary` job) FATAL_ERRORs at
configure time on any EditorKernel internal in the Shell's link closure, so **the Shell cannot link
`assetdb`**. A Files panel therefore needs a new daemon read verb, exactly as Scene Tree and Inspector
did — `registry.cpp:939` `editor scene-tree`, `:949` `editor inspect`.

## 8. The event architecture: two tiers, already shipping

### Tier 1 — the daemon, topic `session`

Verbs in the one registry: `registry.cpp:976` `editor select {ids, mode}`, `:989`
`editor selection-get`, `:995` `editor camera-set`, `:1007` `editor cameras-get`, `:1013-1034` the play
family. All operational, all `session_control` scope. Facts: `selection-changed {ids, mode, origin}`,
`camera-changed {viewportId, origin}`, `play-state {state, simTick, origin}`.

Three rules make it loop-safe, and knowing *which one does the work* matters for this set:

1. **`origin` echo suppression** — a consumer applies a fact whose origin differs from its own and
   drops one that matches. Ids are minted **per wire connection**.
2. **A no-op publishes nothing** — re-selecting the same ids emits no event. *This is the actual cycle
   breaker*, and it is state-based.
3. **The writer sees its own change through the REPLY, never the fact.**

Persistence: `.editor/session.json`, daemon is the single writer.
`editor_session_state.cpp:250` writes `version`; `:262-266` treats a **future** version, a non-number
or `< 1` as **corrupt** — quarantine aside plus defaults plus a loud `editor.session_state_invalid`
diagnostic. There is **no migration branch** for an older version today — and note that an older
version is **not** refused either: it passes the check untouched and every member is then read under an
`if (doc.contains(…))` guard (`:269-289`), the additive absorption `kSessionFileVersion`'s header
describes at `:26-28`. So a document written to an older shape is silently accepted with the members it
no longer carries simply missing. See `08` §3.

**Hierarchy → Inspector already runs on this in production**: `session_feed.cpp:117`
(`scene_tree_->apply_selection(read_ids(payload))`), `:150` (`request_selection` → `editor.select`),
and `inspector_feed.cpp` hydrates off the resulting selection listeners.

⚠ `session_feed.cpp:111` is the **sole** consumer of the `selection-changed` fact and it applies it
**unconditionally**. Introducing a typed subject without filtering it feeds a file selection into
`SceneTreePanel::apply_selection` as L-35 entity id-paths. The failure is silent.

The Inspector is **not** a second consumer — it is downstream: `builtin_panels.cpp:667-690` wires it to
`SceneTreePanel::add_selection_listener`, so filtering `session_feed` protects it transitively. Its own
share of typed selection is D3 (render the *focused* subject), which is a new `selection-focus`
consumer, not a filter.

### Tier 2 — the `editor.ui` bus, window-local chrome

`uibus.ts` — nine closed topics (`focus`, `layout`, `drag`, `viewport`, `theme-changed`, `palette`,
`write-notice`, `chrome`, `menu`) plus package topics declared in the manifest and namespaced under the
package id. The D7 boundary "chrome never reaches the daemon" is held by ctest `webui-uibus-boundary`
(`tools/check_ui_bus_boundary.py`) and by `uibus.test.ts`. Five of the nine topics have no publisher yet.

📌 `docs/editor-ui-bus.md` records an **open gap this set should close**: `panel.daemon.call` now
exists, so the boundary checker owes it a deny-list entry, *"which is not yet implemented — so today
the rule is enforced by review rather than by the gate."*

### Package fan-out

`bridge.events.subscribe` / `.unsubscribe` / `.ack` (`panelverbs.ts:185-197`) → the package's own
baseline daemon session → a bounded per-package Shell buffer (`package_events.h`) →
`panel.events.poll` → `PackageEventPump` (`packageevents.ts`) → pushed into the iframe, carrying loud
`dropped` / `gapped`.

### The two gaps that block the owner's requirement

1. **Cross-package subscription is refused by an explicit decision.** `packageui.ts:30-34`: *"Only
   `BUILTIN_UI_TOPICS` are subscribable. A package's OWN declared topics are a PUBLISH surface — letting
   a package subscribe to another package's namespaced topic is precisely the [escalation refused]
   because the OTHER package never agreed."* `isSubscribableUiTopic()` returns
   `BUILTIN_UI_TOPICS.includes(topic)`.
2. **The selection fact carries only L-35 entity id-paths.** There is no subject type, so a Files panel
   cannot express "a file is selected" and the Inspector cannot be asked to inspect one.

There is **no verb by which a package can publish onto the daemon bus at all** today.

### The grant machinery a consented subscription plugs into

`packagegrants.ts` + `package_grants.h`: `package.grants.list` / `package.grants.decide`, reading a
document under `~/.context/` that no package can write, **clamped to what that package's manifest
declared**, fail-closed in every direction, snapshot-at-boot. The namespacing validators to mirror are
`validatePackageTopic` (`uibus.ts`) and `validatePackageCommandId` (`panelverbs.ts:355`).

## 9. The kit's widget layer has three hover rules and no other state

`src/editor/webui/kit/styles/kit.css` styles the twelve closed `uitree::Role` widget classes — the
content of **every C++-modeled panel**. Measured:

- `:hover` — **3 rules**: `:94` listitem, `:95` treeitem, `:115` button.
- `:active` — **0**. `:disabled` / `[aria-disabled]` — **0**. `transition` — **0**.
- `:focus-visible` — all **12** roles (`:163-174`). This is correct and must survive.

The motion tokens exist and are used one layer over, in the kit *component* stylesheet:
`components.css:52,149,212` use `var(--ctx-motion-duration-fast) var(--ctx-motion-easing-standard)`.
`components.css:55` is the one `:disabled` rule in the kit.

Constraints on any change here:

- ctest **`webui-kit-tokens-only`** rejects a raw literal anywhere in `kit.css`, *including inside a
  `var()` fallback*.
- ctest **`webui-kit-role-coverage`** requires every `Role` to be styled here and no `.ctx-widget-`
  rule to survive in `app.css`.
- `prefers-reduced-motion: reduce` collapses every motion token unconditionally
  (`theme.ts:306-335`), and `app.css:163` re-asserts it.
- The Dockview tab strip is themed through `html .dockview-theme-dark` at `app.css:667` — read the
  ⚠⚠⚠ specificity note above it before touching it; the `html` prefix is load-bearing because
  `dockview.css` declares the same variables on the same selector.

## 10. Every panel prints its own title, and Dockview prints it again

`Role::heading` nodes carrying the panel's own title, emitted as the first child of the panel root:

| Panel | Site | Heading text |
|---|---|---|
| Scene Tree | `scene_tree_panel.cpp:194` | `"Scene Tree"` |
| Inspector | `inspector_panel.cpp:298` | `"Inspector"` |
| Problems | `problems_panel.cpp:239` | `"Problems"` |
| Viewport | `viewport_panel.cpp:144` | `"Viewport"` |
| Viewport Edit | `viewport_edit_panel.cpp:74` | `"Viewport Edit"` |
| Help | `help_panel.cpp:60` | `"Help"` |
| Session History | `undo_journal.cpp:492` | `"Session History"` |
| Tilemap Painter | `tilemap_paint_panel.cpp:60` | ⚠ **none** — `set_label` only, no `set_text` |
| placeholder | `builtin.cpp:20` | `"Context Editor"` |

Eight of the nine set a heading **text** equal to their roster title, so the duplication is real and
visible on those eight. The ninth, Tilemap Painter, sets only an accessible label, so it renders an
empty `<h2>` and shows no visible title at all — `a4`'s text-equality rule correctly does nothing
there, and that is the right outcome, not a miss. (`playbar_panel.cpp:80` is a tenth heading site, out
of scope: the playbar was retired from the dock roster.)

Dockview separately renders `manifest.title` into the tab (`panelhost.ts:1100`).

The heading is **not** decorative: `role_requires_name(Role::heading)` is true
(`uitree/tests/test_node.cpp:30`), it renders as `<h2>` (`:43`), headless and CLI consumers read the
same model, and `gui-a11y-coverage` gates the roster. Hence the planner ruling to hide it in the
renderer rather than delete it.

## 11. Standing repository constraints any task here must respect

- Build files live in `src/`: `cmake -S src --preset dev`, then build/test **from `src/`**.
- **Tests are part of the feature (R-QA-013)** — behaviour and its tests merge in the same PR.
- **Adding a built-in panel is a FOUR-anchor edit** guarded by two *different* ctests: roster entry +
  a11y factory + `coverage.manifest.jsonl` line (`gui-a11y-coverage`) and the `help::panel_topics()`
  entry (`gui-help-contextual`, plus the `m85-exit-4c` gate). A `local`-content panel skips the C++
  a11y factory **and must declare `ts-a11y`** in its coverage line.
- **"Not Run = RED"**: `deterministic`, `wasm-runner`, `editor-cef-smoke` and `editor-boundary` build
  hand-maintained `--target` lists. A new ctest needs both the target *and* the `ctest -R` match.
- **D10 boundary**: adding a library to the exported install set means adding it to
  `editor-boundary`'s `--target` list; `cmake --install` fails on a target never built.
- Wall-clock budgets must be sanitizer-aware in the same PR (`CONTEXT_TSAN_BUILD`).
- **CEF work has no local compile signal.** The Windows dev gate resolves to Ninja + GCC; V8, wgpu,
  CEF and wasmtime are default-OFF MSVC/Clang-ABI prebuilts. CI is the authority. MSVC `/W4 /WX`
  rejects raw C stdio; Clang enables unused-const/capture/private-field warnings GCC does not;
  non-Windows `#if` branches get zero local signal.
- CEF-linking targets are exempt from `context_warnings`; their headless dependencies are not.
- Conventional commits; cite `R-`/`L-` ids in PR bodies. The normative design authority lives
  **outside this repo**; never contradict a lock — surface the conflict.

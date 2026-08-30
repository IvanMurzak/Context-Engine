# 02 — Target architecture

What the editor looks like when this set is done, and what each decision bought and cost.

---

## The shape in one page

```
                        ┌──────────────────────── the daemon (tier 1) ───────────────────────┐
                        │  topic "session"                                                   │
   CLI ─────────────────┤    selection-changed { subject, ids, mode, origin }   ← NEW subject │
   AI agent ────────────┤    selection-focus   { subject, origin }              ← NEW fact    │
   window A ────────────┤    camera-changed / play-state                                     │
   window B ────────────┤                                                                    │
   package X ───────────┤  topic "<pkg>.<name>"  (package facts — STATE, last-value deduped)  │
   package Y ───────────┤    declared in the manifest, subscribed under a consented grant     │
                        └────────────────────────────────────────────────────────────────────┘
                                              ▲ verbs                    │ facts
                                              │                          ▼
   ┌────────────────────────────── one editor window ──────────────────────────────┐
   │  Shell (C++)   compositor: [ viewport layers ] under [ CEF layer ] + [ popup ] │
   │                RegionMap: pointer → viewport region or → CEF                   │
   │                OSR contract: paint · geometry · DRAG · screen-point            │
   │  ────────────────────────────────────────────────────────────────────────────  │
   │  editor-core (TS)   Dockview  ·  PanelHost keyed by (panelId, instanceId)      │
   │                     editor.ui bus (tier 2, chrome only — never leaves)         │
   │  panels: Hierarchy · Inspector · Files · Scene · Problems · Settings · …       │
   └───────────────────────────────────────────────────────────────────────────────┘
```

Two rules keep it acyclic and they are the whole design:

1. **Packages never name each other.** They name *topics* and *subject kinds*, which the contract
   registry owns. Every dependency edge points at the registry, so the graph cannot contain a cycle
   between packages by construction.
2. **The bus carries facts; verbs carry intent.** "Select this" is a verb call; "the selection is now
   X" is a fact. Nothing on a bus dispatches. The owner's example — clicking something in the
   Inspector selects an object in Hierarchy — needs no new channel: the Inspector calls
   `editor.select`, and the resulting fact drives everyone else.

---

## A. The OSR contract, walked as a list (D12)

The editor stays on CEF windowless. The change is that the mode's contract stops being adopted
piecemeal. Task `a0` produces a **conformance table** — every `CefRenderHandler` member, every
windowless-only `CefBrowserHost` method, and `CefContextMenuHandler` — each marked *implemented*,
*deliberately not needed (with the reason)*, or *gap (with a task id)*. That table lands in
`docs/shell.md` and is the thing a future reader consults instead of rediscovering a hole.

Known gaps it will register, beyond the two fixed in this set:

- `GetAccessibilityHandler` — in OSR, OS-level accessibility is not automatic. The `gui-a11y-*` gates
  are honestly green about the models and the DOM and say nothing about a screen reader. This is
  logged as a follow-up with its own scope, **not** silently absorbed here.
- The IME family (`OnImeCompositionRangeChanged`, `OnVirtualKeyboardRequested`) and
  `OnTextSelectionChanged`.

**Trade-off taken:** a conformance audit costs a task that ships no user-visible change. It buys the
end of finding these one at a time — which, on the evidence of this very report, is a real cost
already being paid.

### The two geometry fixes

- **`GetScreenPoint`** (+ `GetRootScreenRect`) implemented, converting view DIP → screen coordinates
  through the window's live placement. `dpi.h`'s `osr_screen_extent` already establishes the
  per-platform DIP-vs-device convention split and the discipline of putting that arithmetic in `dpi.h`
  so all three legs compile and test it; the new conversion follows it.
- **The popup rect converted DIP → physical** at the compositor seam, on **both** present paths, with
  the destination size derived from the physical texture rather than the DIP rect.

**The gate that matters:** a regression test at a scale **≠ 1**. A test at 1.0 is vacuous here — the
bug is a missing multiply by the scale factor, so at scale 1.0 the correct and the broken code produce
identical output. `editor-shell-test_compositor` is the home; the existing popup cases stay and a
scaled sibling is added.

### Drag

`StartDragging` + `UpdateDragCursor` on the render handler, and the
`DragTargetDragEnter/Over/Leave/Drop` + `DragSourceEndedAt` + `DragSourceSystemDragEnded` injections
driven from each platform's OS drag manager: OLE (`IDropSource`/`IDropTarget`/`DoDragDrop`) on Win32,
XDND on X11, `NSDraggingSession` on Cocoa. CEF's own `cefclient` sample carries reference
implementations upstream (`tests/cefclient/browser/osr_dragdrop_win.cc`, `_x11.cc`) — **not vendored in
our minimal distribution**, but public prior art, so this is porting rather than research.

Once the events arrive, **Dockview supplies tab drag, drop-to-split into any edge, and re-docking with
no editor-core change** — they are already implemented in the pinned bundle.

The existing Shell-mediated **cross-window** drag must keep passing
(`editor-cef-smoke-shell-drag`); the two mechanisms are layered, not alternatives, and `drag.ts`
already states the split.

---

## B. Selection: typed, per-subject, with a tier-1 focus (D1, D2, D3)

```
editor select --subject <kind> --ids <id…> --mode replace|add|toggle|remove
editor selection-get [--subject <kind>]      →  { ids, selections: [ { subject, ids }, … ] }
editor selection-focus-get                   →  { subject }

fact  selection-changed { subject, ids, mode, origin }
fact  selection-focus   { subject, origin }
```

`subject` is **optional, defaulting to `entity`**, so the change is additive and `protocolMajor 1`
does not move. **The reply is additive too, and that is a REVISED decision** (`D1`, 2026-08-29): the
existing `ids` member stays and continues to carry the `entity` selection, and the typed view arrives
as a NEW `selections` member — an **array of objects carrying their key**, never a map-keyed object,
matching the authored-data convention the camera array already follows. Replacing the reply with a
bare array would have broken every existing reader silently, including `attach_command.cpp:157`,
which is why the additive form was taken. `--subject` narrows `selections` (and `ids` with it); it
never changes the reply's shape.

**Selections are independent.** Selecting a file does not clear the entity selection (D1). That is why
`selection-focus` exists (D3): it is the answer to *"which of the live selections is the human
actually working on"*, and it lives in the daemon precisely so a CLI client or an agent can read it.
The Inspector renders the focused subject; Scene highlights only `entity`; Files highlights only
`file`.

**The subject vocabulary is open** (D2): `entity`, `file`, `asset` are contract-owned; a package
declares `<pkg>.<kind>` in its manifest and the registry validates the namespacing with the same
discipline `validatePackageTopic` and `validatePackageCommandId` already apply.

### The mandatory migration inside this change

`session_feed.cpp:111-124` is the **sole** consumer of the `selection-changed` fact, and it applies it
unconditionally. It must filter `subject == "entity"` **in the same PR**, or a file selection is fed
to `SceneTreePanel::apply_selection` as L-35 entity id-paths. This failure is silent, so it gets a
test that publishes a `file` fact and asserts the scene tree did **not** move.

`inspector_feed.cpp` is **not** a second filter site: the Inspector never sees the fact. It is driven
by `SceneTreePanel::add_selection_listener`, wired at `builtin_panels.cpp:667-690`, so it is protected
transitively the moment `session_feed` filters. What the Inspector *does* owe this task is D3: it
renders the **focused** subject, which means a new `selection-focus` consumer beside that listener —
a different change from the filter, in a different place.

**Trade-off taken:** independent selections match Unreal and match how a Files panel actually wants to
behave, at the cost of needing a focus arbiter that a single shared selection would not have needed.
The arbiter is one additional fact.

---

## C. Package facts on daemon topics (D4, D5)

A package's manifest gains:

```jsonc
"events": {
  "publishes":  ["acme.tilemap.brush"],      // must be namespaced under the package id
  "subscribes": ["other.pkg.something"]      // another package's topic ⇒ a consented grant
},
"selection": { "subjects": ["acme.tilemap.tile"] }
```

- **Publishing** is a new operational verb, refused unless the topic is declared by *that* package and
  correctly namespaced.
- **Subscribing to another package's topic** is an install-time consented grant, deny-by-default,
  riding the existing `package.grants.list` / `package.grants.decide` machinery — the same document
  under `~/.context/` that no package can write, clamped to what the manifest declared.
- **Delivery reuses the shipping path** end to end: the bounded per-package Shell buffer,
  `panel.events.poll`, `PackageEventPump`, and the loud `dropped`/`gapped` pair. No new transport.
- **`describe` parity (R-CLI-013)**: package topics are introspectable, so the CLI and an agent can
  discover them like any other contract surface.

### D5 is the load-bearing rule, and here is why

The shipping cycle breaker is **"a no-op publishes nothing"** — daemon-side state dedup. It is *not*
`origin`: two packages hold separate baseline daemon sessions and therefore different origins, so
origin suppression does nothing for an A → B → A mirroring loop. Arbitrary package topics have no
state to dedup against.

So a package fact is defined as a **state**, not an edge:

- the daemon **retains the last value per topic** and refuses to publish a repeat of it — the identical
  rule that already protects `selection` and `camera`;
- that retention doubles as **snapshot-on-subscribe**, matching the `editor.ui` bus's model, so a panel
  opened later immediately sees current state instead of waiting for the next change;
- **publishing from inside an event handler is refused** (a reentrancy guard), which turns the
  remaining loop shape into a diagnosable refusal instead of a hang.

**Trade-off taken and stated plainly:** a package cannot send pure edge events — "the button was
pressed twice" is not expressible, because the second press deduplicates against the first. A package
needing that must model it as state (a counter, a timestamped token). This is the price of a
broadcast bus that cannot be made to loop, and it was accepted knowingly.

**Why not the `editor.ui` bus:** it is window-local. Facts on it are invisible to the CLI, to agents,
and to a second window unless mirrored — and routing semantic facts there would erode the D7 boundary
that two ctests exist to defend.

---

## D. Panel instances and the manifest (D6)

`kContractMajor` **2 → 3**. The manifest's dock block changes shape:

```jsonc
"dock": { "defaultZone": "right", "minWidth": 280, "minHeight": 200 },
"instances": { "mode": "singleton" | "limited" | "unlimited", "max": 4 },
"path": "Scene/Debug"          // slash-separated; the Window menu turns it into a tree
```

- `mode: singleton` — a second open **focuses** the existing instance.
- `mode: limited` — refused past `max`, and the menu entry renders disabled with the reason.
- `mode: unlimited` — each open mints a new instance.

Runtime: `PanelHost` (both sides) rekeys from `panelId` to **`(panelId, instanceId)`**; the C++ panel
provider becomes a **factory** producing one model per instance. `instanceId` joins the wire on
`panel.render` / `panel.command` / `panel.gesture` / `panel.state.get` / `panel.state.set`, and the
`webui-panel-contract` byte-compare gate moves in lockstep with the C++ constants.

D6 state persistence, Dockview panel ids, layout restore, and the e10b tear-out/rehome path all key by
instance. **a11y coverage stays keyed by panel KIND** (planner ruling): auditing N copies of one model
proves nothing the first copy did not.

**Trade-off taken:** this is the largest structural change in the set and it breaks the manifest for
every in-repo consumer at once. It is taken *now* because the compatibility window is exactly one
major and there are no out-of-repo consumers yet — the same reasoning that made the 1 → 2 bump safe.
After v1 ships, the identical change costs a deprecation cycle.

---

## E. The Window menu (D9)

**`Window` opens things. `Panel` acts on the focused one.**

`Window` becomes: a search field, then the panel tree built from `path` (slashes → nesting), then a
subsection listing the OS windows that live there today. `Panel` keeps tear-out and the move actions.

Search matches **every path segment and the panel name simultaneously**, so `dbg tile` finds
`Scene/Debug → Tilemap Painter`. It reuses `fuzzyMatch` from `palette.ts:102` rather than growing a
second matcher — the palette's scoring already rewards consecutive runs and word starts, which is
exactly the ranking a path search wants.

Instance rules are visible in the menu, not merely enforced on click: a singleton already open reads
as "focus", a `limited` panel at its maximum renders disabled with the reason in its tooltip — the
honest-degrade rule the menu already follows for the ⏳ rows.

---

## F. The Scene viewport and the Files panel (D7, D8, D10)

### Scene viewport

The compositor and input-routing halves already exist (see `01` §6). What this set adds:

1. a **producer** that renders the scene into an `ITextureView` sized to the panel's rect and calls
   `publish_viewports`;
2. a **hole** in the DOM — the panel's element is transparent so the layer beneath shows through —
   which pins its Dockview renderer to `"always"` (an `onlyWhenVisible` panel is detached from the DOM
   and its rect becomes meaningless);
3. the panel's live rect reported into `RegionMap` on every layout change, so pointer events over it
   route to the viewport instead of to CEF;
4. **camera** driven through the existing `editor.camera-set` / `cameras-get`, persisted where cameras
   already persist;
5. **picking** as a CPU raycast against `render::RenderSnapshot`, answering `editor.select` with
   `subject: "entity"` — which then propagates to Hierarchy and Inspector through the fact everyone is
   already listening to.

**Trade-off taken (D8):** a CPU raycast is less exact at silhouettes than a GPU id-buffer, and it is
chosen because it is assertable on all three build legs with no GPU. A GPU path can be added later as
an accelerator differentially verified against the CPU reference; it is not in this set.

### Files panel

A new daemon read verb — `editor files` — following `editor scene-tree` / `editor inspect` exactly,
because the D10 boundary forbids the Shell linking `assetdb`. The panel publishes `subject: "file"`
selections and consumes `selection-focus`.

**Full file manager (D10):** rename / move / delete over the existing `AssetDatabase::move_asset` and
the `asset move` / `asset rename` verbs, which already implement the R-FILE-004 dependency-safe order,
idempotence under partial apply, and refusal on an occupied destination. What the panel adds is the
authoring surface: the `file_write` grant, the L-30 write path, undo journal entries, and loud refusal
notices on the existing `editor.ui.write-notice` topic.

**Trade-off taken:** entering the write path is where most of this panel's risk lives. It is split into
two tasks so the read/selection half can land and be used while the write half is still in review.

---

## G. What this set deliberately does NOT do

Named so the gaps are visible rather than assumed:

- **OSR accessibility** (`GetAccessibilityHandler`). Registered by `a0` as a gap with a follow-up; it
  is a scope of its own and pretending otherwise would produce a half-implementation on the one axis
  this repo is strictest about.
- **The IME family.** Same treatment.
- **A GPU picking path.** D8 chose the CPU reference; the accelerator is later work.
- **Alt-mnemonics in the web menubar.** Already deferred and recorded in `menu.ts`; unchanged here.
- **The visual-regression harness** for chrome against the mockups — `docs/shell.md` hands that to
  `e16`, and this set does not take it.

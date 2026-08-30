# 06 — The Scene viewport and the Files panel

Covers tasks `e1`, `e2`, `e3`, `e4`. Read before touching `src/editor/gui/viewport/`,
`src/render/present/`, `compositor.cpp`'s viewport layers, `input.h`'s `RegionMap`, or adding any
built-in panel.

---

## 0. The four-anchor rule, up front

Adding a built-in panel is a **four-anchor edit** guarded by **two different ctests**, and missing an
anchor reds a different gate than the one you were watching:

1. the roster entry (`builtin_roster.cpp`);
2. the headless a11y factory (`gui/a11y/registry.cpp`) + linking its library into `context_gui_a11y`;
3. the `coverage.manifest.jsonl` line — anchors 1-3 are guarded by **`gui-a11y-coverage`**;
4. the `help::panel_topics()` entry (`gui/help/src/help_model.cpp`) — guarded by
   **`gui-help-contextual`** and the `m85-exit-4c` milestone gate.

A `local`-content panel skips anchor 2 **by construction** and pays for it on anchor 3: its coverage
line must declare `ts-a11y`, routing its a11y proof to the `webui-ts-*` browser tier over the real DOM.

Both panels here are C++-modelled (`uitree`), so all four anchors apply normally. Hosting also means
appending to `hostable_panel_ids()` (`builtin_panels.cpp:555`), whose lockstep with the bindings is
asserted by `editor-shell-test_builtin_panels`.

---

## 1. The Scene viewport (`e3`, `e4`) — D7, D8

### What already exists (do not rebuild it)

`01` §6 measures this, and it is the good news of the set:

- `compositor.h:85` — `ViewportLayer { id, content_rect (PHYSICAL px), content (ITextureView*),
  content_size }`; `compositor.cpp:341` `publish_viewports`, `:494` the composite loop, drawn beneath
  the CEF layer through its transparent holes.
- `input.h:64-92` — `ShellRegion { id, rect (PHYSICAL client px), kind }` + `RegionMap` with
  back-to-front hit-testing and `RegionKind::viewport` already in the vocabulary.
- `src/render/present/` — `osr_import`, `osr_composite`, `present_blit` (+ the macOS variant).

**Nothing in production calls `publish_viewports`** — only `test_compositor.cpp:240,331,371` do, and
`input.h:88-91` says the region-map generation counter is the seam *"e11's viewport-content damage path
is expected to read"*. So the composite and the input routing are built and tested; the producer and
the wiring are not.

### `e3` — render and camera

1. **The producer.** Render the scene into an `ITextureView` sized to the panel's rect and call
   `publish_viewports`. Damage: `mark_viewport_content()` exists for exactly this.
2. **The hole.** The panel's DOM element is transparent so the layer beneath shows through. This
   **pins its Dockview renderer to `"always"`** — `rendererFor` in `panelhost.ts` already carries the
   reasoning for content that must not be detached, and an `onlyWhenVisible` panel is removed from the
   DOM, which makes its rect meaningless.
3. **The rect.** The panel's live rect is reported into `RegionMap` on every layout change (Dockview's
   `onDidLayoutChange` is the trigger) **in physical pixels**, since that is what `ShellRegion`
   documents and what the OS reports.
4. **Camera** through the existing `editor.camera-set` / `editor.cameras-get`, persisted where cameras
   already persist. No new contract surface — the verbs exist and carry `transform`/`projection`
   **opaquely**, so the viewport owns their meaning.

⚠ **The rect is the coupling to `03`.** The hole, the layer rect and the region rect are all physical
pixels at the live DPI, while Dockview reports the panel's rect in **CSS pixels (DIP)** — so a
conversion sits between them. `a2` fixes the popup instance of exactly this class of bug; do not repeat
it here, and test at a scale ≠ 1 for the same reason `a2` must.

⚠ Note the compositor holds **no `DpiScale`** today (`on_resize` takes a size only, `compositor.h:190`;
neither `compositor.h` nor `compositor.cpp` names `DpiScale`). `a2` is what decides where the scale
enters — which is why `e3` needs `a2`. Take the scale from wherever `a2` put it rather than opening a
second source for it.

### `e4` — picking (D8)

A **CPU raycast** against `render::RenderSnapshot`, answering `editor.select` with
`subject: "entity"`. The selection then propagates to Hierarchy and Inspector through the fact those
panels already consume — **no new channel**, which is the point of `05`.

**Why CPU, restated so it is not re-litigated:** it is assertable on all three `build` legs with no
GPU, matching this repo's headless-first rule. A GPU id-buffer is pixel-exact but only assertable on
the single Linux render leg, and picking that does not work headless is picking CI cannot defend.
Accepted cost: worse accuracy at geometry silhouettes. A GPU accelerator may be added later,
differentially verified against this CPU reference — that is not in this set.

Tests are ordinary ctest cases over a constructed snapshot: a ray that hits, a ray that misses, the
nearest of two overlapping candidates, and a click outside any drawable clearing the selection. All
headless, all three legs.

### `builtin.viewport`

It is already rostered non-singleton (`builtin_roster.cpp:95`). Under manifest v3 it becomes
`instances.mode: "unlimited"` — multiple scene views is the point — which makes it the **natural first
proof that the instance runtime (`c3`) works on a real panel**. Its existing summary model is not
thrown away; it becomes the honest degraded content when no adapter is available, which is what the
reserved `viewport.adapter_absent` code already exists to report.

---

## 2. The Files panel (`e1`, `e2`) — D10

### The boundary dictates the shape

There is no file-browser panel today. The data lives daemon-side in `src/editor/assetdb/`
(`asset_database.h`: a bounded path/guid/kind index built lazily from sidecars, never reading asset
payloads) and `src/editor/filesync/`.

The **D10 boundary gate** (`context_assert_shell_boundary`, the `editor-boundary` job) FATAL_ERRORs at
configure time on any EditorKernel internal in the Shell's link closure — so **the Shell cannot link
`assetdb`**. The panel therefore needs a **new daemon read verb**, following
`registry.cpp:939` `editor scene-tree` and `:949` `editor inspect` exactly: the kernel-typed model
builder runs daemon-side, the model arrives at the Shell as data, and the gate's forbidden list never
moves.

### `e1` — read and selection

- New verb `editor files` (operational, `read_query`): the project's file tree as a boundary-clean
  panel model — path, guid, kind, and the L-35-adjacent identity the panel keys rows by.
- The panel publishes `subject: "file"` selections through `editor.select` and consumes
  `selection-focus`.
- The four anchors + `hostable_panel_ids()`.

This alone closes the owner's four-panel scenario (Files · Scene · Hierarchy · Inspector all reacting
to one another) without entering the write path.

### `e2` — the write half (D10: a full file manager)

Rename / move / delete over the **existing** engine operations: `AssetDatabase::move_asset` and the
`asset move` / `asset rename` verbs, which already implement the R-FILE-004 dependency-safe order
(destination file, destination meta, then source removal — GUID identity survives every observed
mid-state), idempotence under partial apply, and **refusal on an occupied destination, never
overwrite**.

What the panel adds is the authoring surface, and this is where the risk is:

- the `file_write` capability declared in its manifest, never ambient;
- the **one L-30 write path** — the panel does not write files itself;
- undo journal entries, so a rename is reversible like any other authored mutation;
- **loud refusals** on the existing `editor.ui.write-notice` topic, with the correct kind token. The
  three tokens are **`drop`**, **`refusal`** and **`abandoned`** (`write_notice.h:108-110`), byte-compared
  against their TS mirrors `WRITE_NOTICE_KIND_*` by `tools/check_webui_assets.py --panel-contract`
  (`:413-422`) — do not invent a fourth. A refused rename (nothing written) is **`refusal`**; a value
  that moved under the edit is **`drop`**.
  ⚠ `bad` and `wait` are **not** kind tokens — they are the notification TONE `notifications.ts:142`
  already derives from the kind (`drop`/`abandoned` → `wait`, else → `bad`), so the refusal UX gets its
  tone for free and must not put it on the wire.

**Why `e1` and `e2` are separate tasks:** the read half is a new panel following a well-worn pattern;
the write half enters the L-30 path, the grant system, and undo. Splitting lets the read half land and
be used while the write half is still under review, and keeps the diff that touches destructive file
operations small enough to review properly.

⚠ **Deletion is a genuinely new engine operation, verified.** `asset_database.h` exposes no delete —
its only removal is `"meta-residue-removed"`, an internal step of completing an interrupted move — and
the contract registry carries only `asset move` (`registry.cpp:341`) and `asset rename` (`:350`). So
"delete" in D10 is **not** wiring an existing verb: `e2` must mint the engine operation *and* its
contract verb, with the same refusal discipline the move path establishes (dependency-safe order,
idempotent under partial apply, refuse rather than clobber) plus the question move never had to answer
— what happens to the sidecar meta and to files that reference the deleted asset. That is a real
sub-scope, sized in the task spec rather than discovered mid-implementation.

# Scene viewport — findings from the 2026-09-02 investigation

> **This is a findings dump, not a plan.** No decomposition, no tasks, no ROADMAP, no ordering.
> Planning is deliberately deferred to a separate session (owner's call).
>
> **Origin.** Owner report: the editor opens, the Scene panel shows a text summary, and there is no
> 3D anywhere in the window. Diagnosing that turned up three independent defects behind the one
> symptom, plus a set of unrelated things the investigation walked into. Everything here was found
> against the RUNNING editor (`context_editor --project <sample>` on Windows, GPU present path,
> `CONTEXT_BUILD_GUI_CEF=ON` + `CONTEXT_BUILD_RENDER_WGPU=ON`), not by reading.
>
> **Section A is already fixed** (PR #516, merged `455bd376`) and is recorded so a planner does not
> re-task it. **Section B is open.** Section C is process/tooling hazards that cost real time in this
> session.

## How to read the confidence tags

Every item carries one, because they are not equally solid and a plan built on them should know
which is which:

| Tag | Means |
|---|---|
| **MEASURED** | Observed directly, with a control. The stated cause was demonstrated, usually by making it go away and come back. |
| **OBSERVED** | Seen happen, cause not demonstrated. |
| **OBSERVED-ONCE** | Seen a single time and not reproduced. Treat the frequency as unknown, not as "rare". |
| **UNEXPLAINED** | Reproducible observation, no mechanism found. Named so it is not silently forgotten. |
| **DESIGN QUESTION** | Not a defect. A decision nobody has made yet, surfaced by the work. |

---

## A. Fixed in PR #516 — recorded, do not re-task

### A1. The "transparent hole" was never transparent — MEASURED

The Shell composites each viewport's render target BENEATH the CEF layer and relies on the browser
frame being alpha-0 over the panel's rect (`compositor.h` §4). `editor-UX e3` made two elements
transparent — the panel slot and the Dockview group — and recorded the rest as a deferred boundary.
The rest was load-bearing: `.dv-dockview` (dockview's root) paints
`--dv-group-view-background-color` behind every group, and `html, body` paint the document canvas
behind that. Either one alone keeps the frame opaque, so the composited layer was painted over on
every frame, in every theme, on every boot.

Evidence: a pixel inside the viewport panel body read `#0a0a0a` — exactly `colors.panel`,
byte-identical to every other panel body — with both e3 rules already in force. Making those two
transparent turned the same rectangle `(0,0,0)` with an identical histogram for every other colour
(26 873 pixels changed, nothing else).

Fix: `viewport.ts` marks the dockview root and `<html>` off the same answer that marks the group;
`app.css` turns each mark into transparency, scoped to "a viewport is on screen".

### A2. The viewport's rect was never published to the Shell — MEASURED

A `viewport`-kind region exists only once Dockview has laid the panel out, and that moment is
neither a window resize nor a DPI change — the region publisher's only two triggers — while
`LayoutPersistence` subscribes the Dockview event only after `PanelHost.start()` has already added
every panel. So the dock's arrival was invisible to every publisher.

Evidence: an instrumented `ViewportBinding::publish` printed `regions=4 viewport-kind=0 layers=0`
for the entire life of the window, across five runs. A single synthetic
`window.dispatchEvent(new Event('resize'))` produced `regions=5 viewport-kind=1 layers=1 rendered=1`
immediately, and the grid appeared.

Fix: `PanelHostOptions.onArrangementChanged`, fired from the same place the native-surface marks are
synced, wired in `boot.ts` to the publisher's (now public) debounced `schedule()`.

⚠ **The obvious fix is wrong and this is the part worth remembering.** A single awaited
`publisher.publishNow()` after `startPanels` — mirroring the existing menubar precedent — races
Dockview's layout: it measures an unsettled dock, `viewportRegions` drops the degenerate rect, and
nothing ever asks again. Measured at roughly **1 boot in 6 coming up with a dead viewport**. The dock
needs a trigger, not a shot.

### A3. The panel reported `0x0` forever — MEASURED

`ViewportFeed::refresh_present` ran in exactly two places: when a copy's model is materialised, and
when the adapter verdict flips. A model is materialised by the renderer asking for it, which
necessarily happens before the producer has published any layer for that copy — so the size baked in
was always `0x0`, and nothing ever re-read it. `width`/`height` are fields of the R-HEAD-002 present
report the human reads, so this was a wrong number on screen, not a debug detail.

Fix: `ViewportFeed::sync_sizes()` on the pump's existing per-frame `bind_binding`, touching the panel
kind only when a size actually changed.

### A4. Overlay text became illegible in light themes — MEASURED

Surfaced by A1: once the hole works, the panel's status line, present report and control are drawn
ON TOP of live scene pixels rather than on `colors.panel`. The theme's ink is chosen for the latter,
so every light theme rendered dark text on the D5 pass's dark output.

Fix: the slot re-points the colour tokens its content resolves (`ink`, `muted`, `chip`, `line`,
`panel2`). Tokens rather than rules, because `webui-kit-role-coverage` enforces that no
`.ctx-widget-*` rule may live in `app.css` — the kit is the single styling owner.

### A5. Two live smokes encoded "the map is exactly 4 regions" — MEASURED

`cef_shell_smoke.cpp` and `cef_shell_tearout_smoke.cpp` both asserted a region-map SIZE. Both broke
the moment a viewport region started being published. The tearout one broke **only on the ubuntu
leg**: Windows and macOS raced the dock's publish the other way, so the same assertion was green or
red depending on the runner.

Fix: count by `kind`. The claim in both cases is about the chrome channel, and the dock's contents
are not its business.

### A6. `docs/shell.md` asserted a false premise — MEASURED

The e3 deferred-boundary note named `CefBrowserSettings.background_color` as the item that "makes
the rest moot" — "it is unset, so the browser composites onto an OPAQUE base and no alpha could
reach the OSR buffer whatever the DOM says". Unset is `0`, whose **alpha is 0**, which is CEF's
"use transparent painting" for a windowless browser. Nothing C++-side needed to change; the CSS
alone opened the hole. That one sentence is why e3 stopped where it did — it made the two real
blockers look pointless to fix.

The note also predicted that closing the hole would move every `editor-cef-smoke` `kAppBackground*`
coverage floor. It moved none of them: dock groups still paint `colors.panel` over their own boxes.
13/13 smokes green.

---

## B. Open findings

### B1. There is no scene-data path from the daemon to the Shell (the e11c verb) — MEASURED

**The one that still stands between the owner and a useful viewport.** With A1–A3 fixed, the
viewport renders the D5 grid and nothing else: no project geometry, ever. `viewport_binding.h`
§ SCENE DATA, HONESTLY states it — the e11c verb was never built, so the live editor passes an
EMPTY `RenderSnapshot` and every viewport draws grid plus (absent) proxies.

Visible consequences today:

- the panel honestly reports `empty scene` / `3D scene (0 drawables)` while the Scene Tree beside it
  lists 11 entities;
- **picking can never hit anything.** `ViewportFeed::pick` raycasts against the same empty snapshot,
  so a click in the viewport always resolves to a miss and clears the selection. `editor_main.cpp`
  says so in as many words at the call site.

Scope, honestly: this is **not a fix, it is an unbuilt feature**, and its shape is a contract
decision rather than an implementation detail — what a scene snapshot looks like on the wire, the
daemon-side builder, the Shell-side projection into `render::RenderSnapshot`, the CLI ≡ RPC ≡ MCP
projection, schema + generated typings + the C++ mirror + the drift gate. It belongs to the design
authority, not to a bug list.

### B2. `--devtools` does nothing — MEASURED

`context_editor --devtools` parses, sets `cef_options.devtools_enabled`, and stops there. The gate
that actually opens the port is `if (options.devtools_enabled && options.remote_debugging_port > 0)`
(`cef_shell.cpp:1847`) and **`remote_debugging_port` is never assigned from the CLI** — it keeps its
`0` default (`cef_shell.h:110`). So the documented dev-loop flag is inert: no port, no DevTools, no
diagnostic, no error.

Cost, concretely: inspecting the live DOM in this session required staging a `probe.js` beside the
app bundle and injecting a `<script>` tag into the served `index.html`, because there was no other
way to ask the page a question. A `--devtools-port <n>` (or having `--devtools` pick one) would have
saved most of a diagnostic round.

### B3. The daemon outlives the editor and keeps holding the project lock — MEASURED

The Shell spawns-or-attaches `context daemon` as a child (`editor_main.cpp:1191`). Nothing reaps it:
`daemon_command.cpp` serves "until shutdown" and no idle-exit path was found on the daemon side.
Observed: a `context.exe` started 01:44 was still alive and still holding
`samples/platformer-2d/.editor/lock` more than ten hours later, across many editor launches and
exits.

Consequences: the lock blocks ordinary file operations on the project (it is what stopped a plain
`cp -r` of the sample in this session), and per the project's own history a live `.editor/lock`
inside `samples/` makes the blocking `samples-corpus` gate crash.

Scope, honestly: "spawn-or-attach" means a lingering daemon is REUSABLE by design, so this may be
intended lifetime rather than a leak — but nothing observed ever ends it, and the lock is held for
that whole time. The design question ("when, if ever, does a project daemon exit?") is the finding;
the leak reading is a hypothesis.

### B4. `editor-shell-test_win32_dpi` cannot pass on an ordinary dev box — MEASURED

`CHECK(::GetModuleHandleW(L"user32.dll") == nullptr)` (`test_win32_dpi.cpp:48`) asserts that
`user32.dll` has not been loaded into the test process yet — a precondition the test does not
control and that the harness/CRT can satisfy or not depending on the environment. It fails
identically on a clean tree here (baselined by stashing the whole change), so it is not a
regression; it is a test whose local result carries no information.

A permanently-red local test trains everyone to skip the local gate, which is the same failure mode
the repo's own CI-gating notes warn about.

### B5. `editor-shell-cocoa-window` is flaky on the macOS runner — OBSERVED-ONCE

Two assertions in the caption-drag drill failed on one CI run ("the caption press was handed to
`performWindowDragWithEvent:` by the pump", "neither consumed caption press reached the browser")
and passed on the next run of a superset diff.

Confidently NOT caused by the viewport work: that smoke installs a `ScriptedBrowserHost`, so
editor-core never runs in it and no extra region publish can reach it; it also publishes its own
region map by hand. The drill drives a REAL `NSWindow` and `performWindowDragWithEvent:`, which is
window-server-session sensitive.

Frequency unknown — one failure, one pass. Not investigated further.

### B6. A real window resize produced no region republish — UNEXPLAINED

Before A2's fix, resizing the window with `SetWindowPos` produced **no publish at all** in 5/5 runs:
the Shell's `RegionMap::generation()` never moved, and it is incremented unconditionally on every
publish, so the publish genuinely did not happen. The page had relaid out (the dock measured the new
width), so the browser was resized; the page's `resize` event apparently did not reach
`ChromeRegionPublisher`.

Against that: the `editor-cef-smoke-shell` resize drill asserts a resize-driven republish and passes,
including before this change. So the two setups disagree and the mechanism was not found.

Now MASKED: a window resize relayouts the dock, which fires the new arrangement trigger, so a
republish happens either way (verified: `publishes` 5→6 across a resize). The residue is that if the
`window.resize` path is genuinely broken, **the caption and window-control rects were stale after
every window resize** until this change incidentally covered them — worth settling rather than
leaving to a mask.

### B7. The arrangement report can precede the DOM update; correctness rests on the debounce — MEASURED

`PanelHost` reports "the arrangement changed" from Dockview's event, which can fire BEFORE the new
geometry is applied to the DOM. Nothing is wrong today because the publisher measures when the burst
settles (100 ms), not when it starts — this was found by writing a T1 case that sampled geometry at
callback time and watching it fail against a demonstrably working editor.

The residue: if a layout ever settles AFTER the debounce window and fires no further Dockview event,
the published map stays stale until the next arrangement change. Not observed after the fix (6/6
boots in two themes, plus a resize), but it is the same structural shape that made the one-shot
version fail 1-in-6, and it is currently defended by a timing constant rather than by a signal.

### B8. Withdrawal of a parked viewport copy is unverified through a real dock — OBSERVED

`viewportRegions` drops a copy Dockview has parked outside the dock (an `always`-rendered panel that
stops being the active tab is MOVED, not detached). That rule is proved at unit level against
constructed geometry. It could NOT be reproduced end-to-end in the browser harness: after switching
the co-tabbed sibling in, no publish reported zero regions, with debounces of 10 ms and 60 ms.

Either Dockview did not park the copy in that fixture, or no publish followed the parking. Not
resolved — the integration-level test was deliberately narrowed to "a later arrangement change
publishes again" rather than asserting something the fixture does not demonstrably produce.

If the withdrawal really does not happen through a live dock, the consequence is a render target
allocated and a layer composited every frame for a viewport tab the human cannot see, over whatever
panel took its place.

### B9. The viewport panel paints a duplicate of its own accessible label over the render — DESIGN QUESTION

`ViewportPanel::build_panel` gives the `viewport.surface` node an accessible LABEL carrying the scene
description, and then adds a TEXT CHILD carrying the same string, which is what gets painted. Over a
working viewport that text sits on top of the picture and tells a sighted user what the picture
already shows.

It cannot simply be hidden: with no rendering adapter the composited layer is empty and this text is
the only thing that reports `viewport.adapter_absent` (R-HEAD-002 — absence is REPORTED). So the
question is what a native-surface panel should show over live pixels, and in which states — not
whether to delete it.

### B10. The overlay ink is hardcoded because no theme publishes one — DESIGN QUESTION

A4's fix pins overlay colours as literals, on the argument that what the text is drawn over is not
the theme but the viewport pass's output, which is dark in every theme (the pass takes no theme
input at all). That argument holds today and stops holding the moment a viewport pass renders
something bright — a light scene, a 2D view on a white background, a package-supplied pass.

The alternative is theme-published overlay tokens (`colors.overlayInk` / `overlayMuted`), which is a
theme-schema change across four theme files, the schema, the contract gate and the C++ mirror —
deliberately not made unilaterally.

### B11. The dock's sash gutter now depends on an explicit paint — OBSERVED

With `.dv-dockview` transparent while a viewport is on screen, the gutter between groups no longer
inherits the dock's background; `app.css` paints `.dv-sash` with `colors.canvas` instead. That
restores exactly the pixels the root stopped painting, and a sash never lies under a viewport rect
(it lies BETWEEN group boxes), so it cannot re-close the hole.

The fragility: this is now coupled to a vendored engine's DOM. If a dockview upgrade renames or
restructures the sash elements, the gutter silently shows the compositor's clear colour instead of
the theme's canvas, and nothing tests it.

---

## C. Process and tooling hazards hit during the investigation

### C1. A test plant that breaks the typecheck scores a FALSE GREEN — MEASURED

The `context_editor_webui_test` bundle is gated on the typecheck stamp. A planted defect that fails
`tsgo` therefore leaves the stamp untouched, the bundle unrebuilt, and the ctest **runs the previous
bundle and passes**. This happened in this session: deleting a call site produced
`TS6133 declared but never read`, the plant reported GREEN, and the plant round would have certified
a vacuous test if the build log had not been re-read.

Two separable things worth fixing: (a) a plant must be written in a form that compiles — that is on
the author; (b) the test should not be able to run against a bundle older than its sources, which is
a build-graph question and is not the author's to remember.

### C2. `webui-client-typings-drift` fails confusingly on a partial build — OBSERVED

The gate reads `build/.../editor/client/context-client-schema.json`, which is produced by an ALL
target. After a targeted `cmake --build --target <something>` the artifact does not exist and the
test fails with `cannot read schema: No such file or directory` — which reads exactly like a defect
and is not one. It passes after a full build.

### C3. There is no supported way to inspect the live editor's DOM — MEASURED

Consequence of B2. The working method found in this session, recorded so the next person does not
re-derive it: stage a `probe.js` into the served app directory and add
`<script src="./probe.js" defer>` to `index.html` — the app scheme's CSP is `script-src 'self'`, so
an EXTERNAL file from the same scheme is allowed where an inline script is blocked — then paint the
readout into a fixed high-contrast overlay and screenshot it. Also required: `SetProcessDPIAware()`
before any `GetWindowRect` (or the coordinates are off by the display scale), and re-asserting
`HWND_TOPMOST` + `SetForegroundWindow` in a loop until `GetForegroundWindow()` matches, or the
capture silently contains whatever window was actually on top.

---

## What was NOT looked at

Stated so absence is not read as a clean bill of health:

- camera gestures (orbit / pan / zoom) — a known, documented M9 gap, not touched here;
- anything about the viewport on macOS or Linux at runtime — every live observation in this document
  is from Windows with the GPU present path;
- multi-viewport behaviour (`instances.mode: "unlimited"`) beyond what the existing unit tier
  already covers — one copy was open in every live run;
- the `render-present-*` / `render-ui-*` families and the pass itself — the D5 pass was treated as a
  black box that either draws or does not.

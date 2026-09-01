# The Shell — windows, the owner loop, the compositor, input

How the engine draws an interactive window. Landed by M9 e04 (`src/editor/shell/` → `context_editor`).
This records how the repository implements the design; the normative records live in the owner's
design authority (design 03; `R-HEAD-004`, `R-UI-007`, `R-HUX-011`, `L-41`, review `B-F11`/`C-F2`).

It builds directly on e03's present path (`docs/present-path.md`), which landed the seam a window
drives and explicitly deferred the window manager and the `PET_POPUP` layer to here.

The load-bearing property, stated first because the module is arranged around it:

> **Almost none of the Shell is platform code.** The OS window is one file per platform, the browser
> is one file, and everything between them — the layer stack, damage, the resize protocol, input
> arbitration, DPI, the owner loop — is portable C++ compiled and tested on all three OS legs. That is
> deliberate, and since e12a/e12b it matters MORE, not less: all three v1 platforms now have a real
> window backend, but **no CI job opens a window on Linux beyond the one X11 smoke and none opens one
> on macOS at all**, and the browser is a CI-only dependency path — so logic written inside a backend
> is exercised by almost nothing. That is why each backend's decisions are hoisted into a PURE
> decoder (`translate_win32_message` / `translate_x11_event` / `translate_ns_event`) that every leg
> runs, leaving only OS calls behind the platform `#if`.

## Layout

| Where | What |
|---|---|
| `context_editor_shell` (`src/editor/shell/`) | The Shell proper: window seam + the Win32, X11 and Cocoa backends, DPI, input arbitration, compositor, editor-state persistence, the owner loop. Default-built, CEF-free, GPU-backend-free, fully unit-tested locally and on all three `build` legs. |
| `context_editor` (`app/editor_main.cpp`) | The app. Default-built everywhere; links the browser binding where one can be hosted. |
| `context_editor_cef` (`cef/`) | The windowed-OSR CEF binding — the ONE piece that cannot build locally. Behind `CONTEXT_BUILD_GUI_CEF`. |
| `context_editor_shell_smoke_support` (`smoke/`) | **TEST-TIER ONLY**: the `--real-window` seam the live smokes share (`smoke/smoke_window.h`) — window construction, the CPU present attach, and real-mode input injection, in one place instead of ten. TWO injection arms behind one entry point: X-server `XSendEvent` on Linux (#408) and, since e12c-3 (#442), in-process `-[NSApplication postEvent:atStart:]` on macOS (`smoke/src/smoke_inject_cocoa.mm` / `.cpp`, the always-linkable pair `make_cocoa_window_backend` uses). CEF-free, so it is unit-tested on all three `build` legs. **Nothing shipping links it** — `context_editor_shell` deliberately does not, which is what keeps the shipping window contract unchanged (#408). |

## 1. Windows and the owner loop

`WindowManager` owns N `EditorWindow`s; each binds a native window to one OSR browser, one
`WindowCompositor` and one `InputArbiter`. Window 0 hosts the app menu + welcome screen (D13).

**Since M9 e10a that N is real, and `WindowManager` is also the REGISTRY** (`window_registry.h`).
Windows are peers addressable by a minted `WindowId`; **window 0 is primary**; a window is created
and destroyed at runtime through a factory the app binds (`bind_window_factory`), because only the
app knows how to make a browser. Three properties are load-bearing, and each is asserted by
`editor-shell-test_window_registry`:

- **Every window gets its own of everything that carries identity.** Its own `BridgeRouter` +
  handshake — so a **fresh editor-core instance** boots into it (03 §1: not a shared instance, not
  `retainContext`) and a handler can always tell which window asked — and its own **wire connection**
  to the daemon, so it has its own `origin`. e08a mints `origin` per WIRE CONNECTION, so N windows are
  genuinely N origins; the wire half of that is proven by `editor-session-multiclient-t2`, the
  registry half (a window reports its OWN connection's id, and two windows sharing one connection
  collapse to ONE origin) by the registry test.
- **A retired session outlives `CefShutdown`.** `destroy_window` closes the window — which closes its
  browser — but MOVES the session's bridge, client and captured surfaces into a graveyard emptied
  only by `~WindowManager`; `pump_once` does the same for a window that died on its own, and
  `shutdown()` for every window still open. The app must therefore destroy the manager AFTER
  `shell::cef::shutdown()`, which `editor_main.cpp` does by declaration order. This is **CE #319
  generalised**: that bug was one process-wide teardown freeing a router CEF was still dispatching
  to; N windows add a MID-PROCESS destroy of the same shape, and `CloseBrowser` returning is not
  proof CEF is done with the client.
- **Ids are never reused.** A stale id resolves to nullptr forever rather than silently addressing a
  different window — which is what a vector index would do to a panel e10b moved.
- **Closing window 0 is the APP QUIT, not a window destroy.** `destroy_window` refuses the primary;
  `close_window` — the one home of the `window.close` policy — turns that ask into `request_quit`,
  which asks every live window to close so the owner loop ends normally. § 15 has the whole story
  (and the bug it fixes: the primary's ✕ used to collect the refusal and do nothing).

**Creation failure is LOUD** (03 §7): `create_window` never partially adopts, reports one of
`no-factory` / `factory-failed` / `incomplete-parts` / `limit-reached`, fires
`on_window_create_failed` exactly once with the SOURCE window, and leaves the registry usable. e10a
builds that seam and the report; **e10b owns the behaviour** (degrade to a floating Dockview group
inside the source window). It is deliberately a C++ callback rather than a new bridge method: a new
boot-time bridge surface must be installed in EVERY smoke or the router's deny-unknown-methods
default reddens the ones that were not updated (the e06d regression). Live windows are capped at
`kMaxEditorWindows` (16) — containment, not a UI limit.

**What e10a does NOT do:** no panel moves. Tear-out, rehome, cross-window drag and per-window layout
persistence are e10b–e10d, which target the registry above. Nothing in the app triggers
`create_window` yet; the factory is bound and exercised by the smokes.

Production runs `multi_threaded_message_loop=false` with an **integrated pump on the shell's main
thread** — `CefDoMessageLoopWork` driven from `EditorWindow::pump_once`, scheduled by
`OnScheduleMessagePumpWork`. The spike's "prod = multi-threaded + mutex" caveat is **rejected** by the
design in favour of this single-threaded owner loop: simpler invariants, and the compositor already
decouples engine frame rate from CEF's paint rate, which was the only thing the extra thread bought.

`pump_once` takes the clock as an argument rather than reading one. That is what makes the whole
lifecycle — resize, DPI change, focus, input round-trip, popup, placement persistence, teardown — a
deterministic ctest instead of something only a human at a real window can observe.

**Per-OS backends.** e04 shipped Windows (`RegisterClassExW`/`CreateWindowExW` + WndProc,
per-monitor-v2 DPI); **e12a** added Linux (`XCreateWindow` + the Xlib event pump, EWMH placement and
activation, `Xft.dpi`); **e12b** added macOS (`NSWindow` + a `CAMetalLayer`-backed `NSView`, the
`nextEventMatchingMask:` pump, `backingScaleFactor` DPI). All three v1 platforms now have a real
backend, so `make_window_backend`'s remaining diagnostics report a FAILED creation — no display, no
GUI session, a refused window — rather than a platform nobody implemented. A shell that quietly
opened no window would look identical to one that opened an invisible one.

The macOS backend arrived CEF-FREE with e12b; **e12c-1 (issue #436) added the browser**. macOS cannot
re-exec the main binary as a CEF subprocess the way Windows and Linux do, so a browser there needs an
`.app` bundle, an embedded Chromium Embedded Framework loaded at runtime (`CefScopedLibraryLoader`),
and **FIVE** per-process-type helper bundles — the unsuffixed one plus `(Alerts)`, `(GPU)`, `(Plugin)`
and `(Renderer)`, which is what `CEF_HELPER_APP_SUFFIXES` lists in the pinned distribution. *(This
sentence said THREE until e12c-1 measured it; the count is CEF's, not ours.)* `context_editor` is
therefore a real browser-hosting `.app` on macOS in a `CONTEXT_BUILD_GUI_CEF` configure, assembled by
`context_shell_cef_mac_bundle()` in `src/editor/shell/CMakeLists.txt` and audited at configure time
there; a CEF-OFF macOS build still produces the plain executable it always did. The ONE piece of new
logic that is not packaging is `shell::cef::execute_helper_process()` (`cef/cef_shell.h`): the two
older helper bundles in this repo pass a NULL `CefApp`, which is legal only because their apps have no
renderer duties, while `ShellCefApp` registers `context-editor://` + `context-ext://` in every process
and injects `contextEditorQuery` from `OnContextCreated` — so the Shell's helpers hand CEF the real
app object, and without it no handshake exists and every live smoke fails.
e12c-1 proved that hosting model on **two** smokes and **e12c-2** fanned it out to all **nine**, so the
macOS leg now builds and runs the whole `editor-cef-smoke-shell*` family out of real `.app` bundles —
**eleven** bundles counting `context_editor`, each with its five helpers and its own embedded framework
(309 MB apiece ⇒ **3.1 GB** for `editor/shell/Release`, inside a whole CEF-ON `src/build/dev` of
**8.0 GB**, both MEASURED on an arm64 macOS host at **ten** bundles — against the ~14 GB free that
GitHub's `macos-latest` runner-image spec publishes, an upstream figure this repo does not own, so
re-check it there rather than trusting this line). ⚠ **That list HAS grown since the measurement** —
e09e-3's `-inspector-fanout` is the eleventh bundle, so the same 309 MB apiece puts
`editor/shell/Release` near **3.4 GB** today. The figure is a re-measurement waiting to happen, and it
is the one this document asks to be re-checked before the list grows again.
What e12c-1 deliberately did NOT bring is a WINDOW: macOS's CEF smokes are HEADLESS, exactly as
Windows' are, and they still are. The live windowed macOS proof landed with **e12c-3** (#442) as a
CEF-FREE smoke instead — deliberately, so the windowed claim does not depend on the CEF keychain
class (#437) and rides the plain build legs; taking the ten CEF smokes through real NSWindows is
tracked separately (§ 11). e12c-1 also did not RUN its two smokes on the
macOS leg — both ctests were registered `DISABLED` because `CefShutdown()` never returned there. That is
FIXED: the cause was neither the pump configuration nor macOS 26, but a machine-global keychain item
whose ACL no rebuilt binary matches (issue #437, `docs/cef-keychain-isolation.md`), and every smoke now
runs — all nine PASS **5/5 each** on a macOS 26.5.2 host: 45/45 runs over the nine, via
`tools/measure_cef_smoke_rate.py -k 5`, whose table also carries the two older CEF apps
(`cef-substrate-boot`, `editor-cef-smoke-boot`), so an unfiltered sweep reports 55 — use `--only` to
reproduce just the nine. e12c-2 re-confirmed the wedge is what the isolation prevents by REMOVING it
from one of the newly-fanned-out smokes: `HUNG_AFTER_VERDICT` 3/3, with a pending `SecurityAgent`
prompt. Four Cocoa
shapes have no Win32 or
X11 analogue and are therefore decoded by PURE functions in `window.cpp`, executed by
`editor-shell-test_window` on all three legs — which matters more here than anywhere else in this
document, because until M9 e12c-3 no CI job ran a windowed macOS test at all, and the ten live CEF
smokes still do not (#443): `editor-shell-cocoa-window` is the ONE windowed macOS leg, so the
pure-function split is still what gives these four shapes coverage on ubuntu and windows too:

- **The y axis points UP and coordinates are POINTS.** `ns_view_point_to_physical` flips against the
  VIEW height (not the window's — they differ by the titlebar) and then scales by the backing factor.
  A missing flip mirrors the UI; a missing scale is invisible at 1x and halves every coordinate on a
  Retina display.
- **A modifier key produces no key-down/up at all.** It arrives as `NSEventTypeFlagsChanged`, which
  says WHICH key moved but not which way; the direction is recovered by diffing the mask. Cocoa's
  plain mask carries no side, so a second key of the same pair is genuinely unobservable and is
  reported as nothing rather than as a keydown with no matching keyup.
- **The virtual key codes are POSITIONAL** — `kVK_ANSI_A` is 0x00 and `kVK_ANSI_S` is 0x01, the
  physical ASDF row — and `kVK_Delete` is BACKSPACE. `ns_key_code_to_windows_key_code` is a table for
  that reason, and its traps are the ones the test asserts.
- **Command is META and Option is ALT.** Option is also a TEXT modifier there (Option+e begins an
  accent), so the character event is suppressed under Command/Control and deliberately NOT under
  Option — Windows never needed that rule, because Ctrl+S already produces an uninsertable control
  code in its `WM_CHAR`.

Window-level facts (resize, move, backing-scale change) are not NSEvents at all but delegate
callbacks, so the backend POLLS geometry once per pump and diffs it through
`translate_ns_window_geometry` — the Cocoa counterpart of the X11 ConfigureNotify comparison, and
pure for the same reason. It compares PHYSICAL pixels rather than points, because a window dragged
onto a Retina display keeps its point size and doubles its backbuffer.

`make_window_backend` itself lives in the PORTABLE `window.cpp`, not in either platform file. It used
to sit in `win32_window.cpp`, whose `#else` branch was the honest gap report for both other OSes —
a shape that stops working the moment a second real backend exists, because the Linux build's
selection logic would then sit inside a file that is entirely `#if defined(_WIN32)`. Each platform
file now exposes a `make_<platform>_window_backend` factory that returns `nullptr` off its own
platform (mirroring `make_win32_gdi_blitter`), so the off-platform refusal is a VALUE the ctest
asserts on every leg rather than a symbol that is simply absent.

**The platform blind spot, and what is done about it.** The local dev gate defines `_WIN32`, so a
POSIX branch gets no compile signal at all there — and CI's Windows leg is the only thing that ever
runs a WndProc. EACH native backend is therefore split on the same seam: the EVENT DECODING is a pure
function over plain integers (`translate_win32_message`, `translate_x11_event`) that includes no
`<windows.h>` and no `<X11/Xlib.h>`, names no `HWND` and no `Display*`, and is executed by the ctest
on every OS — that is where the bit-twiddling that actually goes wrong lives. Only the OS calls
remain in `win32_window.cpp` / `x11_window.cpp`, each honestly untested off its own platform, exactly
as e03 left the GDI blit body.

The `WM_*` and X11 constants the decoders use are declared locally and `static_assert`ed against the
real ones inside `win32_window.cpp` / `x11_window.cpp`, so a wrong constant is a COMPILE error on the
platform that has the header rather than an event that silently decodes as something else at runtime
on the one platform that runs it.

Three X11 decoding traps, each pinned by a test in `editor-shell-test_window`:

- **A wheel notch is a ButtonPress/ButtonRelease PAIR on pseudo-buttons 4–7.** The core protocol has
  no scroll axis. Decoding the release too scrolls exactly twice as far as the user asked — which
  reads as an over-sensitive mouse rather than as a defect.
- **X's button order is left / MIDDLE / right.** Button 2 is the middle button, whereas Win32's
  `MK_MBUTTON` is the third bit, so an index-based port swaps middle and right on every
  three-button mouse.
- **`state` is the modifier mask BEFORE the event.** A press must add its own button and a release
  must clear it, or the very first mousedown of a drag arrives with no button held — which is how a
  drag never starts.

Two more X11 rules the decoder encodes, for the same reason: a `LeaveNotify`/`FocusOut` whose `mode`
is not `NotifyNormal` is a GRAB artefact (X synthesizes them around every menu and drag, and
forwarding them blurs the caret mid-gesture), and an `Expose` with `count > 0` is one of a run —
repainting per damaged rectangle is a full composite and present for every sliver of a window drag.

**X11 is PROBED, never required.** `find_package(X11)` without `REQUIRED`, in both
`src/editor/shell/` and `src/render/present/`: every ubuntu CI job configures those directories and
only two of them install X11 development packages, so a hard requirement would fail CONFIGURE — that
is, red the whole rollup — on all the others. Without the headers both files compile to their honest
"configured without the X11 development headers" refusal. The skip is kept non-vacuous at the one
place it matters: the `editor-cef-smoke` Linux leg runs the live windowed smoke with
`--require-x11 --require-display`, under which a compiled-out X11 path is a hard failure.

Three Win32 decoding traps, each pinned by a test:

- **LPARAM's coordinate halves are SIGNED 16-bit.** A captured drag left of the client area reports
  −36, which read unsigned becomes 65500 — a position outside every region that silently re-routes
  the drag.
- **`WM_MOUSEWHEEL`'s coordinates are SCREEN-relative**, unlike every other mouse message. The
  decoder therefore reports the wheel with NO position and the backend fills in the last known CLIENT
  position; arbitrating a wheel by a screen coordinate hits the wrong region whenever the window is
  not at the desktop origin.
- **A minimized window reports a 0×0 client size** on every `WM_SIZE`. Forwarding that as a resize
  asks the swapchain to reconfigure to nothing, every frame, for as long as it stays minimized.

**Placement + layout persistence.** Window placement, the dock **arrangement** (Dockview's `toJSON`),
each panel's D6 state blob, the e14b editor **presence marker**, and — since M9 e09c — the **session
undo journal** are persisted,
debounced and crash-safe, to `.editor/editor-state.json`.
The Shell is that file's **single writer**; the daemon is the single writer of `.editor/session.json`
(03 §1, review C-F3). One writer per file is what removes torn writes without any cross-process
coordination. The write stages into a sibling temp file and renames it over the target, so a crash
leaves either the old complete document or the new one. A malformed document degrades to defaults
rather than refusing to boot — a session file that will not load is a user losing their layout.

editor-core owns the arrangement and the panel blobs, so it PUBLISHES them over the e05c bridge
(`editor.state.publish`, M9 e05d2) and the Shell records them through the store — editor-core never
opens, writes, or locks the file, which is what keeps it an ordinary wire client (D18). On boot it
reads the persisted blob back through `editor.state.get` and rebuilds the arrangement itself; a stale
per-panel blob (a schemaVersion mismatch) degrades that ONE panel to its defaults and never discards
the rest of the layout (the D6 contract, e05b). The three write triggers design 04 §2 requires are all
live: debounced during interaction, a final publish on `pagehide`, and — because the debounced writes
are complete atomic files — the last one is a last-known-good a non-graceful exit leaves intact. The
`editor.state.*` surface is `context_editor_shell/editor_state_bridge.{h,cpp}`, CEF-free and bound to
the store in `editor_main.cpp` ahead of the browser.

**Session undo (M9 e09c).** The `.editor/editor-state.json` document carries one more thing 03 §1
assigns to the Shell: the short-horizon session **undo journal** (`gui/session/undo`, R-HUX-001 /
L-20 / L-21). Six properties are worth stating because each is a place this could have gone wrong:

- **The host is `shell/panels/undo_feed.{h,cpp}`.** Until e09c the journal's `to_json`/`load_json`
  were called by no host at all — it recorded nothing, replayed nothing, and persisted nothing. The
  feed records the Inspector's resolved gesture commits (through a checkpoint sink the composition
  root wires) and hosts the `builtin.session.undo` panel, whose `session.undo` / `session.redo`
  commands are what the palette and the e07c Ctrl+Z / Ctrl+Y keymap ultimately dispatch to.
- **Replay is NOT a privileged path.** Undo/redo re-issue their writes through the SAME
  `WireOverrideWriteGateway` a live gesture commits through — the daemon's `edit` RPC with raw-byte
  CAS, the same L-30 rebase-or-drop engine. A replayed write can hit `cas.mismatch` exactly like a
  live one, and then it is DROPPED loudly rather than restoring stale bytes over a co-writer. A
  "restore the previous bytes" undo is precisely what R-HUX-001 forbids.
- **Only a write that LANDED becomes a checkpoint.** A loudly-dropped commit wrote nothing, so
  journaling it would offer a revert of an edit that never happened — which, replayed, would
  overwrite the co-writer's value.
- **The blob is the journal's own canonical serialization, carried as a string.** The journal's DOM
  is `serializer::JsonValue` and the store's is `contract::Json`, whose numbers are all `double`;
  round-tripping user data through a nested-object conversion would round it. So the ONE journal
  serializer stays `UndoJournal::to_json` and `EditorState::undo` is its transport — no second
  serializer, and no second file. `EditorStateStore::set_undo` is the ONE seam it reaches disk
  through, which is what keeps the C-F3 single-writer split assertable.
- **A landed replay re-arms the Inspector (read-your-replays).** Because the replay writes through
  the gateway directly, `InspectorPanel::commit` never runs and the panel's own commit listener —
  which is what normally re-arms its re-read — never fires. `pump_panel_feeds` consumes the feed's
  `take_replay_landed` flag and arms the fetch instead. Without it the panel would keep both the
  pre-undo value *and* the pre-undo CAS token, so the human's very next edit to that field would be
  dropped as a "concurrent writer" that was really their own Ctrl+Z.

- **A refused replay is not a drop, and is not a loss.** No daemon, or a field that cannot be read at
  all: nothing was written and no concurrent writer was observed, so it reports `undo.read_unavailable`
  rather than a fabricated `cas.mismatch`, and the journal **keeps** the step for the human to retry
  once the project is reachable — the same caller contract the Inspector honours for a refused
  gesture. Both a drop and a refusal report `dispatched:false`, because that bit is what the command
  palette shows the human as success.

## 2. DPI

The DPI is the stored value and the scale factor is DERIVED from it. Storing both is the classic pair
that drifts: Windows hands us an integer DPI, CEF wants a float, and a struct with two independently
settable fields can disagree with itself.

Three consumers need the same number: the swapchain (physical backbuffer pixels), CEF
(`device_scale_factor` plus the view rect it reports in **DIP**), and the input pump (an OS position
is physical; a browser mouse event is DIP). A per-monitor-v2 window changes it while running, so it
is a live value threaded through the frame, not a boot-time constant.

A **non-empty logical extent never collapses to zero**. `ISwapchain::resize` ignores a zero extent, so
a 1×1 window rounding to 0 physical would leave the swapchain on a stale size while the window really
did change. An EMPTY extent still stays empty — otherwise a minimized window would report 1×1 and be
reconfigured every frame.

## 3. Input (03 §6)

Five decisions, in order: region arbitration → capture → DPI → focus class → R-HUX-011 timestamps.

**Region arbitration.** Editor-core publishes the window's region map — viewport content rects plus
native-interaction regions — on every layout change, over the e05c bridge (`editor.regions.publish`,
M9 e05d2: the `editor_state_bridge` parses the array into `ShellRegion`s and forwards them into this
window's `InputArbiter`). A pointer inside one takes the native path; everywhere else is the browser's.
The map is replaced **wholesale, never patched**: a layout change that added a panel and moved two
others is ONE consistent state, and an incremental update is how a stale rect outlives the panel it
belonged to (an empty publish therefore CLEARS a removed viewport's rect). Hit-testing is back-to-front
(the last match wins), mirroring the UI package's own `hit_test`, so stacking is expressed by order
alone rather than by a z field the two sides could disagree about. Today editor-core has no viewport
panels — those are **e11** — so the published set is typically empty; e05d2 delivers the wired,
tested PATH, and e11 fills the region list with no Shell change.

**Capture** reuses the `InputRouter`/`UiInputRouter` shape rather than re-inventing arbitration: a
capture is either MODAL (a miss is swallowed — the dropdown backdrop) or an OVERLAY (a miss falls
through to normal arbitration). Pressing a button implicitly captures until the release, which is what
makes a drag that leaves its region keep going where it started. A press on browser chrome captures to
the browser, because CEF is tracking its own drag and the pointer crossing a viewport must not hand
the stream over mid-gesture. A second button pressed during a drag does not re-target it.

**Focus class** (§6.4): a DOM editable having focus sends keys to the browser unconditionally —
including accelerators, because swallowing a key the user is typing into a text field is the failure
this rule exists to prevent. Otherwise the keymap gets first refusal and unresolved keys still fall
through to the browser. The keymap itself lands with **e07**, so its resolver is a seam that by
default resolves nothing, which makes today's behaviour "everything reaches the browser" — the honest
v1. A CHAR event is never offered to the keymap: its RAWKEYDOWN already was, and offering both would
give one physical keystroke two chances to be claimed.

The native path's VIEWPORT consumer — camera controls, picking, gizmo gestures driving the existing
`viewport_edit_model` verbs over the bridge — arrives with **e11**. Until then the arbitration is real
and every sample is accounted for; the dispatch arm stays empty.

**The macOS caption consult (editor-window-chrome c1, target design 02 §4).** The one live native
consumer of the a1 `caption` region kind: the Cocoa window is created with
`NSWindowStyleMaskFullSizeContentView` + a transparent, title-hidden titlebar, so the a2 web
titlebar strip is the visible top of the window while the native traffic lights float above its
measured inset (`chrome.state.controlsInset`, derived from the real `standardWindowButton:` frames
— `cocoa_chrome.h`). The Cocoa pump consults the window's live region map at **NSEvent time** — the
only moment `performWindowDragWithEvent:` still has the event in hand: a single left press on a
published `caption` rect is handed to the OS drag, a double-click is `zoom:`, and either way the
press is consumed whole (never enqueued, never forwarded to `sendEvent:` — the hand-off IS its
AppKit consumption), so the browser can never hold a stuck hover from a half-press. The decision
itself (`caption_press_action`) is pure and is **the arbiter's own verdict, regions AND capture**:
it reads the window's live `InputArbiter` — the same last-match-wins hit-test `route_pointer` uses,
and the same capture state, through the side-effect-free `InputArbiter::preview_pointer` — so a
left press on the caption while another button's implicit drag or a modal `push_capture` is live
is **yielded** (`CaptionPressAction::yielded`, counted in `CocoaCaptionStats::yields`), flows on
to the arbiter like any other press, and is routed to the capture target or swallowed for the
modal backdrop exactly as previewed; an overlay capture the press is outside of lets the caption
keep it, as arbitration does. (c1 shipped with the consult reading only the region map, which made
such a press a window drag its owner never saw — closed 2026-08-29.) It runs in
`editor-shell-test_cocoa_chrome` on all three legs, `preview_pointer`'s agreement with
`route_pointer` across every capture shape in `editor-shell-test_input`; the windowed proof rides
`editor-shell-cocoa-window`'s c1 step on the macOS `editor-cef-smoke` job. **A double-click on the
caption does what the user's macOS says** (owner decision 2026-08-30, superseding 02 §4's fixed
`zoom:`): the pump reads `AppleActionOnDoubleClick` at the press and maps it through the pure
`caption_double_click_action` — "Maximize" (and an unset preference) → `zoom:`, "Minimize" →
`miniaturize:`, "None" → consumed but nothing happens, "Fill" → `zoom:` (no public NSWindow API
tiles to the screen; the closest one is documented, not silently dropped). A native title bar
honours the same setting because AppKit draws it; ours is a web strip, so the read is ours. The
windowed smoke pins the preference to "Maximize" for its own process through NSUserDefaults'
argument domain (`cocoa_pin_double_click_preference`), so its zoom assertion holds on any Mac
without touching the user's real setting.

**The macOS native menu (editor-window-chrome d3, menu structure 03).** editor-core publishes its
ONE declarative menu model over `menu.publish` (`window_bridge.h` — installed on every window that
installs `window.*`, so the ten-smoke rule holds structurally), the composition root parses it
fail-closed (`menu_model.h`, all-legs tested) and asks the Cocoa backend to build the global
`NSMenu` bar from it (`cocoa_menu.h` — real in `cocoa_window.mm`, an honest false everywhere else,
which is exactly the `accepted:false` a Windows/Linux publish degrades to: the web menubar in the
titlebar strip is the rendering there). An activated item — clicked, or reached through the key
equivalent built from its published accelerator (`Ctrl` maps onto ⌘, the CmdOrCtrl reading) —
returns as an `editor.ui.menu` fact (`menu_facts.h`, the chrome fact's unicast sibling) carrying
the command id, which editor-core executes through the ONE e07b registry: no second dispatch
system. The build + activation round trip is asserted in `editor-shell-cocoa-window`'s d3 step
(programmatic `cocoa_menu_perform`, which refuses disabled items — they are truly inert); clicking
the REAL on-screen menu bar is deferred interactive verification, named in the landing PR.

## 4. The compositor (03 §4)

One frame: acquire → viewport layers → the full-window premultiplied CEF layer → the `PET_POPUP`
layer → present. Editor-core keeps viewport content rects transparent (alpha 0) so native content
shows through — the "transparent hole" contract, which is what makes CEF chrome draw OVER a viewport
for free.

**`PET_POPUP` is a SECOND OSR layer**, required for production: every dropdown and `<select>` depends
on it, and the spike explicitly skipped it. Drawing it needed one addition to the RHI —
`IRenderPassEncoder::set_scissor_rect` (WebGPU `setScissorRect`) — because the composite pass is a
FULLSCREEN triangle, so confining a layer to a rect means scissoring the draw. It is a pure virtual
rather than a defaulted no-op on purpose: a silently ignored scissor does not fail, it draws the popup
over the entire editor.

Scissoring is only half the job. The composite's UV is interpolated across the WHOLE target, so
`compute_layer_uv` **extrapolates** the UV outward such that the interpolation is correct inside the
scissor. Without it a popup drawn in a corner samples whatever part of its texture that corner's UV
happens to land on. The full-window case reduces exactly to e03's `compute_composite_uv`, which a test
asserts so the two cannot drift.

A hidden popup **drops its layer** rather than merely stopping the draw: CEF reuses the popup texture
for the next dropdown at a different size, so a retained layer composites the previous menu's pixels
for the frame between the hide and the next paint.

**Redraw is damage-driven, not a frame loop.** An undamaged frame is SKIPPED; a shell that presented
unconditionally would burn a GPU queue submit per vsync on a completely static editor. Conversely, the
damage **survives a failed frame** — an `Outdated` acquire reconfigures and the next frame must draw
what was never presented. Clearing it there would blank the editor until something else happened to
damage it, which in a shell that has just gone idle can be never.

Acquire statuses follow e03's contract: `Outdated`/`Lost` are NORMAL (a resize raced the frame; the
device went away) and reconfigure without presenting; `Suboptimal` is the one status where the frame
IS presentable and must be presented and THEN reconfigured — treating it like `Outdated` drops a good
frame on every pending resize.

The resize protocol drives **both** halves: reconfigure the swapchain AND tell the browser
(`WasResized`). Doing only the first leaves the browser painting at the old size and the composite
sampling a UV sub-rect that no longer matches the window.

**The CPU present fallback (C-F2)** composites the popup into the view buffer on the CPU before
blitting, so a GPU-less host is not silently popup-less. That blend is a shell-local
`blend_premultiplied_bgra` rather than e03's `composite_reference_cpu`: the latter is the GPU ORACLE
and writes RGBA8, while this is the SHIPPING arithmetic over a BGRA8 destination the blitter hands
straight to the OS — routing through the oracle would mean a swizzle per frame purely to reuse a
function whose destination format is wrong.

## 5. The browser (03 §1, §3)

`IBrowserHost` is the CEF-free seam; `cef/` implements it over a real browser. The frame vocabulary is
e03's `OsrFrame` unchanged — it already carries exactly what `OnPaint` delivers and what the import
driver consumes, and a second shell-local frame struct would be a translation layer whose only job is
to be kept in sync.

- **Windowed-OSR**: rendering is off-screen but the native window is passed as the device-context
  OWNER, which is what gives the browser a correct screen/DPI context.
- **`OnPaint` → the CPU-upload path.** Per the owner ruling of 2026-07-19 the Windows accelerated
  (`OnAcceleratedPaint` → shared-handle import) path is **not implemented**: stock wgpu-native exposes
  no external-texture import and a patched fork was rejected (upstream ask:
  gfx-rs/wgpu-native#621). The seam is still WIRED — the host's `accelerated_osr` option feeds e03's
  `OsrImportOptions`, whose per-platform policy decides — so restoring it is a policy flip plus a
  backend implementation. `OnAcceleratedPaint` is deliberately NOT overridden and
  `shared_texture_enabled` is left off: overriding it to do nothing would advertise a path that does
  not exist.
- **`OnBeforePopup` suppresses stray `window.open`.** Tear-out does NOT ride `window.open` — it is a
  PanelHost/Shell mechanism (04 §2) — so a popup reaching there is an accident, and letting CEF create
  a default popup window would put an un-composited native window on screen.
- **DevTools** is dev-loop only (review B-F11) and off unless asked for twice (`devtools_enabled` AND
  a port): a naive DevTools pass-through from an OSR browser does not display, so the remote-debugging
  port is the working route — and an open debugging port in a shipped editor is a security hole.
- **Never `SendExternalBeginFrame`** (L-41, cef#4033): CEF-internal pacing only.

### The OSR screen mapping — where the view is (a1)

An off-screen browser gets none of its geometry from the OS. `resize()` tells it how big its view is;
`IBrowserHost::set_client_origin()` tells it **where that view sits on screen**, which is what
`CefRenderHandler::GetScreenPoint` and `GetRootScreenRect` answer with. Unimplemented, both default to
`false` and CEF then treats view coordinates AS screen coordinates — the reported offset context menu
(§ 16, owner item #5).

Three things about it are easy to get wrong, so all three live in one place:

- **The two members do NOT share a convention** (pinned `cef_render_handler.h`): `GetScreenPoint`
  wants screen **device pixels** on Windows/Linux and screen **DIP** on macOS — the same split
  `osr_screen_extent` already encodes — while `GetRootScreenRect` is **DIP on every platform**.
  Applying the split to both multiplies the root rect by the scale factor on Windows/Linux, and like
  every bug in this family it is invisible at 100 %.
- **The arithmetic is in `dpi.h`, not in the CEF binding** (`osr_screen_point` /
  `osr_root_screen_rect`), for the reason that file already states about `osr_screen_extent`: the
  per-platform branch is the ONE branch the local gate cannot build *and* no CI job executes. The
  binding passes its platform's convention as a single file-scope constant that all three screen
  callbacks share, so the three cannot disagree.
- **It is the CLIENT origin, never the window rect.** The Win32 window is frameless, so its client is
  inset from its window rect (§ 15) — the window origin puts every menu that inset away from the
  cursor, at every scale. `IWindowBackend::client_origin()` is the one source: each backend answers
  from the live OS (Win32 `ClientToScreen`, X11 the root translation the placement already uses),
  because the persisted `WindowPlacement` cannot answer it — on Win32 it is the RESTORE rect, so it is
  wrong for as long as the window is maximized, and on macOS it is deliberately kept in Cocoa points
  with a bottom-left origin.

The origin is marked stale on the window's **`moved` event** (and by `sync_browser_size`, since a
maximize moves the client as well as resizing it) and re-read on the **placement poll** as a backstop
(a WM-driven move, a maximize, or a backend that reports geometry by polling rather than by event),
never on every pump: the value changes only when the window does, and `set_client_origin`
deliberately does NOT drive `WasResized()` — a window that moved has not resized.

The event path **marks** rather than pushes, and `pump_once` performs the single push after the drain
and before the browser pump. `client_origin()` reads the LIVE OS position at drain time, so every
push within one drain would carry the same value, and the only reader — CEF pulling the mapping
through `GetScreenPoint` — runs after the drain. A Win32 caption drag is a modal `DefWindowProc` loop
that blocks the shell thread, so a whole gesture's `WM_MOVE`s queue up and drain in ONE pump: the
per-event form spent a `ClientToScreen` round-trip per drag frame for a single observable result.
Coalescing costs no freshness — the one read happens *later* than the last read it replaces. The
placement poll still pushes inline, because it runs *after* the browser pump (a deferred flag would
not be consumed until the next iteration) and its interval plus change gate already admit at most one
push per pump.

macOS is an honest zero here and unchanged in behaviour: the browser there is still created with no
NSView owner, so there is no OSR view positioned against that window to map, and the flip convention
Chromium expects for macOS screen DIP is verified by nothing in this repo.

`OnPaint` delivers straight into the compositor with **no copy**: it runs inside
`CefDoMessageLoopWork()`, on the one owner thread, so CEF's buffer is valid for exactly the duration
of the callback and the sink consumes it there.

**The frame sink is bound for the browser's LIFETIME, not for the duration of one `pump()` call**
(M9 e10a — the defect the multiwindow smoke caught on its own first CI run). `CefDoMessageLoopWork()`
is **process-wide**: it drains the pending work of *every* browser in the process, so window 0's pump
dispatches window 1's `OnPaint`. Binding the sink only while a window pumps its own browser therefore
throws away every frame the loop happens to deliver during a *sibling* window's pump — and since the
owner loop pumps window 0 first and each tick's work accumulates during the inter-tick sleep, window 0
won that race essentially always: secondary windows composited **nothing at all**, deterministically,
on both CI legs, while their bridges and handshakes worked perfectly (which is what made the symptom
read as "the second window is blank" rather than "frames are being dropped").

Two rules fall out and both are enforced in `cef/src/cef_shell.cpp`:

- `IBrowserHost::pump(sink)` **retains** `sink`; the caller keeps it alive until `close()`.
  `EditorWindow` satisfies this by construction — the sink is its own `compositor_` member and the
  host is its `browser_` member.
- `close()` **unbinds before it pumps**, not after. It also runs from the host's destructor, by which
  point the owner's compositor is already gone, and it drives `CefDoMessageLoopWork()` — so unbinding
  late would dispatch a paint into freed memory, CE #319's shape one layer down. Once closing, the
  sink can never be re-bound.

`shell::cef::frames_dropped_without_sink()` counts any frame delivered to a live, already-bound
browser with no sink attached — i.e. the defect above, made observable. The multiwindow smoke asserts
it is **zero**, so a regression reports itself instead of degrading to a blank window.

## 6. The D10 shell boundary

The Shell is an ORDINARY CLIENT: it reaches the daemon over the published `context_client` SDK and
never links the EditorKernel's own modules. The `editor-boundary` CI job proves one half (the SDK's
installed headers are self-contained and an out-of-tree consumer builds against them); the
configure-time `context_assert_shell_boundary` gate proves the other, on the real target graph —
because an in-tree target can link whatever it likes and still build.

Attach is **authenticated, with no unauthenticated path**: token enforcement has been on since e02,
and `guard_shell_attach` refuses to even ATTEMPT an attach with no token from either source. Checking
there rather than letting the daemon refuse turns "there is no token on this machine" into its own
message instead of an `attach.denied` that reads like a wrong password. A failed attach is REPORTED,
not fatal — the editor opens read-only, because a shell that would not start without a daemon could
not be used to diagnose why the daemon would not start.

### The panel libraries and the boundary (M9 e05d1)

Design 04 §4 (D17) makes the **C++ panel models the logic + a11y authority** and has the Shell render
them over the bridge, so hosting panels means reaching the headless GUI libraries. That is compatible
with D10, but only because of where the links sit — and the split is load-bearing rather than tidy:

| Target | Links | Why |
|---|---|---|
| `context_editor_shell` | `context_gui_uitree`, `context_gui_contract` | The panel-agnostic host: it renders *a* uitree panel and reads the roster. Both closures are D10-clean. |
| `context_editor_panels` | the above + `context_gui_panel_problems` | The composition root — the only target that links a PANEL library. Reached only by the executables. |

Two of today's panels (`context_gui_panel_scenetree`, `context_gui_panel_inspector`) link
`context_compose`, which the gate FORBIDS. Keeping panel libraries off `context_editor_shell`'s
closure is therefore what lets the panel layer exist at all without weakening the gate — and it is
the seam **e05d3** plugs into when it splits those kernel-typed builders out. The gate's FORBIDDEN
list is unchanged by e05d1 and still passes non-vacuously (all nine forbidden targets PRESENT in the
build, both audited closures CLEAN).

⚠ An earlier version of this section, and of `src/editor/shell/CMakeLists.txt`'s header, said the
Shell links "nothing from ... gui". That was never what the gate checked and is no longer what the
Shell does; the gate is the authority on this boundary.

## 7. Panels — the host, the providers, and the live feed (M9 e05d1)

`PanelHost` (`src/editor/shell/src/panel_host.cpp`) publishes six methods on the privileged bridge —
`panel.list`, `panel.render`, `panel.command`, `panel.gesture`, `panel.state.get/set` — over the e05b
roster. It is **panel-agnostic by construction**: no panel id appears in it, and the ability to render
one comes from a `PanelProvider` (a bundle of `std::function`s) bound at the composition root. Adding
a panel is a roster entry plus one provider binding, with no change to the host or to the TS hydration
runtime; `editor-shell-test_panel_host` asserts that over synthetic panels the host has never heard of.

- **The roster is authoritative; the provider table is capability.** Every rostered panel is LISTED;
  one with no provider reports `hosted: false`. That is how the editor shows its whole panel set while
  the Viewport, Tilemap Painter and Viewport Edit panels still have no provider, and why
  `panel.unknown` and `panel.not_hosted` are different refusals.
- **Hostable today**: five — `placeholder` (from `context_gui_uitree`), `builtin.problems`,
  `builtin.scene-tree` + `builtin.inspector` (e05d3) and
  `builtin.session.undo` (e09c), from four different libraries. (`builtin.playbar`, hostable
  e08b..e09, was retired by editor-window-chrome e1: the d1 titlebar strip is the Play Bar's only
  home, over the surviving `SessionFeed`/`PlaybarModel` transport.) More than one deliberately — a single
  panel would leave panel-agnosticism resting on a claim. `hostable_panel_ids()` is the one
  enumeration, and `editor-shell-test_builtin_panels` asserts every id in it is hosted and that
  nothing else is, so this list cannot drift into a claim the bindings do not honour.
- **The live read path**: the Shell subscribes to the daemon's `diagnostics` + `derivation` topics
  through the SDK's `SubscriptionConsumer` and projects what arrives onto the `ProblemsPanel` model
  (`src/editor/shell/panels/src/problems_feed.cpp`). The projection is pure and unit-tested on all
  three build legs; only the wiring needs a daemon.
- **Single-threaded, deliberately.** The subscription is pumped from the owner loop with
  `poll_timeout_ms = 0` and a short reconnect ladder, NOT from a background thread. The feed mutates
  the panel models and the bridge handlers that render them run on this same thread, so a background
  pump would be a data race on every model, requiring a lock around the whole host. The cost of the
  choice is a bounded (~1 s) stall on a daemon restart; revisit it when panels get heavier.
- **D6 state** is round-tripped through `contract::persist_panel_state` / `restore_panel_state`, with
  the schema version read from the MANIFEST, not the provider. A version mismatch is not an error: the
  panel keeps its defaults and the caller gets a diagnostic — the degrade e05d2's layout restore needs
  so one stale blob cannot discard a whole layout.

## 8. Test map

| Test | Covers |
|---|---|
| `editor-shell-test_dpi` | Scale derivation, the OS-nonsense clamp, round-to-nearest, the never-collapse rule (and that empty stays empty), signed point conversion across zero; **a1**: the OSR screen mapping on both platform conventions — view→screen at a NON-INTEGRAL scale from a NON-ZERO (and negative) client origin, the three wrong answers it must not give, the root rect proved DIP by describing ONE window in each convention and requiring the SAME rect (the no-split rule in a form a scaled implementation cannot pass), the two members' agreement at the view origin, and the CLIENT-origin case composed from the real `win32_frameless_client_insets` at 96 and 144 dpi |
| `editor-shell-test_input` | Back-to-front hit-testing, edges and NEGATIVE coordinates, wholesale publish, viewport-vs-browser arbitration, DIP dispatch positions, the implicit drag capture (incl. a second button mid-drag), modal swallow vs overlay fall-through, focus-class key routing, the R-HUX-011 stamp |
| `editor-shell-test_editor_state` | Round-trip (incl. a negative x and a maximized window's restore rect), the debounce, no-op on identical, `flush_now`, the atomic replace leaving no temp, the degrade on a malformed/negative-extent document, a failed write staying dirty to retry |
| `editor-shell-test_window` | The pure Win32 decoder — signed LPARAM halves, the minimize carve-out, button mapping, MK_*/modifier split, the signed wheel delta and its deliberate absence of a position, key/char/sys-key, `WM_DPICHANGED`'s low word — plus the headless backend and the never-silent platform selection; since editor-window-chrome b1 the frameless-frame decisions (insets, the no-8px-overhang max geometry at 96 and 150 %, `hit_test_frame`'s bands / corners / precedence / DPI scaling / maximized branch, the NC-mouse forwarding + consume + synthetic leave, the headless chrome recorder, and — a1 — the headless `client_origin()` proved COMPUTED from the placement plus a modelled frame inset rather than stored beside it), and since g1 the `hit_test_frame` SWEEP CORPUS — every point of the window rect at five DPIs (three exact scales plus 100 and 150, where the round-to-nearest metric rule is what decides), both frame states, three region maps, judged against a spec oracle and six oracle-free invariants (§ 15) |
| `editor-shell-test_cocoa_chrome` | editor-window-chrome c1: the caption-press consult (drag / zoom / none over the real `RegionMap`, last-match-wins layering, click-count rules), the measured traffic-light inset arithmetic (Retina, RTL, degenerate frames), and the off-platform / wrong-backend refusals of the `cocoa_chrome.h` surface |
| `editor-shell-test_chrome_facts` / `editor-shell-test_menu_facts` | a1 / d3: the `editor.ui.chrome` maximized fact and the `editor.ui.menu` activation fact — envelope shape, UNICAST delivery to the affected window, the honest unbound-store path (and, for the menu, the empty-id refusal + the off-platform `cocoa_menu` refusals) |
| `editor-shell-test_menu_model` | d3: the published menu model's total, fail-closed parse (drop-per-item tolerance, depth/size caps, the outer shape) and the accelerator tokenizer, on every leg |
| `editor-shell-x11-window` | e12a: a REAL X11 window through the real `make_window_backend`, the real X11-SHM blitter, live panels, a server-driven repaint (`XClearArea` → `Expose`) and resize (`ConfigureNotify`), the placement readback and the session flush; e12a-x11-legs (#408): a pointer pair + a key INJECTED THROUGH THE X SERVER and decoded by the real `translate_x11_event`, down to one press / one release and the round-tripped `VK_TAB`; **editor-window-chrome g1**: a caption gesture in the a2 shape — hover, press, the drag that leaves the strip, release — suppressed end to end through the X server with the implicit capture released on the release, the dock forwarded again afterwards, and a control press forwarded INSIDE its physical rect (§ 15, Linux). SKIPs (77) with no display; the Linux `editor-cef-smoke` job runs it DIRECTLY with `--require-x11 --require-display` (§ 10) |
| `editor-shell-cocoa-window` | e12c-3 (#442): a REAL `NSWindow`, the real `CALayer.contents` blitter, live panels, a granted resize, a marked pointer pair + key round-tripped IN-PROCESS through `-[NSApplication postEvent:atStart:]` (the three Cocoa fidelity limits § 11); **editor-window-chrome c1 / d3 / f1**: the hybrid style mask re-read LIVE, the measured positive inset, a caption press consumed whole by `performWindowDragWithEvent:` and a double-click by `zoom:` (which really zooms), the release still arbitrated, no leaked capture, a non-caption press forwarded; the `NSMenu` bar built from `menu.publish` and activated programmatically (`cocoa_menu_perform`, disabled items refused); a factory-created second window carrying the same mask + inset. SKIPs with no GUI session; the macOS `editor-cef-smoke` job runs it DIRECTLY with `--require-cocoa --require-display` |
| `editor-shell-test_compositor` | The extrapolated layer UV (incl. the full-window identity vs e03), the premultiplied blend + clipping, damage-driven skip, LAYER ORDER and the popup's scissor rect, a hidden popup dropping its layer, the resize protocol, Outdated/Lost keeping the damage, Suboptimal presenting first, a refused surface, both present paths, a malformed producer frame |
| `editor-shell-test_shell` | The attach guard, the owner loop end to end (DIP browser sizing, input round-trip, viewport vs browser, focus dropping a live drag, idle skip, popup), placement persistence + restore, window drop, shutdown flush; **a1**: the client origin seeded before the first paint and updated on the `moved` event under the REAL 250 ms poll interval (so the push is attributable to the event, not to the poll), the modelled frameless inset proving it is the CLIENT origin and not the window rect, the placement-poll BACKSTOP for a move with no event, the COALESCING (four moves drained in one pump push ONCE, carrying the last position — the per-event form scored five), and the negative half — an idle pump pushes nothing |
| `editor-shell-smoke-session0` | **The blocking CI requirement**: the whole shell loop over software-OSR frames with the composited present asserted PER-PIXEL — see § 9 |
| `editor-shell-boundary` | The D10 link-closure audit actually ran and covered a real forbidden target |
| `editor-shell-test_panel_host` | The panel-agnostic surface over SYNTHETIC panels: roster projection (hosted vs listed-but-unhosted), render payload, command dispatch + the stale-command refusal, the four gesture verbs and the refusal of a fifth, the D6 round-trip and all three degrade paths, every `panel.*` binding, and hostile params on every method |
| `editor-shell-test_editor_state_bridge` | e05d2: the layout/panels publish→store→get round-trip (incl. a restart), all three persistence triggers (debounce, `flush_now`, crash-restore), region parsing (kind tokens, negative-pixel clamp, malformed-element skip) reaching a live `InputArbiter` and routing a pointer, the empty-publish clear, the `not_ready`/`bad_params` degrade paths, and the full `editor.*` JSON-RPC binding over a real router |
| `editor-shell-test_problems_feed` | The LIVE `diagnostics` projection without a daemon: severity/stability tokens, all three snapshot shapes, every publisher shape the topic carries, hostile/degenerate payloads, R-BRIDGE-008 promotion + settle, and the node-id -> diagnostic-identity mapping |
| `editor-shell-test_builtin_panels` | The composition root: that all five hostable panels bind and nothing else does (the e1-retired playbar stays unhosted and off the roster), the Scene tree's selection reaching the Inspector's fetch, a daemon event reaching a rendered panel end to end, and (e09c) that the undo replay left its mark on the bag's OWN wire-gateway instance, that the pump turns a landed replay into an Inspector re-read, and the two undo persistence seams against a real editor-state file |
| `editor-shell-test_undo_feed` | e09c: the session undo host — a checkpoint recorded and replayed through the BOUND gateway, a co-writer's field DROPPED loudly on undo AND redo (R-HUX-001), a refused replay KEEPING the step while dirtying and touching nothing, the read-your-replays flag raised once then consumed, the provider's honest `dispatched` verdict, and the DoD line: the journal round-trips through a REAL `EditorStateStore` writing a REAL `.editor/editor-state.json`, survives a full teardown/rebuild, and Ctrl+Z still reverts the pre-restart edit |
| `editor-shell-test_user_config` | e06d: the per-user config store - the total reader (absent / malformed / non-object / oversized), the merge-preserving read-modify-write (a member from a FUTURE build survives), the recents-and-theme co-existence regression, the CLOSED settable vocabulary (`config.unknown_key` / `config.bad_value` / `config.write_failed`), unique staging names, the generation watch (identical rewrite and cosmetic reformat are NOT changes), and the full `config.*` binding over a real router |
| `editor-shell-config-writers` | e06d: the C-F14 SINGLE-WRITER source gate - exactly one TU writes `~/.context/config.json`, editor-core carries no client-side persistence API, and one module names `config.set` (`tools/check_config_writers.py`) |
| `editor-shell-session-ownership` | e09d: the C-F3 SESSION-FILE OWNERSHIP source gate - one C++ writer per session file (which must still write THAT document, not merely contain write machinery), each owner in its OWN process's subtree, and the in-process override-write gateway named by nothing but its own definition and tests and linked by no Shell target (`tools/check_session_ownership.py`) - see § 14 |
| `editor-shell-test_smoke_window` | e12a-x11-legs + e12c-3: the smoke-tier window seam, asserted with no display — the `--real-window` flag parse, headless construction/present/injection, `browser_geometry`'s DIP conversion at a non-identity DPI, BOTH key-table inverses (X11 keysym and macOS virtual key) SWEPT back through their shipping decoder map, a THIRD sweep pinning the two tables to EACH OTHER (a VK one arm accepts and the other refuses is a smoke that silently cannot inject on one OS), and the load-bearing negatives: real mode REFUSING a headless backend (pointer, key AND resize) and a window with no presentable native surface, rather than degrading |
| `editor-cef-smoke-shell` | The LIVE CEF half: a real browser through the real integrated pump, its `OnPaint` frames composited + presented, input round-tripped, a live resize repainted. Windowless on Windows and (since e12c-1) on macOS, where it boots from a real `.app` with five helper bundles; since e12a-x11-legs the Linux leg runs it through a REAL X11 window and injects its gestures through the X server (`editor-cef-smoke` job; BUILT and RUN on all three OSes — macOS included since issue #437 was fixed at its cause, `docs/cef-keychain-isolation.md`) |
| `editor-shell-cef-keychain` | #437: the source gate that every CEF smoke isolates Chromium's OSCrypt key from the MACHINE keychain — each source constructing a `CefShellOptions` under `src/editor/shell/cef/src/` sets `use_mock_keychain`, each source defining `OnBeforeCommandLineProcessing` under `src/editor/` names the switch, and the option is still declared/latched/appended (`tools/check_cef_keychain_isolation.py`). Needs no CEF build, so it runs on the CEF-OFF legs too — see `docs/cef-keychain-isolation.md` |
| `editor-shell-test_window_registry` | e10a: the registry — window 0 primary, ids minted in order and NEVER reused, all four create-failure classes reported once with the source window (and the registry still usable after four in a row), the live-window cap, per-window `origin` reporting, and the CE #319 lifetime rule in both directions: a destroyed window's browser dies NOW while its session is retired until the manager does, across 25 create/destroy cycles and across `shutdown()` with windows still open. Since the close-button fix it also carries the `window.close` POLICY: the primary's close is the app quit and reaches EVERY window, a secondary's destroys only it, `destroy_window` still refuses the primary, and a stale primary id (window 0 already dead, a secondary still up) is `unknown-window`, never a quit |
| `editor-cef-smoke-shell-multiwindow` | e10a, the LIVE half a fake cannot reach: a SECOND real CEF browser booting its OWN editor-core instance (two DIFFERENT round-tripped handshake nonces), a REAL renderer `window.open` refused by `OnBeforePopup` with NO browser created, and a MID-PROCESS destroy followed by another create (`editor-cef-smoke` job, all three OSes since e12c-2) |

All `editor-shell-*` tests are a plain (non-gate) family: the `build` job's general ctest step runs
them on all three OS legs and `--preset dev` builds them, so **no `ci.yml` `--target` bookkeeping**.
That covers `context_editor_panels`' tests too — they register in the same family, and the library
itself is built transitively by the jobs that build `context_editor` / the CEF smoke, so e05d1 needed
**no `ci.yml` change at all**.
`editor-cef-smoke-shell` is the exception and IS on the `editor-cef-smoke` job's hand-maintained
`--target` list — the "Not Run = RED" tripwire. So are **all nine** of its siblings:
`editor-cef-smoke-shell-restore` (e05d4), `-palette` (e07d), `-settings` (e06d),
`-multiwindow` (e10a), `-tearout` (e10b), `-drag` (e10c), `-uimirror` (e10d), `-iframe` (e13a) and
`-inspector-fanout` (e09e-3) — **ten** registrations in total, which is the number every statement
about this family must use (the same miscount §11 corrects, and the one that once left the parent's
configure-time staging roster two targets short). The roster itself therefore names **eleven**
targets: `context_editor` plus those ten.
**e12c-1** had made that list **PER-OS** (Windows/Linux all nine, macOS the two whose `.app` hosting
model it ported) because the other seven were declared only under `if(OS_WINDOWS OR OS_LINUX)` and a
shared list would have failed the macOS build outright. **e12c-2** fanned the recipe out to all nine, so
it is **ONE SHARED list** again — and the per-smoke CMake that had been copied nine times collapsed into
`context_configure_shell_cef_smoke()` in the same pass. That function requires a macOS `BUNDLE_ID` **on
every platform**, which is the structural reason a tenth smoke cannot repeat e12c-1's gap by landing
Windows-only. No new ctest NAME was involved on macOS in either task, so the "Not Run = RED" tripwire
bites only on the `--target` side.
Two things deliberately stay spelled out per target at the call sites rather than moving inside that
function: `SET_EXECUTABLE_TARGET_PROPERTIES(<exe>)` and `add_dependencies(<exe> context_editor_cef_stage)`.
`tools/check_cef_staging.py` reads both from the SOURCES with the executable named LITERALLY; it resolves
a `${var}` through file-local `set()` but a function PARAMETER is unresolvable by construction, and the
lint correctly refuses to skip a name it cannot resolve — it reports the stage dependency as UNVERIFIED
instead. Hiding either behind the function would have traded ~18 lines for ten unaudited executables.

⚠ **THE ROSTER IS NO LONGER TRUSTED TO BE COMPLETE — it is DERIVED and cross-checked (M9 x11).**
`_ctx_cef_shell_executables` is what both configure-time audits ITERATE, so a target missing from it
was never flagged, only SKIPPED — and that had already happened twice (`-uimirror`, `-iframe`). Two
tiers now close it, because neither is sufficient alone:

- **Graph tier** (`src/editor/shell/CMakeLists.txt`, CEF-ON configures on all three legs): the set of
  CEF-hosting executables is derived from the real build graph — an executable in this subtree that
  reaches `context_editor_cef` / `libcef_lib` / `libcef_dll_wrapper` on its link closure or as a direct
  manual dependency — and must EQUAL the literal roster. Derived-but-unlisted is a hard, NAMED
  `FATAL_ERROR`; listed-but-not-derived is the FLOOR, so the literal list cannot silently shrink
  either. The manual-dependency half is required because macOS never LINKS libcef (the framework is
  dlopen'd), and the ONE exclusion is CEF's per-process-type helper bundles — 55 of them on macOS —
  identified by the same owner + `OUTPUT_NAME` `"<owner> Helper<suffix>"` equality the macOS bundle
  audit already asserts, deliberately not by a name pattern, so a smoke declared as a child of another
  smoke would fail loudly rather than be excluded. An EMPTY derived set is itself an error.
- **Source tier** (`tools/check_cef_staging.py` check 5, every default CEF-FREE `build` leg): the
  literal roster must equal the literal `add_dependencies(<exe> context_editor_cef_stage)` consumer
  set. It can only read literal names — the `${var}` limitation above — which is exactly why the graph
  tier exists; and the claim that this repo declares exactly one non-empty roster is asserted in
  `tools/tests/test_cef_staging.py`, so a RENAMED roster cannot become how check 5 passes.
The two macOS registrations e12c-1 landed were `DISABLED TRUE` until issue **#437** was fixed at its
cause; all ten now RUN there. ⚠ A `DISABLED` ctest reports `Not Run (Disabled)` and leaves ctest's
exit code at
**0**, so for as long as the property was there the CE #319 `cef_shutdown_returned` assertion could not
fail on this leg — which is what makes `DISABLED` the strongest possible form of a vacuous gate, and why
re-enabling both had to be proven by PLANTING against the assertion rather than by observing green.
`docs/cef-keychain-isolation.md` has the mechanism, the k=5 rates on both sides of the switch, and the
two product exposures that remain open for the owner.
`editor-shell-test_window_registry` is NOT: it is a plain
`editor-shell-*` family member like every other unit suite.

## 12. The per-user config — `~/.context/config.json` (M9 e06d, C-F14)

One small JSON document holding the user's editor preferences: the chosen theme (06 §4), the recent
projects the welcome screen lists (e14c), and the window defaults. Per-user, not per-project.

**The Shell is its SINGLE WRITER.** editor-core is a pure wire-client with no filesystem: it READS the
document over `config.get` and REQUESTS changes over `config.set`, and `UserConfigStore` (user_config.h)
validates against a CLOSED settable vocabulary — today exactly one key, `theme` — before persisting.
An unknown key or a malformed value is refused with a stable code and writes nothing.

Three properties are worth knowing because each fixes a real defect:

- **Every write is a read-modify-write over the parsed document.** `record_recent_project` used to
  REPLACE the file with `{version, recents}`, so opening a project discarded whatever else was in it.
  Two features share this document; a member an older build does not understand still survives a write
  from it.
- **Staging names are unique** (`<config>.tmp.<pid>.<counter>`). The e14c writer staged through one
  fixed `.tmp`, so two launches racing could publish each other's partial bytes. The rename is the
  atomic publish; last-writer-wins is the correct semantic for a single-user preference file.
- **The single-writer rule is MECHANISED, not documented.** `tools/check_config_writers.py` (ctest
  `editor-shell-config-writers`) fails a tree where a second TU writes the file, where editor-core
  gains any client-side store (`localStorage` and friends), or where a second module names
  `config.set`. It FAILED on the pre-e06d tree — that is what makes it a gate rather than a comment.

The store is polled from the owner loop like the keybindings and themes watches, so a theme picked in
one editor window becomes visible to another without a restart.

## 13. The two notification banners (M9 e14d, design 07 §4 / 08)

Two strips the editor can raise: the **notify-only update banner** and the **daemon-lost read-only
banner**. Both are Shell-owned STATE served over the privileged bridge (`update.state`,
`daemon.linkState`) and rendered by editor-core (`core/src/banners.ts`) on the welcome screen, in the
editor's fixed top strip, and — for updates — in Settings § Updates.

**The update check is notify-only (O3, owner-confirmed 2026-07-22).** One HTTPS GET against the
latest published release, a LOCAL version comparison, and a click-through to the downloads page.
There is no in-app updater and no download; that is post-M9.

**The privacy commitment is the feature, and it is proven rather than stated** (08 threat row,
*Update-check privacy*: "notify-only version GET, no identifiers"). Four properties make it checkable:

- **The request is a compile-time CONSTANT.** `build_release_check_request()` takes no arguments and
  reads no host state — not the running version, not the OS, not a locale, not any machine / install /
  user id, and it carries no query string and no body. Every Context Editor on every machine emits
  byte-identical bytes, which is the strongest available claim: the server cannot tell two users
  apart because the two requests are the same. `editor-shell-test_banners` golden-compares the whole
  request (method, URL, every header, body) via `canonical_request_text()`, so ANY addition fails —
  and each layer of that suite was falsified by planting a violation, not by inspection.
- **The comparison is local.** The endpoint is asked what the latest release is; it is never told
  what we run.
- **The OS transport adds nothing.** `native_net.cpp` drives WinHTTP with a NULL user agent, NULL
  accept types, cookies and authentication disabled, and the autologon policy set to HIGH — so no
  default `User-Agent`, no `Accept: */*`, and never the signed-in user's credentials. Nothing in C++
  can observe what WinHTTP put on the wire, so those four are pinned by the source gate
  `tools/check_release_request.py` (ctest `editor-shell-release-request`), which also refuses a
  second request builder and any header literal in the transport.
- **editor-core may not make the request.** A renderer `fetch()` would look cleaner and would be a
  privacy REGRESSION: Chromium attaches its own `User-Agent`, `Accept-Language` and client hints
  beneath the JavaScript. The same gate refuses any network primitive in `src/editor/webui/**` (a
  same-origin relative `fetch` — the T1 harness reporting to its own driver — is the one carve-out,
  and it is carved out by the ARGUMENT's shape, not by the file being a test).

**No new dependency.** The transport is the platform's own HTTPS client (WinHTTP, Windows SDK):
nothing is added to `src/vcpkg.json` or `tools/license-allowlist.json`, so design 08 §3's standing
owner-approval gate for a new dependency is not reached. macOS and Linux report an honest "not wired
on this platform yet", and the banner then says "not checked" rather than implying the build is
current. Note the shape of what is left: e12a/e12b closed the WINDOW-backend gap on both platforms,
so this and `native_pick_folder()` are now the remaining per-OS gaps in their own right — each owed a
platform HTTPS client and folder chooser, not a shell.

**Two behaviours that look like omissions and are not.** (1) A FAILED check renders no banner; it is
reported in Settings § Updates instead, because a strip on every offline launch is how the notice
that matters gets ignored. (2) Dismissal is SESSION-scoped and deliberately not persisted — the
config document has a closed settable vocabulary and one writer (§ 12), and a durable dismissal would
have to answer "until which version?", a product question notify-only does not pose.

**The daemon-lost banner reads e14a, it does not drive it.** `make_daemon_link_probe(lifecycle)`
projects `DaemonLifecycle::read_only()` / `reconnect_attempts()` / `ownership()`; the backoff ladder
and the read-only policy stay entirely in `daemon_lifecycle.h`. The probe is bound ONLY in project
mode, which is a correctness point rather than an optimisation: `read_only()` is true whenever the
Shell is not live read-write, and on the WELCOME screen that is always — no project is open, so there
is no daemon to have lost. A daemon can only be LOST once one was wanted.

**Every live CEF smoke installs `BannerBridge`.** editor-core asks for both banner states during
boot and the router denies unknown methods by default, so an uninstalled surface would trip the
smokes' strict `bridge.refused() == 0` invariant even though the renderer degrades gracefully — the
exact regression e06d shipped with its config surface. The smokes bind neither collaborator, so the
surface honestly reports "no update channel" and a live link: no network call, and no banner painted
over the surface whose per-pixel coverage those legs assert.

## 14. The session-file ownership split — `.editor/editor-state.json` (M9 e09d, C-F3)

Two session files sit side by side in a project's `.editor/` directory with two DIFFERENT owners
(design 03 §1, an explicit refinement of L-20's file mapping):

| file | owner | contents |
|---|---|---|
| `.editor/session.json` | the **daemon** | selection, cameras, play state (`editor-session-state.md`) |
| `.editor/editor-state.json` | the **Shell** | dock layout, window placement, panel state blobs, the e09c session undo journal, the e14b presence marker |

**One writer per file is the whole point.** Two processes writing one document is a torn write nobody
notices until a user's window layout — or, since e09c, their **undo history** — comes back mangled.
Splitting by file means neither owner ever has to coordinate with the other.

### The rule is MECHANISED, not documented

This is precisely the class of invariant that gets asserted in a header comment and never checked: a
runtime test passes just as happily against a tree that has grown a second writer, and reproducing the
real failure needs two processes racing. So `tools/check_session_ownership.py` (ctest
`editor-shell-session-ownership`, the same tier as § 12's config gate) scans the sources for what a
second writer would have to make false:

1. exactly **one C++ TU writes each file** (`editor_session_state.cpp` / `editor_state.cpp`);
2. each owner lives in its **own process's subtree** — one writer per file is not enough, because
   moving the Shell's writer into the daemon satisfies that and kills the split;
3. the in-process compose/filesync override-write gateway (`ProjectOverrideWriteGateway`) is **named**
   by no product source and **linked** by no Shell target — the live editor commits over the daemon's
   `edit` RPC (e09b-2's `WireOverrideWriteGateway`), and the in-process one is the injected T1 mock's
   real-disk sibling. The `context_assert_shell_boundary` FORBIDDEN set already forbids its
   `context_compose`/`context_filesync` dependencies **transitively**; this names the subject;
4. plus an **anti-vacuity half** — each declared sole writer must still name its document and still
   contain a write, because every prohibition above is trivially true of a tree that deleted the thing
   it protects.

A file that legitimately READS a document it does not own while writing its own sibling artifact —
`arbitration.cpp`, which reads the presence marker and writes `.editor/focus-request` — is listed as a
DOCUMENTED READER with its reason, and re-checked on every run: the entry is reported stale if it stops
naming the document, and reported as a violation if it starts naming the owner's store.

**It was verified by PLANTING, not by reading the pattern** (`conventions.md` § "Authoring a
SOURCE-SCAN gate"), across **two rounds**, and both paid for themselves.

The first round found ordinary filename spellings the pattern missed (the whole relative path as one
literal, a raw string literal, a Windows separator), an `#include <fstream>` counted as a write, and
an anti-vacuity half satisfied by a name the writer itself defines.

The second round, run against the finished first revision, found that fixing that last one had **moved
the hole rather than closed it** — the sole writer *defines* `atomic_write_text`, whose body carries
the primitive write spellings, so "this TU still writes" was satisfied forever no matter what happened
to the real save. It also found rules 4 and 4b had **no anti-vacuity half at all** (renaming the
gateway type, or its library, retired each rule silently); a **false positive** that would have redded
all three `build` legs, because CMake's `#` comments were never stripped and a commented-out link read
as a real one; the same unstripped comments **hiding** a real link behind a `)`; a Shell sub-library
missing from the hand-maintained target list; a documented READER one line from becoming a second
writer under a whole-file exemption; the two **platform** write APIs this repo actually uses
(POSIX `open`/Win32 `CreateFileW`) missing from the spelling list entirely; and a scan corpus that
skipped `.inl` and was case-sensitive about suffixes.

⚠ **The enumeration lives in ONE place**: the `# FOUND BY PLANTING` markers in
`tools/tests/test_check_session_ownership.py`, each a regression case. Earlier revisions of this
paragraph, the CMake registration comment, and that file's own docstring each carried a *count*, and
the three had already drifted into describing different rounds — so none of them states a number now.

### Recovery is loud and non-blocking (07 §6)

`editor-state.json` is session state, so a document that will not load must never block the boot — and
must never be reset **silently** either. Before e09d it was: `EditorStateStore::load()` caught the
parse error, took the defaults and said nothing, so a user lost their layout and undo history with no
diagnostic and no bytes to recover from. Now an unusable document (unreadable, empty, malformed,
not an object, or from a foreign `version`) is:

1. renamed aside to `.editor/editor-state.corrupt.json` (`-1`, `-2`, … if taken — evidence is never
   clobbered),
2. replaced by defaults, with `load()` filling an `EditorStateRestoreReport`, and
3. announced **loudly** by `editor_main.cpp`: a line on stderr *and* an `editor.editor_state_invalid`
   diagnostic pushed into the **Problems panel** through `report_local_problem` — the same dispatch a
   daemon diagnostic takes, because a second rendering path for "a problem" is how the two drift.

A **foreign (future) `version` is quarantined too**, which looks wrong and is not: leaving it in place
is not preservation. The store runs on defaults and the first dirty flush replaces the file, so the
newer build's state is destroyed either way — quarantining is the only version where a copy survives.

`editor.editor_state_invalid` is its own catalog code, never the daemon's
`editor.session_state_invalid`: a recovery diagnostic that cannot say WHICH file was reset has thrown
away the only distinction the split created.

⚠ One thing to know if you add a Shell-local diagnostic: put the path in the payload's **`file`**
member, not `pointer`. `ProblemsFeed`'s parser reads both, but only `file` is rendered and navigable,
so a path in `pointer` is invisible in the panel.

## 15. The window chrome — `chrome.state`, the strips, and the OS frame (editor-window-chrome a1–g1)

The Shell's window was stock OS chrome until 2026-08-28; the **editor-window-chrome** set (Taskflow
`.taskflow/2026-08-28-editor-window-chrome/`, target design 02, owner decisions D1–D7) made it the
mockup's frame — titlebar / play bar / dock / statusbar — without a windowing framework (D5: no
Electron; L-15/L-41 stand). This section records how the repository implements it; the closing task
g1 verified it, and the table at the end names where every claim lives.

**The contract (a1, 02 §1).** ONE new bridge read, `chrome.state`, on the `window.*` surface
(`window_bridge.h` `kChromeStateMethod`, bound in `editor_main.cpp`'s `bind_chrome_state`), fetched by
editor-core at boot beside `welcome.state` (`window.ts`, a total parser that defaults to
`system`/`primary`):

```
{ mode: "custom" | "hybrid" | "system",   // what the LIVE backend does — win32 custom, cocoa hybrid, x11 system
  controlsInset: { left, right },         // physical px the strip must reserve (macOS traffic lights; else 0)
  maximized, focused,                     // boot snapshots; runtime flips are FACTS (below)
  window: "primary" | "secondary" }       // derived from the window id the boot seed already distinguishes
```

Beside it, three window-control verbs (`window.minimize`, `window.toggle-maximize` — the toggle is
composed in the handler from `placement().maximized`, so the renderer never has to know the state to
flip it — and `window.focus`, with an optional `windowId` since d3) join the existing `window.close`;
`IWindowBackend` gained the two chrome VERBS `minimize()` / `set_maximized(bool)` (pure, every backend
answers: X11's private EWMH `_NET_WM_STATE` shape promoted to the seam plus `XIconifyWindow`, Cocoa
`miniaturize:`/`zoom:`, Win32 through the placement machinery, headless honest state-only) and the two
chrome FACTS `set_chrome_regions` / `set_appearance` (NOT pure — "ignore it" is a truthful answer for
a platform that renders stock chrome; `window.h` states the split). A maximize can come from anywhere
— the button, a caption double-click, Win+Up, the WM — so the glyph never polls: the 250 ms placement
poll that already detects the flip publishes it as the **`editor.ui.chrome` fact** (`chrome_facts.h`,
UNICAST to the affected window, `shell` origin, the eighth built-in `editor.ui` topic), which
editor-core drains on its existing `ui.mirror-poll`. Being a boot-time surface, every one of these
is installed in ALL TEN live CEF smokes in the PR that introduced it (the ten-smoke rule,
`window_bridge.h`) — a1 for `chrome.state` + the three verbs, d1 for `session.control`, d3 for
`menu.publish`.

**The ✕ — `window.close` is TWO asks, and for a long time only one of them worked.** The titlebar's
close button and the Window menu's Close / Quit all dispatch `window.close` on THIS window's bridge,
and the Shell decides what that means: a SECONDARY window is destroyed (its panels rehome to window 0
first — the D6 relay above), while the PRIMARY window's close is the **app quit**. It has to be:
window 0 hosts the app menu and the welcome screen, so `destroy_window` refuses it by design
("it closes with the app, not on its own"). Until 2026-08-29 nothing acted on the other half of that
sentence — the app's handler called `destroy_window` directly, so the primary's ✕ dispatched its
verb, collected `primary-refused`, and did NOTHING, while the minimize and maximize buttons 40 px to
its left worked perfectly. `WindowManager::close_window` is now the ONE home of that policy
(`shell.h`): unknown id → `unknown-window` (a stale id must never quit the app), secondary →
`destroy_window`, primary → `request_quit`, which ASKS every live window's backend to close — the
same ask the OS makes for Alt+F4, per backend — and lets each die on its own next pump, so windows are
retired through the path that already exists, `pump_once` reports the empty registry, and the owner
loop ends through its ordinary termination condition with the whole teardown sequence (state flush,
`shutdown()`, `CefShutdown`) unchanged. `destroy_window`'s primary refusal is deliberately untouched:
it stays the honest answer to "destroy window 0 as a window", which is what makes `close_window` the
only path that can close it. Pinned by `editor-shell-test_window_registry` (the quit reaches EVERY
window — a quit that reached only window 0 would leave a secondary running the app with no menu bar —
the secondary path, the still-refusing `destroy_window`, and the stale-primary-id case) and verified
live on Windows: with the measured `HTCLOSE` rect clicked by a real synthetic press, the process exits
0 and writes its editor state, in both project and welcome modes.

**The strips (a2, 02 §2).** `app/index.html` is a flex column: `#editor-titlebar` (38 px) /
`#editor-playbar` (40 px) / `#editor-root` (flex: 1, the dock) / `#editor-statusbar` (24 px), with
`#editor-banners` staying the fixed overlay. Strips are app chrome in the banners pattern — styled in
`app.css` from existing tokens, every CONTROL inside is a kit component, no new kit family, no new
tokens, no inline styles (CSP). They render in BOTH welcome and project modes; the play bar hides on
the welcome screen (no session to control). The titlebar's content is mode-gated off `chrome.state`
(`chrome.ts` `mountChrome`): `custom` = brand + menubar + title + the window-controls cluster;
`hybrid` = the same minus the cluster, left-padded by `controlsInset` through two CSSOM custom
properties; `system` = a menu-bar-only strip with no drag duty. The DOM tier proves all three by
INJECTING states, independent of the live backend. Observables a test or a smoke reads:
`data-chrome-mode` / `data-chrome-window` / `data-maximized` / `data-chrome-control` on the strip's
elements, and the `<html>` boot reports `data-editor-strips`, `data-editor-playbar`,
`data-editor-statusbar`. This deliberately spent what `index.html` used to protect: the dock shrank
by 102 px and the CEF smokes' per-pixel coverage floor was RECALIBRATED, value unchanged (ROADMAP
risk 1) — a coverage delta outside the strips' own pixels is a regression, not an expectation to
widen.

**The region flow (a1/a2, 02 §6) — the vocabulary grew, the seam did not.** `RegionKind` gained
`caption`, `caption_min`, `caption_max`, `caption_close` (wire tokens `caption`, `caption-min`, …) in
its CLOSED vocabulary, landed atomically across all four mirror sites (`input.h`,
`editor_state_bridge.h`, `editorstate.ts`, the `webui-panel-contract` gate — the standing rule is that
they move together in one commit). editor-core's titlebar is the FIRST real `regionProvider` the
e05d2 channel ever had: it measures its caption drag surface and the three control rects
(`getBoundingClientRect` → PHYSICAL px, edge-rounded so a published edge can never overhang or bite a
neighbour on a fractional DPR) and publishes them WHOLESALE — caption FIRST, controls after, so the
arbiter's back-to-front last-match-wins needs no carve-out token — on layout change, window resize,
DPI change, and once more when the d3 menubar fills its column. In `system` mode it publishes an
EMPTY set: "no drag duty" is a fact the Shell must also see. From the arbiter's `RegionMap` the map
reaches its consumers three ways: `EditorWindow::pump_once` pushes it DOWN to the backend
(`set_chrome_regions`, generation-gated, wholesale) for Windows' NC hit-test; the Cocoa pump consults
the arbiter's map at NSEvent time (§ 3); and the shared arbitration arm (`input.cpp` `target_for`)
routes the caption DRAG surface `native` — where `shell.cpp`'s native arm drops it, which IS the
suppression on every backend without an OS-frame consumer — while the CONTROL rects route to the
BROWSER, because the buttons they outline are web-drawn and the click must reach the web button that
draws the glyph. The region ids are grep-stable (`chrome.caption`, `chrome.caption-min`,
`chrome.caption-max`, `chrome.caption-close`) and mirrored by the C++ smokes.

**Windows — `custom`, frameless by the standard NC takeover (b1, 02 §3).** The style stays
`WS_OVERLAPPEDWINDOW` (Snap, the maximize animation, minimize-to-taskbar all preserved); the frame is
taken over in the NC messages, every DECISION a pure function over plain integers in `window.h`, run by
`editor-shell-test_window` on all three legs, and `win32_window.cpp` only converts coordinates and
applies answers:

- `WM_NCCALCSIZE` → `win32_frameless_client_insets`: restored, l/r/b keep the DPI-scaled resize
  border (8 px at 96 dpi, 12 at 150 %) and the TOP inset is ZERO — the client reaches the window's top
  edge and the web titlebar draws there; maximized, ALL sides inset by the border.
- `WM_GETMINMAXINFO` → `win32_frameless_max_geometry`: the work area inflated by the border on all
  sides, so with the maximized insets the CLIENT equals the work area EXACTLY — the classic 8 px
  overhang cancels by construction (ROADMAP risk 2), on a secondary monitor too.
- `WM_NCHITTEST` → `hit_test_frame(point, client, dpi, maximized, regions)`: resize bands FIRST
  (the l/r/b NC strips plus the first `border` rows INSIDE the client; the corner extent, 16 px at
  96 dpi, resolves the diagonals; NO bands when maximized), then the published regions by the map's
  own last-match-wins — `caption` → `HTCAPTION` (the OS then owns drag / snap / double-click / system
  menu, which is the whole point of the pattern over a hand-rolled drag loop), the controls →
  `HTMINBUTTON` / `HTMAXBUTTON` (what lights Snap Layouts on Windows 11) / `HTCLOSE`, every other kind
  → `HTCLIENT`.
- The NC MOUSE family → `translate_win32_nc_mouse`: returning the `HT*BUTTON` codes makes the control
  rects non-client, so the OS stops sending client mouse messages there — but the buttons are
  web-drawn, so NC moves over a control are FORWARDED (CSS hover), NC presses/releases are forwarded
  AND CONSUMED (or `DefWindowProc` would run the classic caption-button tracking and fire `SC_CLOSE`
  itself — a double action), and sliding off a control synthesizes the LEAVE a client-area exit would
  have produced. The caption itself is suppressed wholesale: no caption point is ever forwarded.
- `window.set-appearance` (fail-closed tokens, byte-pinned by the `webui-panel-contract` gate) →
  `DWMWA_USE_IMMERSIVE_DARK_MODE`, the ONE Dwm call the design adds, so the frame's edge tint and drop
  shadow follow the active theme (D7 skipped the interim DWM-dark phase).

No CEF `OnDraggableRegionsChanged`, no `-webkit-app-region`: the regions already had a proven
in-house channel, and a second CEF-specific one would duplicate it (02 §11).

**macOS — `hybrid`, native buttons stay native (c1, 02 §4; d3 for the menu).** § 3 has the
mechanism: `NSWindowStyleMaskFullSizeContentView` + a transparent, title-hidden titlebar; the traffic
lights float where macOS puts them and `controlsInset` is MEASURED from the real
`standardWindowButton:` frames (RTL mirror included, `cocoa_chrome.h`); a single left press on a
published `caption` rect is handed to `performWindowDragWithEvent:` and a double-click is `zoom:`,
the press consumed whole so the browser can never hold a stuck hover from a half-press; both arms
`makeKeyAndOrderFront:` first, so a background window's titlebar press cannot drag it while the
keyboard stays elsewhere. The menu does NOT render in the strip there: `menu.publish` feeds the
native global `NSMenu` bar, and an activation returns as the `editor.ui.menu` fact.

**Linux — `system`, server-side decorations (D6).** The WM owns the frame: no CSD, no window
buttons, no caption duty. The strip is the web menubar (two stacked bars is the conventional Linux
shape), the titlebar publishes an empty region set, and the X11 backend keeps `window.h`'s no-op
`set_chrome_regions` / `set_appearance` forever. What the Linux leg CAN prove — and does, in
`editor-shell-x11-window`'s g1 step — is the shared-arbitration half of the contract against samples
that made the real client → X server → client round trip: a caption gesture published in the a2
shape is arbitrated and dropped (hover, press, the drag that leaves the strip, the release — the
implicit capture holding throughout and released on the release), a sample over the dock reaches the
browser again afterwards, and a control press is forwarded INSIDE the physical rect it was aimed
at.

**The play bar (d1, 02 §7), the statusbar (d2, §8), the menu (d3, 03).** The play bar is the
mockup strip — status dot + label, the `t+` timer, a disabled "Scene" Target chip (declared future
surface), transport buttons — rendered by editor-core (`playbar.ts`) into the a2 slot and driven by
the existing 500 ms `session.state` poll, whose reply gained an ADDITIVE `simTick` (the daemon already
minted it) so the timer is truthful. Control rides ONE new bridge method, `session.control {verb:
play|pause|stop|step}` on the existing `SessionBridge`, relaying to the surviving `SessionFeed` writer
with its `origin` echo suppression; editor-core registers real `play.play/pause/stop/step` commands
(the retired panel's ids) so the strip's buttons, the palette and the menu share one write path. It is
the first writer of `data-play-state`, with the honest 3→5 mapping (`edit→idle`, `playing→running`,
`paused→paused`; `compiling`/`error` stay unreachable until the build pipeline publishes those facts,
pinned as such). FPS is NOT rendered — nothing measures it until e11. The docked `builtin.playbar`
panel was RETIRED (e1, D2): roster entry, a11y factory + manifest row, help topic, `hostable_panel_ids`
6 → 5, and the enumerated m5/m85 frozen gates AMENDED owner-visibly (the e06d five-gate-partition
precedent); the `PlaybarModel`/`SessionFeed` transport survives untouched and still carries the id.
The statusbar renders exactly what already has a truthful source: daemon link state (the banners'
`daemon.linkState` feed), the problems count, the active theme, the project name. The menu is ONE
declarative model in editor-core (`menu.ts`, 03's tree: App on macOS / File / Edit / View / Selection /
Panel / Window / Help, every item a command id in the e07b registry, enablement sourced from
`CommandAvailability`), rendered as a web menubar inside the titlebar on Windows/Linux (ARIA
`menubar`/`menu`/`menuitem`, arrow/Enter/Escape/Home/End navigation) and as the native `NSMenu` bar
on macOS through `menu.publish` (§ 3) — no second dispatch system, pinned by a test that feeds one
spy from both the palette and a menubar click.

**Secondary windows (f1, 02 §9, D4).** A torn-out window gets the same chrome mode with a COMPACT
strip: panel title + the controls cluster (`custom`) or the inset padding (`hybrid`), no brand, no
palette button — and the play-bar + statusbar elements are REMOVED from the document outright, so
"no menu / play bar / statusbar there" is structural, never a hidden node. `chrome.state.window ==
"secondary"` is the gate, observable as `data-chrome-window`; the compact strip publishes its own
caption + control regions over ITS window's channel; framelessness holds for every window the factory
creates because they take the same `WindowDesc` → backend path as window 0 (pinned live in
`editor-cef-smoke-shell-tearout` and, for the Cocoa style mask, in `editor-shell-cocoa-window`).

**The interim-honesty staging — now history.** The set was cut so that `chrome.state.mode` always
reported what the backend actually DID, never the target table: a1 shipped every backend as `system`
with ZERO visual change, b1 flipped win32 → `custom` and c1 flipped cocoa → `hybrid` in the same PRs
that made those true, x11 stays `system` by design. That is what avoided double chrome (web controls
over a stock OS titlebar) between waves, and why a2 implemented and DOM-tested all three modes by
injecting states before any live backend reported them. It is closed: every backend now reports what
it does, and `editor-shell-cocoa-window` re-reads the Cocoa style mask off the live `NSWindow` at
call time, so the report can never again outrun the behaviour.

**Where each chrome claim lives (the g1 verification map).**

| Claim | Pinned by |
|---|---|
| `chrome.state` shape, the three verbs, honest unbound degrade, `window.focus` `windowId` refusal | `editor-shell-test_window_bridge`; the read + verbs asserted with `bridge.refused() == 0` in all ten CEF smokes |
| The two backend chrome verbs on every backend; the headless recorder for the push-down | `editor-shell-test_window` (the `headless_backend_chrome_*` cases), `editor-shell-test_shell` (the generation-gated push) |
| The `maximized` fact: envelope, unicast, no phantom boot fact at the real poll interval | `editor-shell-test_chrome_facts`, `editor-shell-test_shell` |
| Four caption tokens in all four mirror sites, unknown kind refused | `editor-shell-test_editor_state_bridge`, `webui-panel-contract`, `webui-ts-unit` (`editorstate.test.ts`) |
| The strips, mode gating, glyph flip, physical-px measurement at dpr 2, publisher triggers, secondary compact strip, ARIA | `webui-ts-unit` (`chrome.test.ts`, `boot.test.ts`, `window_a11y.test.ts`) |
| Four chrome rects arrive in the live `RegionMap` in physical px, caption first, resize re-publishes; coverage floor recalibrated | `editor-cef-smoke-shell` (a2 step) |
| Arbitration: caption → native, controls → browser, implicit capture across a drag | `editor-shell-test_input`, `editor-shell-test_shell` |
| Insets, max geometry (no 8 px overhang at 96 and 150 %), band + corner metrics, `hit_test_frame` precedence, NC mouse forwarding/consume/leave | `editor-shell-test_window` (b1 cases) |
| `hit_test_frame` over EVERY point of the window rect — five DPIs (incl. 100 and 150, where the metric rounding rule is exercised), both frame states, three maps (empty / a2 / overlapping) — against a spec oracle plus oracle-free invariants (closed answer set, no bands maximized, band-map independence, mirror symmetry, measured metrics, DPI monotonicity) | `editor-shell-test_window` (the g1 sweep corpus) |
| The live rects answer the NC codes; a live caption press routes native and a control press to the browser; restored-vs-maximized top row | `editor-cef-smoke-shell` (b1 step), `editor-cef-smoke-shell-tearout` (f1, the secondary's own map) |
| Cocoa: style mask live, inset measured and positive, caption press → drag consumed whole, double-click → `zoom:` really zooms, release still arbitrated, non-caption press forwarded, factory window carries the mask, menu build + activation round trip | `editor-shell-cocoa-window` (c1 / d3 / f1 steps, macOS `editor-cef-smoke` job); pure decisions in `editor-shell-test_cocoa_chrome`, `editor-shell-test_menu_model`, `editor-shell-test_menu_facts` |
| X11: a caption gesture through the real X server suppressed end to end, capture released, dock forwarded afterwards, control press forwarded inside its physical rect | `editor-shell-x11-window` (the g1 step, Linux `editor-cef-smoke` job) |
| `session.control` → `SessionFeed`, `simTick` relay, `data-play-state` mapping, the retired dock panel absent everywhere it was anchored | `editor-shell-test_session_bridge`, `editor-shell-test_session_feed`, `webui-ts-unit` (`playbar.test.ts`), `editor-shell-test_builtin_panels`, `gui-contract`/`gui-a11y`/`gui-help` roster gates, the amended `m5-exit-1/2/3` + `m85-exit-4c` gates |
| Statusbar content and its ARIA groups | `webui-ts-unit` (`statusbar.test.ts`) |
| The menu model's total parse, caps, accelerator tokenizer; the web menubar's keyboard map; one dispatch path | `editor-shell-test_menu_model`, `webui-ts-unit` (`menu.test.ts`) |

**The ROADMAP's six named risks, closed out.** (1) the CEF-smoke pixel-coverage floor: recalibrated
for the 102 px-shorter dock with its VALUE unchanged (`cef_shell_smoke.cpp` § the coverage floor);
(2) the frameless-maximized overhang: `test_maximized_client_lands_exactly_on_the_work_area_at_96_and_150_percent`
plus the sweep's "no band anywhere maximized" invariant; (3) caption drag vs CEF input: the suppression
is asserted live on all three legs — `editor-cef-smoke-shell` (headless arbitration + the NC decision
table), `editor-shell-cocoa-window` (the consumed press, the arbitrated release, no leaked capture),
`editor-shell-x11-window` (the whole gesture through the X server) — and pure in `test_input` /
`test_shell` / `test_window`'s synthetic-leave case; (4) the m5-exit amendments: enumerated in PR #486's
body per the standing gate, none deleted; (5) `chrome.state` on the welcome screen: the strips render
there, the play bar hides, the welcome smokes joined a2's update set; (6) play-state staleness after a
daemon restart (CE #356): INHERITED by the strip, documented in `playbar.ts`, fixed upstream, not
here.

## 9. Why the blocking smoke opens no window

The Windows CI legs run on a self-hosted runner installed as a LocalSystem service — Session 0. There
is no interactive desktop, and native GPU windowed teardown crashes there; the repo has a standing
"never add a Windows native-GPU render leg" rule for exactly this reason.

So `editor-shell-smoke-session0` opens no window, creates no device and links no CEF. It does not need
to: everything between the OS and the pixels is the Shell's own code, and it runs all of it against the
real objects — the real owner loop over the honest offscreen backend, real software-OSR frames, the
real compositor including the `PET_POPUP` layer, and the real C-F2 present path into e03's
`MemoryBlitter`, which `present_blit.h` documents as "the honest present target for a
headless/offscreen shell". The composited output is asserted per-pixel, INSIDE and OUTSIDE the popup
rect — asserting both is what distinguishes a real second layer from a popup that was dropped (view
everywhere) or drawn full-window (popup everywhere).

## 10. Deferred verification — the interactive Windows pass

The task's DoD line *"full windowed pass verified on the interactive Windows box and recorded"* is
**advisory and manual at this stage**: per the design's gate table the interactive-session Windows
runner is **not provisioned**, so nothing in CI can perform it, and no automated gate here claims it.
Recorded rather than silently skipped.

### Already verified on a real interactive Windows desktop (e04, CEF-free build)

Run during e04 development with `context_editor --project <tmp> --frames 40` on an interactive
session, built WITHOUT `CONTEXT_BUILD_GUI_CEF` / `CONTEXT_BUILD_RENDER_WGPU`:

| Covered | Observed |
|---|---|
| Window creation + class registration | A real top-level window was created and shown (`RegisterClassExW` / `CreateWindowExW`). |
| The owner loop + WndProc pump | 40 iterations ran and the process exited **0** — no hang, no teardown crash. |
| Per-monitor DPI + frame adjustment | A 1280×800 logical client area produced a 1296×839 window rect, i.e. `AdjustWindowRectExForDpi` really ran. |
| Monitor identity | `MonitorFromWindow` + `GetMonitorInfoW` resolved `\\.\DISPLAY1`. |
| Placement persistence | `.editor/editor-state.json` was written by the Shell with the real placement, through the debounced crash-safe writer. |
| The C-F2 CPU present path | Selected and reported (`present path = cpu-blit`) with no render backend in the build. |

### Still manual — the CEF-dependent half

These need a build with `-DCONTEXT_BUILD_GUI_CEF=ON` (and `-DCONTEXT_BUILD_RENDER_WGPU=ON` for the
GPU present path), which cannot be produced on the local GCC dev gate:

```sh
cmake -S src --preset dev -DCONTEXT_BUILD_GUI_CEF=ON -DCONTEXT_BUILD_RENDER_WGPU=ON
cmake --build --preset dev --target context_editor    # from src/
./build/dev/editor/shell/context_editor --project <a project dir> --url <a page>
# On macOS that last path does not exist: since e12c-1 a CEF-ON configure makes context_editor an
# .app (§ 3), so the binary is at
#   ./build/dev/editor/shell/context_editor.app/Contents/MacOS/context_editor
```

| Step | Expected observation |
|---|---|
| Boot | The page is **rendered inside** the window. stdout reports `present path = gpu-swapchain` (or `cpu-blit` on a GPU-less box — also a pass, see the next row). |
| No usable adapter | Boot with the GPU disabled (or on a box with no adapter): the UI still appears, presented through GDI, and stdout reports `present path = cpu-blit`. |
| Mouse | Hover highlights follow the cursor; a click activates the element under it; the cursor leaving the window clears the hover. |
| Wheel | The page scrolls, and in the direction the wheel turned. |
| Keyboard | Typing into a text field inserts the characters, including a non-ASCII one; Tab moves DOM focus. |
| Drag | Press inside the window, drag past the window edge and release: the gesture stays with where it started and ends on the release. |
| `PET_POPUP` | Open a `<select>` / dropdown: the popup renders over the page, clipped to its own rect, and closes cleanly with no ghost of it left behind. |
| Live resize | Drag the window edge: content re-lays out and the window does not tear, flash black, or lag more than a couple of CEF paints behind. |
| Live DPI change | Drag the window between monitors at different scale factors (or change scaling while it is open): the window keeps its APPARENT size and the UI re-renders crisp, not scaled-up. |
| Minimize/restore | Minimize and restore: no crash, and the content is present again on restore. |
| `window.open` | A page calling `window.open` produces NO second native window (`OnBeforePopup` suppresses it). |
| Context menu placement (a1) | Move the window well away from the screen origin, then right-click inside it: CEF's own menu opens **at the cursor**, not offset by the window's position. Repeat at 150 % scaling and with the window MAXIMIZED (the state a placement-derived origin would get wrong), and on a second monitor placed LEFT of the primary one, where the client origin is negative. The pure arithmetic and the push plumbing are ctest-pinned on all three legs (§ 8); what only a real desktop can show is that the number CEF asks for is the one it receives. |

Automating this needs an interactive runner, which the design's gate table still tracks as
unprovisioned — **every row of the table above is still manual, on all three OSes.**

### Still manual — the window chrome (editor-window-chrome a1–f1, closed out by g1)

Every landing PR of the chrome set named its CI-unreachable remainder under this precedent; g1
collects them here so the list has ONE home. What CI DOES carry for each is named beside it, so
"manual" never reads as "unverified" — the pure halves are ctest-pinned on all three legs and the
live halves ride the windowed smokes (§ 15's verification map).

| Step | Expected observation | What CI already pins |
|---|---|---|
| Windows: drag the titlebar strip | The window MOVES with the pointer, the OS drag (not a hand-rolled loop); a double-click on the strip maximizes / restores; right-click on the strip opens the system menu | `hit_test_frame` → `HTCAPTION` over the live rects (`editor-cef-smoke-shell`, Windows self-hosted leg); the sweep corpus |
| Windows: Snap | Win+Arrow, drag-to-edge and Snap Layouts on hover over the web-drawn maximize button all behave as on a stock window | `HTMAXBUTTON` over the live maximize rect; NC moves over a control forwarded, never consumed (`translate_win32_nc_mouse`) |
| Windows: the web-drawn controls | Hover lights them (CSS), a click minimizes / toggles maximize / closes exactly once (no double action from `DefWindowProc`'s classic tracking), sliding off a control clears its hover | NC press/release forwarded AND consumed, the synthetic leave — `editor-shell-test_window`; the verbs — `editor-shell-test_window_bridge`; the close POLICY — `editor-shell-test_window_registry`. ⚠ This row is why the dead ✕ shipped: it was never actually walked, and the ✕ is the one control whose verb the Shell could refuse. Verified by hand 2026-08-29 (minimize as the positive control, then close → exit 0). |
| Windows: maximize on a multi-monitor mix (a secondary monitor, a docked taskbar, 150 %) | The client fills the work area exactly — no 8 px spill off any edge, no letterbox gap | `win32_frameless_max_geometry` ∘ `win32_frameless_client_insets` == work area at 96 and 144 dpi, taskbar offset included |
| Windows: the DWM dark-mode tint | Switching the theme flips the frame's edge tint / drop shadow to match | `window.set-appearance` tokens (`editor-shell-test_window_bridge`, `webui-panel-contract`); the Dwm call itself is CI-unreachable |
| Windows: the real NC message stream | `WM_NCCALCSIZE` / `WM_NCHITTEST` / the NC mouse family arriving from a REAL `HWND` on an interactive desktop | The pure decisions on all three legs; the Session-0 runner cannot deliver NC messages |
| macOS: drag the strip by hand, double-click it | The window moves under the real cursor; a double-click zooms | `editor-shell-cocoa-window` drives the same consult PROGRAMMATICALLY (`postEvent:` → `performWindowDragWithEvent:` / `zoom:` — the zoom is observed, the drag's window move is not asserted because AppKit's synthesized-event drag needs a real cursor) |
| macOS: click the real on-screen menu bar | The item's command runs; the key equivalent (⌘-mapped from the published accelerator) runs it too | The `NSMenu` build + `cocoa_menu_perform` activation round trip, disabled items refused |
| macOS: the traffic lights over the strip | The strip's content starts to the right of the buttons at every window width; no control sits under them | The inset is MEASURED from the real button frames and asserted positive and sane; the padding is the DOM tier's |
| Linux: the strip under a real window manager | Two stacked bars — the WM's titlebar with its own buttons above the web menubar; dragging the WM's bar moves the window, dragging the strip does NOT (no drag duty in `system` mode) | An empty region set in `system` mode (`chrome.test.ts`); the arbitration half through the X server (`editor-shell-x11-window`); bare Xvfb in CI has no WM, so WM behaviour is not reproducible there |
| Web menubar: Alt-mnemonics | Alt+F opens File, … | NOT implemented (d3 deferred and recorded); arrows / Enter / Escape / Home / End are pinned |
| All OSes: the chrome against the d1 mockups, pixel for pixel | Strip heights, spacing, typography and the play-bar flourish match `mockups/editor.html` in both themes | Nothing pixel-level — this is what **e16**'s visual-regression harness is handed (the m9-editor backlog records the hand-off) |

What **e12a** moved onto CI is the layer *underneath* that table, on Linux: the CEF-FREE
window/present/event spine. The `context_editor_shell_x11_smoke` executable — run directly under
xvfb with `--require-x11 --require-display` in the `editor-cef-smoke` Linux leg, not via its
`editor-shell-x11-window` ctest registration, which deliberately SKIPs where there is no display —
opens a REAL X11 window, presents real frames through the real X11 blitter, and asserts a
server-driven repaint (`XClearArea` → `Expose`) and a server-granted resize (`XMoveResizeWindow` →
`ConfigureNotify`), plus the placement readback and the session-state flush. **e12a-x11-legs** (#408)
added the input half: a pointer move/press/release and a key press INJECTED THROUGH THE X SERVER
(`XSendEvent` into the smoke's own window) and decoded by the real `translate_x11_event`, asserted
down to exactly one press and one release and the decoded `windows_key_code`.

**e12c-3** (#442) did the same for macOS, with two platform differences worth knowing. The
`context_editor_shell_cocoa_smoke` executable — run directly with `--require-cocoa --require-display`
in the `editor-cef-smoke` **macOS** leg, not via its `editor-shell-cocoa-window` ctest registration,
which SKIPs where there is no GUI session — opens a REAL `NSWindow`, presents through the real
`CALayer.contents` blitter, and injects a pointer pair and a key IN-PROCESS with
`-[NSApplication postEvent:atStart:]`, decoded by the real `translate_ns_event`. **(1)** There is no
OS-driven repaint to assert: Cocoa has no window-event queue at all (geometry is polled and diffed,
`cocoa_window.mm` shape 2), so the OS-sourced geometry claim is the granted RESIZE alone. **(2)** The
delivered pointer location is only approximate — AppKit scales it about the window centre — so the
position claim is the Y-flip direction plus separation, never an equality. Both are recorded in §11.

It does **not** shrink the table: it links no CEF and drives `ScriptedBrowserHost`, so the
CEF-dependent rows — including the functional ones (wheel, keyboard, `PET_POPUP`, `window.open`) —
remain manual for their OBSERVABLE outcomes (a hover highlight, DOM focus). What is no longer equal
across the three OSes is who DELIVERS the gesture: since e12a-x11-legs the Linux `editor-cef-smoke-shell`
leg drives a real pointer gesture and a real Tab into a LIVE CEF browser through the X server, which
Windows and macOS still do not (on macOS the real-window proof is the CEF-FREE smoke above).

## 16. The OSR contract audit (D12)

The editor runs CEF **windowless (OSR)** (§ 5), which puts on the host a burden a normal browser
window gets from the OS for free: reporting geometry, accepting pixels, delivering input, and driving
the drag protocol. This section is the first full walk of that contract as a list, rather than one
member discovered per bug report — three of the owner's seven 2026-08-29 UX reports (the dead tab
drag, the misplaced/click-swallowing dropdown, and the offset context menu) all traced to
unimplemented members this table would have named on day one. Produced for task `a0`
(`.taskflow/2026-08-29-editor-ux-packages-events/tasks/a0-osr-contract-audit.md`; design doc
`03-osr-geometry-and-drag.md`, decision `D12`). **This is a table, not a code change.**

**Audited against CEF `149.0.6+g0d0eeb6+chromium-149.0.7827.201`** (`chromium_version:
149.0.7827.201`) — the version pinned in `tools/cef-prebuilt.json`, the single source of truth
`tools/fetch_cef.py` reads. The three headers below were read verbatim from that exact pinned
distribution (`include/cef_render_handler.h`, `include/cef_browser.h`,
`include/cef_context_menu_handler.h`), staged standalone with
`python tools/fetch_cef.py --triple x86_64-pc-windows-msvc --dest <dest>` — the identical fetch
`context_acquire_cef()` (`cmake/ContextCef.cmake`) runs at configure time into
`<CMAKE_BINARY_DIR>/_cef/<triple>/include/` (e.g. `src/build/dev/_cef/x86_64-pc-windows-msvc/include/`
with the `dev` preset). Reading the headers needs no MSVC and no link step, so this audit runs on the
box whose GCC dev gate cannot build the CEF-linking targets (see this repo's `CLAUDE.md`).

`ShellCefClient` (`src/editor/shell/cef/src/cef_shell.cpp:668-673`) derives `CefClient`,
`CefRenderHandler`, `CefLifeSpanHandler`, `CefLoadHandler`, `CefDisplayHandler` and
`CefRequestHandler` — **not** `CefContextMenuHandler`, and nothing in `src/` derives `CefDragHandler`
or calls the windowless-only `CefBrowserHost` drag/IME/visibility family.

### `CefRenderHandler` — 17 of 17 members

| Member | Verdict | Detail |
|---|---|---|
| `GetAccessibilityHandler` | gap | An OS-level screen reader sees nothing in this window; the `gui-a11y-*` gates assert the C++ models and the DOM only, which is honestly green and still blind here — sharp for a repo with R-A11Y-001. Registered outside this set's scope (no task id yet); tracked in `01-current-architecture.md` §1 and this table pending a follow-up issue. |
| `GetRootScreenRect` | **implemented** | Closed by task `a1`. `cef_shell.cpp:794` — the CLIENT rect on screen, always in DIP (`osr_root_screen_rect`, dpi.h), with no per-platform split: the header's own fallback for this member is `GetViewRect`, so the honest correction of it for a windowless browser whose view IS the client is that same rect moved to where it really is. |
| `GetViewRect` | **implemented** | `cef_shell.cpp:777` — reports the view in DIP. |
| `GetScreenPoint` | **implemented** | Closed by task `a1` — the default returned `false`, so CEF treated view coordinates as screen coordinates, which **was** the reported offset context menu (owner item #5). `cef_shell.cpp:807` — view DIP + the window's CLIENT origin through `osr_screen_point` (dpi.h), taking the per-platform device/DIP split `GetRootScreenRect` above must NOT take. macOS remains an honest zero-origin (no NSView owner exists there yet — § 5), i.e. unchanged rather than wrongly changed. |
| `GetScreenInfo` | **implemented** | `cef_shell.cpp:817` — `device_scale_factor` plus the per-platform DIP/device screen-rect split. |
| `OnPopupShow` | **implemented** | `cef_shell.cpp:835`. |
| `OnPopupSize` | **implemented** | `cef_shell.cpp:845` — the rect itself is reported correctly, in DIP; the bug that mis-places/crops the popup is downstream, in how the compositor consumes it (`01-current-architecture.md` §2; task `a2`), not a gap in this member. |
| `OnPaint` | **implemented** | `cef_shell.cpp:857`. |
| `OnAcceleratedPaint` | deliberately not needed | Owner ruling 2026-07-19: stock wgpu-native exposes no external-texture import and a patched fork was rejected; rationale recorded in `cef_shell.h:13-17`. The seam stays wired (`CefShellOptions::accelerated_osr` feeds e03's `OsrImportOptions`) and `shared_texture_enabled` is left at its default (off), so CEF never calls this. |
| `GetTouchHandleSize` | gap | No touch input pipeline exists anywhere in the Shell (`input.h` has no touch event type). `01-current-architecture.md` §1 already flags this "not needed today" but explicitly **not yet decided** — no ruling is recorded. Tracked here pending a follow-up issue. |
| `OnTouchHandleStateChanged` | gap | Pairs with `GetTouchHandleSize` above — same absent decision, same absent input source. |
| `StartDragging` | gap | Default returns `false`, which the header defines as *"abort the drag operation"*: every HTML5 drag in the editor is actively refused (owner item #3, dead tab drag). Closed by task `b1`. |
| `UpdateDragCursor` | gap | No drag-feedback cursor once dragging exists. Closed by task `b1`. |
| `OnScrollOffsetChanged` | gap | `01-current-architecture.md` §1: "not needed today" but explicitly **not yet decided**; no ruling recorded. Tracked here pending a follow-up issue. |
| `OnImeCompositionRangeChanged` | gap | Composition-based text input has no candidate-window placement. Registered outside this set's scope (IME family); tracked in `01-current-architecture.md` §1 pending a follow-up issue. |
| `OnTextSelectionChanged` | gap | OS services that read the current selection (e.g. a screen reader's text cursor, macOS Services) get nothing. Registered outside this set's scope; tracked pending a follow-up issue. |
| `OnVirtualKeyboardRequested` | gap | Same IME family as `OnImeCompositionRangeChanged` — an on-screen keyboard is never shown or hidden automatically. Registered outside this set's scope; tracked pending a follow-up issue. |

Count: 17/17 — **seven** implemented, one deliberately not needed, nine gaps. (The audit counted five
and eleven; `a1` closed `GetScreenPoint` and `GetRootScreenRect`. The counts are updated in place
rather than left as of the audit date, so this table stays a statement about the CURRENT tree — which
is the only form in which it can be re-audited.)

### `CefBrowserHost` — windowless-only members

Enumerated from the pinned `include/cef_browser.h`. Each is documented *"only used when window
rendering is disabled"* (or, for `SetFocus` / `SendKeyEvent` / `SendMouse*` / `SendTouchEvent`, is the
windowless input-injection surface the render-handler contract above depends on).

| Member | Verdict | Detail |
|---|---|---|
| `SetFocus` (`cef_browser.h:401`) | **implemented** | `cef_shell.cpp:1380`. |
| `WasResized` (`:699`) | **implemented** | `cef_shell.cpp:1307` — the resize protocol: makes CEF re-read `GetViewRect` and repaint. |
| `WasHidden` (`:707`) | gap | No call site in `src/`. CEF keeps laying out and calling `OnPaint` at full rate while the window is minimized or hidden — a CPU/GPU cost with no correctness effect. Newly found by this audit, no task id; tracked here pending a follow-up issue. |
| `NotifyScreenInfoChanged` (`:730`) | gap | No call site. `resize()` (`cef_shell.cpp:1296-1308`) drives only `WasResized` on a DPI change, and the CEF SDK's own doc comment for `WasResized` (`cef_browser.h`) says it re-reads `GetViewRect`/`OnPaint`, not `GetScreenInfo`/`GetRootScreenRect` — note this is narrower than `resize()`'s own local comment, which says `WasResized` re-reads `GetScreenInfo` too; that local comment is itself inaccurate against the pinned SDK doc, a pre-existing mismatch outside this task's scope. So a live DPI change may not refresh CEF's own `window.devicePixelRatio` / `screen.*` JS values. Newly found by this audit, no task id; tracked here pending a follow-up issue. |
| `SendKeyEvent` (`:751`) | **implemented** | `cef_shell.cpp:1372`. |
| `SendMouseClickEvent` (`:758`) | **implemented** | `cef_shell.cpp:1342,1346`. |
| `SendMouseMoveEvent` (`:768`) | **implemented** | `cef_shell.cpp:1334,1339`. |
| `SendMouseWheelEvent` (`:780`) | **implemented** | `cef_shell.cpp:1350`. |
| `SendTouchEvent` (`:788`) | gap | Same absent decision as `GetTouchHandleSize` / `OnTouchHandleStateChanged` above — no touch input pipeline exists in the Shell. Tracked here pending a follow-up issue. |
| `NotifyMoveOrResizeStarted` (`:801`) | gap | Popups are not dismissed or repositioned when the window moves. Registered outside this set's scope; already on `03-osr-geometry-and-drag.md`'s pre-registered gap list (§ "a0 — The OSR conformance audit"), pending a follow-up issue. |
| `SetWindowlessFrameRate` (`:821`) | deliberately not needed | The frame rate is fixed at browser-creation time via `CefBrowserSettings.windowless_frame_rate` (`cef_shell.h:116`, set at `cef_shell.cpp:1791-1792`; default 60, most smokes pin 10) and never needs a runtime change — consistent with `cef_shell.h`'s `NEVER SendExternalBeginFrame … CEF-internal pacing only` rule for the same reason: let CEF own pacing rather than drive it from the host. |
| `ImeSetComposition` and siblings — `ImeSetComposition` (`:849`), `ImeCommitText` (`:865`), `ImeFinishComposingText` (`:876`), `ImeCancelComposition` (`:885`) | gap | The host-injection half of the same IME family as `OnImeCompositionRangeChanged` / `OnVirtualKeyboardRequested` above — no call site for any of the four. Registered outside this set's scope; tracked pending a follow-up issue. |
| `DragTargetDragEnter` / `DragTargetDragOver` / `DragTargetDragLeave` / `DragTargetDrop` (`:897-927`) | gap | No call site — the injection half of the drag protocol `StartDragging` above needs. Closed by task `b1`. |
| `DragSourceEndedAt` / `DragSourceSystemDragEnded` (`:939-951`) | gap | No call site. Closed by task `b1`. |

### `CefContextMenuHandler` — 0 of 7 members

Not derived by `ShellCefClient` at all (`cef_shell.cpp:668-673`), so CEF displays its own built-in
context menu for every member below.

| Member | Verdict | Detail |
|---|---|---|
| `OnBeforeContextMenu` (`cef_context_menu_handler.h:106`) | deliberately not needed | CEF's own built-in menu is used; task `a1`'s own scope note records this as acceptable: *"CEF's own menu, positioned through this callback, is acceptable once positioned correctly."* The reported offset (owner item #5) was fully explained by the then-missing `GetScreenPoint` above, not by this handler — and `a1` closed it there, leaving this row's verdict unchanged. |
| `RunContextMenu` (`:120`) | deliberately not needed | Same reason. |
| `OnContextMenuCommand` (`:138`) | deliberately not needed | Same reason. |
| `OnContextMenuDismissed` (`:151`) | deliberately not needed | Same reason. |
| `RunQuickMenu` (`:163`) | deliberately not needed | Same reason (the touch-context-menu counterpart of `RunContextMenu`; moot while `SendTouchEvent` above is also a gap). |
| `OnQuickMenuCommand` (`:179`) | deliberately not needed | Same reason. |
| `OnQuickMenuDismissed` (`:191`) | deliberately not needed | Same reason. |

### What this closes and what it opens

- Replaces an impression ("is this the wrong framework?",
  `.taskflow/2026-08-29-editor-ux-packages-events/README.md` § "The framework question, answered
  once" — not the repo-root `README.md`, which has no such section) with a count: 5 implemented at
  audit time (**7 now** — see the render-handler count above), 3
  deliberately-not-needed groups (`OnAcceleratedPaint`,
  `SetWindowlessFrameRate`, all of `CefContextMenuHandler`), and the rest gaps.
- Tasks `a1` and `b1` each close a named subset of the gaps above; their own task files cite the exact
  rows they close. **`a1` has landed**: `GetScreenPoint` + `GetRootScreenRect`, with the arithmetic in
  `dpi.h` and the client-origin channel through `IWindowBackend::client_origin()` →
  `IBrowserHost::set_client_origin()` (§ 5). `b1`'s drag rows are still open.
- Six gaps surfaced by this audit were **not** already on `03-osr-geometry-and-drag.md`'s
  pre-registered list: `WasHidden`, `NotifyScreenInfoChanged`, `SendTouchEvent` (plus its render-handler
  counterparts `GetTouchHandleSize` / `OnTouchHandleStateChanged`), and `OnScrollOffsetChanged`. None
  has a follow-up issue filed yet — filing one each is the natural next step, and this table is what a
  filed issue should point back to.
- No promise is made here about when any gap row gets fixed — per this task's own scope, that is a
  decision for whichever task or issue closes the row.

## 11. What this does NOT yet do

Named so the gaps are visible rather than assumed:

- ~~**No Linux window backend.**~~ Landed by **e12a**: `x11_window.cpp` (X11/XWayland, D21), the pure
  `translate_x11_event` decoder, the X11-SHM present blitter, and the live `editor-shell-x11-window`
  smoke that opens a REAL window and asserts a server-driven repaint and resize.
- ~~**The live CEF scenario smokes never run through a real window.**~~ Landed by **e12a-x11-legs**
  (#408). All **ten** `editor-cef-smoke-shell*` smokes now take their window through the shared
  smoke-tier seam `src/editor/shell/smoke/smoke_window.h`, and the ctest registration passes
  `--real-window` on **Linux**, where they open a REAL X11 window and present through the REAL X11
  blitter `EditorWindow::attach_cpu_present()` selects; the **two** of them that drive input take it
  FROM THE X SERVER (for the other eight the server is the source of `Expose`/`ConfigureNotify`
  only). The Windows leg keeps the offscreen backend, which is what the Session-0 runner requires. Two earlier
  claims here were WRONG and are corrected rather than deleted, because both were load-bearing for
  the "this is not a constructor swap" reading: the count is **ten**, not eight (`-uimirror`,
  `-iframe` and `-inspector-fanout` landed after this note was written), and only **two** of them ever
  `post()`ed at all —
  the other eight built a `HeadlessWindowBackend` and drove their scenarios entirely through the CEF
  bridge, so for those it genuinely WAS a construction swap. What was not a swap is INPUT: real mode
  sends pointer and key events to the smoke's own window through the X server (XSendEvent with an
  empty event mask, which the protocol delivers back to the creating client), so they re-enter
  through the real `translate_x11_event`; a `post()`-shaped seam on the real backend would have
  bypassed the server, the decoder and the window path, and passed with all three broken.
- **A RETIRED window keeps its OS window until the process exits.** Surfaced by e12a-x11-legs, but a
  property of the shipping lifecycle rather than of the smokes: `WindowManager::retire()` MOVES the
  whole `EditorWindow` into `retired_` (the CE #319 rule — the browser dies now, the session lives
  until the manager does), `finish_close()` detaches the compositor and calls `backend_->close()`,
  and the X11 `close()` only posts itself a `WM_DELETE_WINDOW`; the window is destroyed in
  `~X11WindowBackend`, which now runs at `~WindowManager`. Since `pump_once` iterates `windows_`
  only, a retired backend also never drains its queue again. Offscreen this was invisible; with a
  real window it means a destroyed (e.g. torn-out-then-closed) window stays MAPPED for the rest of
  the run. Harmless for the smokes — they assert `retired_session_count()`, not the desktop — and
  deliberately not changed here, since releasing the backend at retire touches the shipping window
  contract this task left alone. Named so the e10 tear-out author does not discover it as a surprise.
- **No live DPI-change event on Linux.** `Xft.dpi` (falling back to the screen derivation) is read
  ONCE, in `X11WindowBackend::create`. Nothing watches `RESOURCE_MANAGER` on the root window for the
  `PropertyNotify` that a desktop scaling change publishes, and RandR is deliberately out of e12a —
  so `ShellEventKind::dpi_changed` is unreachable on X11 and a scaling change made while the editor
  is open does not re-inform `input_.set_dpi` / the browser / the compositor until restart. The Win32
  backend does emit it (`WM_DPICHANGED`). Post-e12a.
- **No `paint_requested` event on macOS.** Win32 emits it from `WM_PAINT` and X11 from `Expose`;
  `cocoa_window.mm` has no `drawRect:` analogue, so `ShellEventKind::paint_requested` is unreachable
  there. Harmless while the owner loop renders every iteration and `render_frame()` is damage-gated
  internally — it becomes real work only for the event-driven loop of §10, which is exactly when
  `request_redraw()` gains its first caller. Recorded here so that loop's author finds it.
- ~~**No macOS window backend.**~~ Landed by **e12b**: `cocoa_window.mm` (NSWindow + a
  `CAMetalLayer`-backed NSView), the pure `translate_ns_event` / `translate_ns_window_geometry`
  decoders, and the `CALayer.contents` CPU present blitter.
- ~~**No macOS CEF hosting.**~~ Landed by **e12c-1** (issue #436) and completed by **e12c-2**:
  `context_editor` and ALL TEN live smokes are real `.app` bundles on macOS, each with its five
  per-process-type helper bundles and its embedded framework, driven by
  `shell::cef::execute_helper_process()` (see § 3). `editor-cef-smoke (macos-latest)` BUILDS all eleven
  bundles — the ten smokes off ONE shared `--target` list, plus `context_editor` on its own step — and
  RUNS the ten, so the assembly, the load-bearing helper names, the framework embed and the
  no-`libcef_lib` link line have CI coverage on every scenario rather than on the two e12c-1 ported.
  (`context_editor` itself stays build-only on every leg: no ctest names it, and the family's
  `ctest -R "^editor-cef-smoke-"` step cannot match it — see § 9.)
- ~~**macOS shell smokes BUILD but do not RUN — `CefShutdown()` wedges (issue #437).**~~ **RESOLVED.**
  The two macOS ctests e12c-1 landed were registered `DISABLED` and printed as `Not Run (Disabled)`;
  they now RUN and PASS, as do the seven e12c-2 added. The e12c-1 diagnosis blamed
  `CefSettings.external_message_pump` because it was the one
  structural difference from the two macOS CEF apps that shut down cleanly in the same job — and that
  was WRONG: those two hung 5/5 as well once measured directly. The cause was Chromium's OSCrypt
  reading a MACHINE-GLOBAL `"<product> Safe Storage"` keychain item on a BLOCK_SHUTDOWN ThreadPool
  task, whose ACL macOS binds to the CREATING executable's cdhash — so a rebuilt binary raises a modal
  SecurityAgent prompt nothing can answer, and `CefShutdown()` waits on the ThreadPool shutdown event
  forever. Every CEF smoke now passes Chromium's `--use-mock-keychain`, enforced by the
  `editor-shell-cef-keychain` source gate. **Nothing about teardown was bounded or skipped**: the CE
  #319 `cef_shutdown_returned` assertion now RUNS on macOS, where the `DISABLED` property had made it
  unfalsifiable. Full mechanism, measurements, and the two remaining PRODUCT exposures (all CEF apps
  share one keychain item; an unsigned build prompts on every rebuild) are in
  `docs/cef-keychain-isolation.md`. **e12c-2 and e12c-3 are unblocked.**
- ~~**No live WINDOWED macOS proof.**~~ Landed by **e12c-3** (#442), the exact mirror of the
  e12a → e12a-x11-legs carve-out: the Cocoa arm of the smoke-tier injection seam
  (`smoke/src/smoke_inject_cocoa.mm`) plus the CEF-free `context_editor_shell_cocoa_smoke`, run
  directly with `--require-cocoa --require-display` in the `editor-cef-smoke` **macOS** leg. It opens
  a REAL `NSWindow` through the real `make_window_backend`, presents real frames through the real
  `CALayer.contents` blitter, renders the real e05d1 panel models, and round-trips a pointer + key
  through the REAL AppKit queue. So "a window APPEARS and a presented frame is VISIBLE" is now a
  checked claim on macOS, where before this only `editor-shell-test_window`'s selection assertion ever
  called `-[NSWindow initWithContentRect:]` at all (and it accepts either outcome, since a runner
  commonly has no GUI session).
  **Injection is `-[NSApplication postEvent:atStart:]`, not `CGEventPost`, and that choice is
  MEASURED rather than stylistic**: `CGEventPost`/`CGEventTapCreate` cross the HID boundary, so macOS
  gates them behind a TCC **Accessibility** grant a hosted runner cannot hold — which would have made
  a CGEventPost design worthless as a CI gate. `postEvent:` is in-process; with
  `CGPreflightPostEventAccess()` and `AXIsProcessTrusted()` both **false** on the reproduction host, a
  synthesized move + press + release + key still round-tripped 5/5 out of the backend's own
  `nextEventMatchingMask` pump. It is also the closer analogue of the X11 arm, which sends to the
  smoke's OWN window rather than driving the server globally.
  **Three Cocoa fidelity limits the X11 arm does not have** (all in `smoke_window.h`, all measured):
  the delivered `locationInWindow` is NOT the requested one — AppKit returns it scaled about the
  window's centre, ~1.4% on the reproduction host, so ~4.5 points at a 640-point window's edge — so
  the smoke asserts the Y-FLIP DIRECTION, the SEPARATION and a HALF-PLANE rather than an equality;
  the pressed-BUTTON mask cannot be injected at all, because the backend reads it from
  `+[NSEvent pressedMouseButtons]`, a live HID query (the modifier FLAGS do travel); and — the one
  that reddened `main` on both macOS jobs — **a posted location is resolved against the window's
  frame origin at DEQUEUE time, not at post time**, so a window AppKit moved after the post delivers
  displaced samples and the shipping decoder correctly reports what it was handed. The smoke answers
  that one in two places: it settles the window on a real predicate (the whole `placement()`
  unchanged across two consecutive pumps, never a sleep) before anything is posted, and it corrects
  its range claim by the frame-origin displacement through the pure, unit-tested
  `ns_delivered_shift_for_window_move` seam — a no-op whenever the window held still. That claim is
  also now TWO-sided (`0 <= y < height`): a one-sided `y >= 0` passed a sample delivered BELOW the
  window in silence. And because a delivered sample can no longer be identified by its ORDER in the
  stream (the desktop is entitled to deliver moves of its own), the four injected samples carry a
  modifier MARKER (Shift+Control+Option) and are selected by it, with unmarked samples counted and
  reported rather than silently dropped.
  **Still deliberately open:** the ten `editor-cef-smoke-shell*` smokes run HEADLESS on macOS — see
  the next bullet, which carries the mechanism and the tracking issue.
- **The ten live CEF smokes are still HEADLESS on macOS.** e12c-3 closed the DoD line above with a
  CEF-free smoke, which is what makes the windowed proof independent of the CEF keychain class
  (#437); the CEF legs themselves keep the offscreen backend on macOS exactly as Windows does. The
  work is the direct macOS twin of what e12a-x11-legs did for Linux: pass `--real-window` from
  `context_configure_shell_cef_smoke`'s `add_test` on macOS, set `WindowSpec::headless = false` on
  the four multi-window smokes' SECOND window, and forward the flag from the restore smoke to its two
  phase children. It is a distinct mechanism class (ten live CEF legs, each a real browser plus a
  real NSWindow) and is scheduled separately as **#443**.
- **macOS `WindowPlacement` is in Cocoa POINTS with a bottom-left screen origin**, where the Win32
  and X11 backends record physical pixels with a top-left one. Deliberate: the document is
  per-machine session state written and read by one backend, so points round-trip exactly through
  `apply_placement`, and a conversion would need a screen height that makes the stored value wrong
  the moment the display arrangement changes. Nothing compares a placement across platforms.
- **No native viewport consumer.** Region arbitration routes to the native path, but camera controls,
  picking and gizmo gestures over the bridge arrive with **e11**. Viewport layers are rect slots with
  no live content yet.
- **No keymap.** The focus-class rule is implemented and the resolver seam is in place; the command
  keymap itself lands with **e07**, so every unresolved key currently falls through to the browser.
- **No multi-window UI.** ✅ **HALF-LANDED with e10a**: the Shell can now CREATE and DESTROY a second
  native window on demand, each with its own fresh editor-core instance, its own bridge and its own
  `origin`, and `window.open` cannot produce an unmanaged one. What is still missing is everything
  that MOVES a panel — the tear-out gesture, rehome, cross-window drag and per-window layout
  persistence (e10b–e10d) — so nothing in the running app asks for a second window yet, and both
  windows currently publish their layout into the same editor-state slot.
- **No Windows accelerated OSR** — deferred by owner ruling pending gfx-rs/wgpu-native#621.
- ~~**No app scheme.**~~ ✅ **LANDED with e05c.** `context-editor` is registered in every process
  (`STANDARD|SECURE|CORS_ENABLED|FETCH_ENABLED`), `context-editor://app/…` serves editor-core's built
  asset set through a resource handler under a strict no-inline-script CSP, and `context_editor` now
  defaults to `context-editor://app/index.html` (`--app-root` overrides the asset root, `--url` the
  document). There is deliberately **no `file://` fallback** — the `webui-scheme-contract` gate
  asserts none reached the asset set. URL→asset resolution, the media-type allowlist and the CSP live
  in the CEF-free `app_scheme.h`/`.cpp` (ctest `editor-shell-test_app_scheme`, all three `build`
  legs); the CEF binding is a thin translator. The privileged native↔JS IPC bridge landed alongside
  it (`ipc_bridge.h`/`.cpp`, ctest `editor-shell-test_ipc_bridge`), and the live
  `editor-cef-smoke-shell` now boots the real bundle over the real scheme and round-trips a handshake
  through it. **Packaging** the asset root install-relative is still **e15**'s: the default is a
  build-tree path compiled in by CMake.
- **Package panels render, hold ONE authenticated port, and now carry real operator-granted
  capability** — of the original **e13b-2–f** list only the **demo external package** is still
  outstanding: theme-token delivery and the state-blob round trip landed with **e13d**, the
  `context new --template extension-panel` scaffold with **e13e**, and the capability/consent model
  with **e13c-4**.
  ✅ **The CAPABILITY/CONSENT MODEL landed with e13c-4**: a persisted per-package grant document
  (`<home>/.context/package-grants.json` — a SHELL document, deliberately NOT inside
  `~/.context/packages/`, which the boot scan enumerates and would refuse a loose file out of every
  boot) records the operator's answer in three separate fields (`requested` / `granted` / `decided`,
  so "granted nothing" and "never asked" stay different states). It is deny-by-default on every
  failure path, and a grant is CLAMPED to the package's own declared `capabilities` on both the load
  and the `decide` path — the contract registry's `manifest_defect` grant≤declaration check stays an
  INDEPENDENT second control rather than being folded into it. The store feeds
  `Contribution.sandbox.granted_scopes`, the `AttachOptions::scope` each package's baseline daemon
  session attaches with (`kPackageSessionScope` is now the DEFAULT, not the constant), and
  editor-core's capability gate at its ONE call site — `ShellPackageGrants` (`packagegrants.ts`)
  replaces `DENY_ALL_CAPABILITY_GRANTS` — so a package granted `ui_events` receives `editor.ui`
  facts over e13c-2's push path through `bridge.ui.subscribe`, while one without it is refused
  `bridge.capability_not_granted` from a real grant LOOKUP rather than a hardcoded deny.
  Subscribable topics stay CLOSED to the built-in set: a package's own declared topics are a PUBLISH
  surface, and subscribing to another package's would need a consent that third party never gave.
  The operator surface is the privileged router pair `package.grants.list` / `package.grants.decide`
  plus a boot printout of pending requests — NOT a package-callable verb:
  `panel_callable_daemon_methods()` is a closed seven-name EXACT-match allowlist (no prefix rule)
  over DAEMON methods, so a panel can neither name a Shell router method nor grant itself anything.
  ⚠ NOT claimed: an entry is keyed by package id ALONE — no version, no manifest hash — so a bundle
  reinstalled under a name that was already answered for inherits that answer up to its own
  declaration; binding the record to package IDENTITY is tracked as a follow-up, not closed here.
  ✅ **The PANEL PORT landed with e13b-1**: every `text/html` response the `context-ext://` scheme
  serves gets `<script src="/.context-panel-port.js">` spliced in ahead of any script the document
  carries (`ext_inject_port_bootstrap`), and the scheme serves that ONE synthetic asset out of itself
  (`ext_port_bootstrap_script`, `ExtResolution::synthetic`) — so the code that mints the
  `MessageChannel` and transfers a port UP to editor-core is the Shell's, never the package's. A
  document navigation to any other media type is refused (`ext_document_media_type_permitted`),
  which is what makes "the bootstrap ran first" true of EVERY panel rather than of the
  well-behaved ones. editor-core's half (`panelport.ts` `PanelPortBridge`) accepts only the FIRST
  conforming handshake per frame and revokes the port on a second `load` on the frame element; an
  `iframe` panel is docked with Dockview's `renderer: "always"` so a tab switch cannot detach it and
  forge that second load. As e13b-1 landed it the port carried **zero capability** — every verb
  answered `bridge.verb_not_granted` out of an EMPTY table — so what landed THERE was the
  authentication and the lifecycle, not a feature; e13c-4 is what fills that table (above). ⚠ The obligation as originally written ("key off the handshake's
  verified `event.origin`") is NOT implementable: every panel document reports the opaque origin
  `"null"`, and an iframe's `WindowProxy` is stable across same-slot navigations, so neither can name
  a DOCUMENT INSTANCE (`event.source` IS used, but only to tell one FRAME from another). ⚠ NOT
  claimed: that the host can prove WHICH PACKAGE the port-holding document is — that link is a
  Shell/editor-core agreement, not a browser-verified fact. Tiers: `webui-ts-unit`
  (`panelport.test.ts`, a real opaque-origin `srcdoc` child with real transfer semantics),
  `editor-shell-test_ext_scheme` (the bootstrap bytes, the splice point, the document-media-type
  gate — all three `build` legs), and the live `editor-cef-smoke-shell-iframe` (the port round-trips
  the deny-all refusal through the real CEF pump).
  ✅ **The IFRAME HOST landed with e13a-2**: editor-core's PanelHost renders a `content.type:
  "iframe"` manifest as an `<iframe sandbox="allow-scripts">` (never `allow-same-origin` — an opaque
  origin is what isolates one package from another and from the editor) pointed at a validated
  `context-ext://<package-id>/…` URL, with `allow=""` (delegate no powerful feature) and
  `referrerpolicy="no-referrer"`. It docks, floats and is disposed exactly like a built-in, through
  the same Dockview geometry calls. The entry grammar is `extpanel.ts` — a total, fail-closed parser
  that is the ONLY layer at which a `javascript:` / `data:` / `https:` entry can be refused at all,
  since the Shell's resolver never sees a URL the browser did not route to it — and its vocabulary is
  byte-compared against `ext_scheme.h` by the `webui-scheme-contract` ctest. The per-content-type
  gate lives in `PanelHost#mountable` and is *decided* in `open()` rather than in `start()`'s loop
  (which still calls the same predicate, but only to build its `unavailable` report), because
  `openById` (the e10b tear-out seed path) reached `addPanel` without it. `#renderer` is the second,
  independent guard at the construction chokepoint: it is fail-closed for **every** content type, so
  the one caller that does not pass `open` — `restoreLayout`, which hands a persisted arrangement
  straight to Dockview's `createComponent` — cannot default a drifted manifest into the `innerHTML`
  hydration sink either. Tiers: `webui-ts-unit`
  (`extpanel.test.ts` — the grammar, plus the RENDERED frame's own `DOMTokenList` against real
  Dockview) and the live `editor-cef-smoke-shell-iframe`.
  ⚠ **Two facts about a sandboxed panel that were MEASURED, not assumed** (ext_scheme.h carries the
  experiment). `'self'` in a CSP response header DOES match inside an opaque-origin document — CSP
  resolves it from the response URL, not from the document's origin — so the strict panel policy
  needs no relaxation. But an ES MODULE is fetched in CORS mode and a sandboxed frame's origin is the
  opaque `null`, so a module response with no `Access-Control-Allow-Origin` is fetched and then
  DISCARDED: the document loads, its stylesheet applies, a classic `<script>` even runs, and only the
  module graph dies, naming no directive. `ext_response_headers` therefore emits
  `Access-Control-Allow-Origin: null` on SCRIPT media types **only** — the narrowest form that works,
  so every non-script asset stays unreadable cross-origin under the same-origin policy rather than
  under the CSP alone (every panel frame shares the origin `null`).
  ✅ **Their SECURITY FOUNDATION landed with e13a-1**:
  `context-ext` is registered in every process pinned to `STANDARD|SECURE|CORS_ENABLED` (no
  `CSP_BYPASSING`, no `LOCAL`, and — unlike the app scheme — no `FETCH_ENABLED`), its resource
  handler is installed **unconditionally**, and `context-ext://<package-id>/…` resolves through the
  CEF-free `ext_scheme.h`/`.cpp` (ctest `editor-shell-test_ext_scheme`, all three `build` legs).
  That resolver is deny-by-default four times over — a valid package id, a package that is actually
  MOUNTED (never path-joined from the id), a media type on the shared asset allowlist, and canonical
  containment inside that ONE package's root — and `mount()` additionally refuses overlapping roots
  (a package nested inside another's root would be a cross-package read that per-root containment
  cannot see) and a root it cannot canonicalize (the overlap check is lexical, so it establishes
  disjointness only between canonical paths). With no package mounted (today's state — the install
  flow is e13b's) every `context-ext://` request is refused, which is the intended configuration and
  not a gap. Both schemes walk ONE containment chain — declared in `app_scheme.h`, which documents
  the primitives and why they are shared — so a traversal rejection cannot rot in one copy and ship
  the hole in the other; a colon is refused ANYWHERE in a path segment, covering both the
  drive-relative re-root and NTFS alternate data streams. Refusal STATUSES are chosen so the mount
  table is unobservable: an unmounted package and an absent asset both answer 404, so a response
  cannot be used to enumerate which packages a user has installed.
  editor-core's own CSP widened `frame-src 'none'` → `frame-src context-ext:` for this, and nothing
  else in that policy moved. The CEF-free `kExtSchemeOptions` pin is `static_assert`ed against CEF's
  own `CEF_SCHEME_OPTION_*` values in `cef_shell.cpp`, so a CEF bump that renumbered them fails the
  build rather than silently changing the scheme's semantics. e13a-2 tightened the panel response's
  `frame-ancestors` from the scheme-source `context-editor:` (which also authorized
  `context-editor://ipc`) to the host-source `context-editor://app`, verified by the live smoke's
  own assertion that the panel's SUBRESOURCES were fetched — a frame the directive blocked never
  parses its document, so it never asks for them.
- **No triple-click.** Double-click works (the window class sets `CS_DBLCLKS` and the decoder reports
  `click_count = 2`), but nothing tracks a click RUN, so `click_count` never reaches 3 and
  triple-click-to-select-line is inert in the browser.
- **No real-adapter proof of the scissor.** `IRenderPassEncoder::set_scissor_rect` is exercised only
  through the fake RHI, which records the rect rather than applying it; `render-wgpu-osr-composite`
  is e03's full-window composite gate and never scissors. The PET_POPUP confinement is asserted as
  "the compositor passed this rect", not per-pixel against a GPU.
- **The interactive windowed pass is manual** — § 10.
- **The window chrome (editor-window-chrome, § 15) — what it deliberately does NOT do.** No CSD on
  Linux (D6, by design, not a gap). No live FPS in the play bar and a static, disabled Target chip —
  the sources arrive with e11 / the build pipeline. `compiling` / `error` play states are unreachable
  until those facts are published. The strip inherits CE #356 (play-state staleness after a daemon
  restart) — fixed upstream, not here. `window.quit` from a SECONDARY window closes only that window
  (the Window menu's Quit dispatches `window.close` on its OWN window; the Shell-side quit that
  the primary's ✕ now reaches — `WindowManager::request_quit` — has no bridge verb of its own,
  so a secondary cannot ask for it); the web menubar's dropdown does not close
  on focus-out and has no Alt-mnemonics; the About dialog is `aria-modal` without a focus trap (all
  three recorded by d3). No keymap wiring beyond what the menu needs (the e07c resolver seam is
  untouched); the accelerator column DISPLAYS `DEFAULT_KEYBINDINGS` strings, and only the macOS
  `NSMenu` key equivalents are live wiring. The ten live CEF smokes still run HEADLESS on macOS (CE
  #443), so `mode:"hybrid"` is provable only in the CEF-free `editor-shell-cocoa-window`. The real
  Windows NC message stream is CI-unreachable (Session 0) — the pure halves are pinned everywhere, the
  interactive rows live in § 10. And no visual-regression pins the finished chrome yet: that is
  **e16**'s, and this set hands it a STABLE surface (§ 15's DOM ids, attributes and region ids).

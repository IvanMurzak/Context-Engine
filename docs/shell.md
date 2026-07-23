# The Shell — windows, the owner loop, the compositor, input

How the engine draws an interactive window. Landed by M9 e04 (`src/editor/shell/` → `context_editor`).
This records how the repository implements the design; the normative records live in the owner's
design authority (design 03; `R-HEAD-004`, `R-UI-007`, `R-HUX-011`, `L-41`, review `B-F11`/`C-F2`).

It builds directly on e03's present path (`docs/present-path.md`), which landed the seam a window
drives and explicitly deferred the window manager and the `PET_POPUP` layer to here.

The load-bearing property, stated first because the module is arranged around it:

> **Almost none of the Shell is platform code.** The OS window is one file, the browser is one file,
> and everything between them — the layer stack, damage, the resize protocol, input arbitration, DPI,
> the owner loop — is portable C++ compiled and tested on all three OS legs. That is deliberate: the
> window backend is Windows-only in v1 and the browser is a CI-only dependency path, so logic written
> inside either would be exercised by almost nothing.

## Layout

| Where | What |
|---|---|
| `context_editor_shell` (`src/editor/shell/`) | The Shell proper: window seam + Win32 backend, DPI, input arbitration, compositor, editor-state persistence, the owner loop. Default-built, CEF-free, GPU-backend-free, fully unit-tested locally and on all three `build` legs. |
| `context_editor` (`app/editor_main.cpp`) | The app. Default-built everywhere; links the browser binding where one can be hosted. |
| `context_editor_cef` (`cef/`) | The windowed-OSR CEF binding — the ONE piece that cannot build locally. Behind `CONTEXT_BUILD_GUI_CEF`. |

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

**Per-OS backends.** v1 ships Windows (`RegisterClassExW`/`CreateWindowExW` + WndProc, per-monitor-v2
DPI). macOS (NSWindow/NSView) and Linux (X11/XWayland, D21) are **e12's**, and that gap is REPORTED,
not silent: `make_window_backend` returns a selection carrying a diagnostic naming e12, mirroring how
e03's `make_present_blitter` reports its own missing platforms. A shell that quietly opened no window
would look identical to one that opened an invisible one.

**The platform blind spot, and what is done about it.** The local dev gate defines `_WIN32`, so a
POSIX branch gets no compile signal at all. The Win32 backend is therefore split: the MESSAGE
DECODING is a pure function over plain integers (`translate_win32_message`) that includes no
`<windows.h>`, names no `HWND`, and is executed by the ctest on every OS — that is where the
bit-twiddling that actually goes wrong lives. Only the OS calls remain in `win32_window.cpp`,
Windows-only and honestly untested off-Windows, exactly as e03 left the GDI blit body.

The `WM_*` constants the decoder uses are declared locally and `static_assert`ed against the real ones
inside `win32_window.cpp`, so a wrong constant is a Windows COMPILE error rather than a message that
silently decodes as something else at runtime on the one platform that runs it.

Three decoding traps, each pinned by a test:

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
and each panel's D6 state blob are persisted, debounced and crash-safe, to `.editor/editor-state.json`.
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

The native path's consumer — camera controls, picking, gizmo gestures driving the existing
`viewport_edit_model` verbs over the bridge — arrives with **e11**. Until then the arbitration is real
and every sample is accounted for; there is simply no native consumer yet.

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

`OnPaint` delivers straight into the compositor with **no copy**: it runs inside
`CefDoMessageLoopWork()`, which runs inside `pump()`, so the sink is live and CEF's buffer is valid
for exactly that window.

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
  Scene tree and Inspector are still boundary-blocked, and why `panel.unknown` and `panel.not_hosted`
  are different refusals.
- **Hostable today**: `placeholder` (from `context_gui_uitree`) and `builtin.problems`. Two rather than
  one deliberately — a single panel would leave panel-agnosticism resting on a claim.
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
| `editor-shell-test_dpi` | Scale derivation, the OS-nonsense clamp, round-to-nearest, the never-collapse rule (and that empty stays empty), signed point conversion across zero |
| `editor-shell-test_input` | Back-to-front hit-testing, edges and NEGATIVE coordinates, wholesale publish, viewport-vs-browser arbitration, DIP dispatch positions, the implicit drag capture (incl. a second button mid-drag), modal swallow vs overlay fall-through, focus-class key routing, the R-HUX-011 stamp |
| `editor-shell-test_editor_state` | Round-trip (incl. a negative x and a maximized window's restore rect), the debounce, no-op on identical, `flush_now`, the atomic replace leaving no temp, the degrade on a malformed/negative-extent document, a failed write staying dirty to retry |
| `editor-shell-test_window` | The pure Win32 decoder — signed LPARAM halves, the minimize carve-out, button mapping, MK_*/modifier split, the signed wheel delta and its deliberate absence of a position, key/char/sys-key, `WM_DPICHANGED`'s low word — plus the headless backend and the never-silent platform selection |
| `editor-shell-test_compositor` | The extrapolated layer UV (incl. the full-window identity vs e03), the premultiplied blend + clipping, damage-driven skip, LAYER ORDER and the popup's scissor rect, a hidden popup dropping its layer, the resize protocol, Outdated/Lost keeping the damage, Suboptimal presenting first, a refused surface, both present paths, a malformed producer frame |
| `editor-shell-test_shell` | The attach guard, the owner loop end to end (DIP browser sizing, input round-trip, viewport vs browser, focus dropping a live drag, idle skip, popup), placement persistence + restore, window drop, shutdown flush |
| `editor-shell-smoke-session0` | **The blocking CI requirement**: the whole shell loop over software-OSR frames with the composited present asserted PER-PIXEL — see § 9 |
| `editor-shell-boundary` | The D10 link-closure audit actually ran and covered a real forbidden target |
| `editor-shell-test_panel_host` | The panel-agnostic surface over SYNTHETIC panels: roster projection (hosted vs listed-but-unhosted), render payload, command dispatch + the stale-command refusal, the four gesture verbs and the refusal of a fifth, the D6 round-trip and all three degrade paths, every `panel.*` binding, and hostile params on every method |
| `editor-shell-test_editor_state_bridge` | e05d2: the layout/panels publish→store→get round-trip (incl. a restart), all three persistence triggers (debounce, `flush_now`, crash-restore), region parsing (kind tokens, negative-pixel clamp, malformed-element skip) reaching a live `InputArbiter` and routing a pointer, the empty-publish clear, the `not_ready`/`bad_params` degrade paths, and the full `editor.*` JSON-RPC binding over a real router |
| `editor-shell-test_problems_feed` | The LIVE `diagnostics` projection without a daemon: severity/stability tokens, all three snapshot shapes, every publisher shape the topic carries, hostile/degenerate payloads, R-BRIDGE-008 promotion + settle, and the node-id -> diagnostic-identity mapping |
| `editor-shell-test_builtin_panels` | The composition root: what binds, that Scene tree + Inspector stay listed-but-unhosted until e05d3, and a daemon event reaching a rendered panel end to end |
| `editor-shell-test_user_config` | e06d: the per-user config store - the total reader (absent / malformed / non-object / oversized), the merge-preserving read-modify-write (a member from a FUTURE build survives), the recents-and-theme co-existence regression, the CLOSED settable vocabulary (`config.unknown_key` / `config.bad_value` / `config.write_failed`), unique staging names, the generation watch (identical rewrite and cosmetic reformat are NOT changes), and the full `config.*` binding over a real router |
| `editor-shell-config-writers` | e06d: the C-F14 SINGLE-WRITER source gate - exactly one TU writes `~/.context/config.json`, editor-core carries no client-side persistence API, and one module names `config.set` (`tools/check_config_writers.py`) |
| `editor-cef-smoke-shell` | The LIVE CEF half: a real windowless browser through the real integrated pump, its `OnPaint` frames composited + presented, input round-tripped, a live resize repainted (`editor-cef-smoke` job, Windows/Linux) |
| `editor-shell-test_window_registry` | e10a: the registry — window 0 primary, ids minted in order and NEVER reused, all four create-failure classes reported once with the source window (and the registry still usable after four in a row), the live-window cap, per-window `origin` reporting, and the CE #319 lifetime rule in both directions: a destroyed window's browser dies NOW while its session is retired until the manager does, across 25 create/destroy cycles and across `shutdown()` with windows still open |
| `editor-cef-smoke-shell-multiwindow` | e10a, the LIVE half a fake cannot reach: a SECOND real CEF browser booting its OWN editor-core instance (two DIFFERENT round-tripped handshake nonces), a REAL renderer `window.open` refused by `OnBeforePopup` with NO browser created, and a MID-PROCESS destroy followed by another create (`editor-cef-smoke` job, Windows/Linux) |

All `editor-shell-*` tests are a plain (non-gate) family: the `build` job's general ctest step runs
them on all three OS legs and `--preset dev` builds them, so **no `ci.yml` `--target` bookkeeping**.
That covers `context_editor_panels`' tests too — they register in the same family, and the library
itself is built transitively by the jobs that build `context_editor` / the CEF smoke, so e05d1 needed
**no `ci.yml` change at all**.
`editor-cef-smoke-shell` is the exception and IS on the `editor-cef-smoke` job's hand-maintained
`--target` list — the "Not Run = RED" tripwire. So are its siblings
`editor-cef-smoke-shell-restore` (e05d4), `-palette` (e07d), `-settings` (e06d) and
`-multiwindow` (e10a). `editor-shell-test_window_registry` is NOT: it is a plain
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
on this platform yet" until e12 brings their shells up — the same gap `native_pick_folder()` and the
window backend leave — and the banner then says "not checked" rather than implying the build is
current.

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

Automating this is **e12's** (with the mac/Linux backends) plus the interactive-runner provisioning
the design's gate table tracks.

## 11. What this does NOT yet do

Named so the gaps are visible rather than assumed:

- **No macOS or Linux window backend** — e12. `make_window_backend` reports the gap; the app degrades
  to the honest offscreen backend.
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
- **No triple-click.** Double-click works (the window class sets `CS_DBLCLKS` and the decoder reports
  `click_count = 2`), but nothing tracks a click RUN, so `click_count` never reaches 3 and
  triple-click-to-select-line is inert in the browser.
- **No real-adapter proof of the scissor.** `IRenderPassEncoder::set_scissor_rect` is exercised only
  through the fake RHI, which records the rect rather than applying it; `render-wgpu-osr-composite`
  is e03's full-window composite gate and never scissors. The PET_POPUP confinement is asserted as
  "the compositor passed this rect", not per-pixel against a GPU.
- **The interactive windowed pass is manual** — § 10.

# 01 — Current architecture (ground truth)

> Gathered 2026-07-18 by four parallel explorations of `IvanMurzak/Context-Engine` @ `4b7456f`
> (local checkout `engines/context/Context-Engine`). Every claim below carries file:line refs
> obtained this session. Where M9 relies on a fact, it is cited from here.

> ### ⚠️ AMENDED BY OWNER RULING — 2026-07-19 (scope: the OSR-import seam only)
>
> **DO NOT FORK, PATCH, OR RE-PIN `wgpu-native`.** The owner **rejected** the patched-fork
> approach (carrying a fork = unbounded long-term maintenance cost); **`s2` is SUPERSEDED**.
> Two forward-looking passages below are affected — the §4 spike bullet and the §6 seam-index
> "OSR-texture import" row — each carries an inline note. The *descriptive* ground truth is
> unchanged and still exact: stock wgpu-native v29's C API genuinely exposes no
> external-memory import, and the measured accel ≈27 µs / software ≈114 µs numbers stand.
> What changed is the **decision** taken on top of those facts:
>
> - **Windows** — accelerated DXGI shared-handle import is **deferred**; the **CPU-upload** path
>   (`OnPaint` BGRA → `wgpuQueueWriteTexture`, dirty rects) ships, accepted **for the Editor on
>   Windows ONLY**. Accel↔software flag/seam retained, Windows branch disabled pending
>   <https://github.com/gfx-rs/wgpu-native/issues/621>.
> - **macOS** — unaffected, **stays accelerated** via **STOCK** native accessors
>   (`wgpuTextureGetNativeMetalTexture`, upstream wgpu-native #557) → IOSurface → Metal blit.
> - **Linux** — unchanged: software upload (dmabuf only behind the accel gate, ships OFF).
>
> Authoritative record: [`ROADMAP.md`](ROADMAP.md) — Backlog section + the 2026-07-19
> progress-log entries. Full per-OS restatement: [03 §3](03-shell-window-compositor-input.md).

## 1. Editor GUI layer as-built (`src/editor/gui/`)

Three headless, default-built, CEF-free libraries + panels + one CI-only CEF host
(`src/editor/gui/CMakeLists.txt:14-68`, split rationale `:6-12`; overview `src/editor/gui/README.md:8-35`):

| Target | Role | Key files |
|---|---|---|
| `context_gui_uitree` | Headless UI-logic tree: `UiNode` (ARIA role, label, focusable, bound command — `uitree/include/.../node.h:51-100`, role vocab `:23-37`), `Panel` (`panel.h:29-60`), deterministic `render_html` (`node.h:106`), `audit_a11y` (`panel.h:64-85`), `focus_order` (`panel.h:73`) | `uitree/` |
| `context_gui_contract` | R-EDIT-001 as-built: `Contribution{id, kind, target, title, contract_version(kContractMajor=1), SandboxPolicy}` (`contract/include/.../extension.h:20-44`), deny-by-default `ExtensionRegistry::register_contribution` (`registry.h:22-25,46`), capability shim `ExtensionBridge` (`shim.h`) | `contract/` |
| `context_gui_compositor` | L-41 **mode selector only** — `SurfaceCapabilities`/`CompositingMode`/`select_mode()`/`make_handoff()` (`compositor/include/.../surface.h:26-65`); `external_begin_frame` hardwired false (`src/surface.cpp:39`, cef#4033); the only compile-time platform `#if` in the seam (`surface.h:16-18`, `surface.cpp:43-52`) | `compositor/` |

**Panel roster** (each = headless model + `build_panel()` + `kContributionId`; hand-listed in
`a11y/src/registry.cpp:21-113` + `a11y/coverage.manifest.jsonl:8-16` — there is NO auto-registration
authority and NO PanelBase/manifest file). ⚠ Exception: `builtin.session.undo` appears in
**neither** anchor (a11y-covered via its own headless command-surface ctests) — the M9 roster
promotion must ADD it, not just migrate the hand-list (review A-F2):

| Panel | id | Files |
|---|---|---|
| Scene tree | `builtin.scene-tree` | `panels/scenetree/.../scene_tree_panel.h:40` (select/listeners `:62-68`, selection key = L-35 id-path `:26-30`) |
| Inspector | `builtin.inspector` | `panels/inspector/.../inspector_panel.h:102`; **the ONE L-30 write engine** `commit_override_write` `:187-193` (CAS + rebase-or-drop), gesture `stage_edit/commit` `:125-136`, `OverrideWriteGateway` seam `:57-73` |
| Problems | `builtin.problems` | `panels/problems/.../problems_panel.h:42` |
| Viewport (observer) | `builtin.viewport` | `viewport/.../viewport_panel.h:44`; `ViewportPresent`/`compute_present` = outcome descriptor, **no pixels** (`viewport_model.h:51-73`); latency-timestamp hook documented-unbuilt (`viewport_panel.h:25-28`) |
| Viewport override-edit | `builtin.viewport-edit` | `viewport/.../viewport_edit_panel.h:25`; gizmo gestures `viewport_edit_model.h:117-144`; session state in-memory `:9-11` |
| Play bar | `builtin.playbar` | `playbar/.../playbar_model.h:83` — play/pause/stop/step via local `SessionControl*` seam `:102-117`, local command ids `:51-54`; **not** RPC, **not** on the `session` topic |
| Tilemap paint | `builtin.tilemap-painter` | `panels/tilemap/.../tilemap_paint_panel.h:39`; pixel→cell gesture verbs `tilemap_paint_model.h:140-172` |
| Contextual help | `builtin.help` | `help/.../help_panel.h:19`; generated from live registry (`help_model.h` — `all_verb_help`, `panel_topics`) |
| Session undo | `builtin.session.undo` | `session/undo/.../undo_journal.h`; journal replays through `commit_override_write` (`:171-186` in inspector_panel.h); `.editor/session.json` (de)serialization `to_json/load_json` `:129-136` — **host never actually reads/writes the file yet** |

**`editor_host`** (`src/editor/gui/host/src/editor_host.cpp`, target `host/CMakeLists.txt:45`,
ctest `editor-cef-smoke-boot` `:56-59`): boots CEF **windowless** — `no_sandbox=true`,
`windowless_rendering_enabled=true`, single-threaded pump (`:323-332`), `SetAsWindowless(0)`
(`:340-341`), `windowless_frame_rate=1` (`:342-343`), loads `file://` temp HTML rendered from
uitree strings (`:239-243,346-347`), forces `disable-gpu` flags (`:127-134`).
**`OnPaint` is an empty no-op** (`:97-100`) — CEF output goes nowhere.

## 2. What does NOT exist (verified absences)

- **No native OS window in production.** Zero `CreateWindow*/NSWindow/GLFW/SDL/X11/Wayland`
  windowing in `src/` (only spikes; miniaudio's X11 hits are audio). CEF is windowless-only.
- **No input routing.** Zero CEF `SendMouse*/SendKey*` in `src/`; interaction = direct C++ calls
  on panel models from tests. No keyboard dispatcher, no pointer hit-test in the editor layer.
- **No docking / multi-window / layout model.** Greenfield.
- **No web assets.** Zero `.html/.ts/.js/.css` under `src/editor/gui/`; no npm/package.json/
  bundler anywhere in the repo (TS tier uses SHA-pinned esbuild/tsgo subprocesses —
  `tools/ts-toolchain.json`, `tools/fetch_esbuild.py`).
- **No implemented present path.** `ISurface`/`ISwapchain` are empty stubs
  (`src/render/include/context/render/rhi.h:374-384`, comment `:367-373`); `BGRA8Unorm` reserved
  (`rhi.h:40`, comment `:35`); no `wgpuSurface*` calls in `src/`.
- **No external-texture import in the RHI.** `IDevice` (`rhi.h:322-338`) has no import method;
  `wgpu_rhi.cpp` never touches shared handles. **Net-new seam.**
- **No render-side camera / multi-viewport abstraction.** `render_world.h:24-137` has no Camera
  component; 3D scenes hardcode `view_proj`; only 2D `sprite::Camera2D`
  (`src/render/sprite/.../ortho.h:48`). One observer viewport, composited never.
- **No client SDK.** No CMake `install()`/export anywhere in `src/editor`, `src/cli`,
  `src/kernel`, `cmake` (the root `src/CMakeLists.txt:284-288` installs only the
  `context-hello` demo + `context_kernel` for the R-VER-004 layout); out-of-tree consumers
  would copy CLI wire plumbing.
- **No client-side subscription helper.** `wire_client.h` is one-shot request/response only.
- **No command palette / no command↔verb unification.** Panel command ids are ad-hoc constants.
- **No installers / releases.** CPack ZIP only (`src/CMakeLists.txt:290-298`);
  `release-sign.yml` is a manual dry-run skeleton (`:18-21`); no GitHub Releases.
- **CEF sandbox OFF.** `USE_SANDBOX OFF` (`cmake/ContextCef.cmake:91`) with the bootstrap.exe
  model explicitly deferred (comment `:75-78`).

## 3. Client contract as-built (`src/editor/bridge/`, `src/editor/contract/`)

- **Transport**: JSON-RPC 2.0 over Unix socket / Windows named pipe, 4-byte BE length frames,
  64 MiB cap (`bridge/include/.../transport.h:33-39,43-150`). **Serial single-connection (M1
  model)** — one client served to disconnect (`transport.h:14-17`, `kernel_server.h:37`);
  multi-client fan-in is a documented follow-up. → D19.
- **Daemon**: `.editor/lock` try-lock = attach signal (`daemon.h:28-47`); scope clamp at attach
  (`daemon.h:68-69`). Discovery `.editor/instance.json` `{endpoint, pid, protocolMajor, token}`
  (`src/cli/src/daemon_command.cpp:95-122`), 0600 POSIX. **Token carried but NEVER verified**
  (`daemon_command.cpp:91-94`; handshake struct has no token field — `handshake.h:40-44`). → D20.
- **protocolMajor = 1, frozen** (`contract/include/.../handshake.h:27`); hard-fail negotiate
  (`:59`); min-minors deprecation = 2 (`:34`).
- **Dispatcher**: one registry, scope check FIRST on every method (`dispatcher.h:45-51,88`;
  `dispatcher.cpp:203`), `MethodBackend` seam for new operational verbs; `KernelServer` serves
  `edit, edit-batch, query, snapshot, reconcile, resource.read, shutdown`
  (`kernel_server.h:11-30,54,70-82`).
- **Event stream** (`bridge/include/.../event_stream.h`): topics `files/derivation/diagnostics/
  session/clients/log` (registered with payload schemas — `contract/src/registry.cpp:956-1010`);
  envelope `{seq, incarnationId, generation, topic, payload}` (`:44-54`); snapshot-then-delta
  `subscribe(topics, path_scope, since_seq, capacity)` → `{sub_id, snapshot, catchup[], gapped}`
  (`:135-197`); ack-based retention vs slowest cursor (`:170-172`); bounded queues + gap markers
  (`:59-94`). **`session` topic carries lifecycle only** `{event: started|reloaded|stopped}`
  (`registry.cpp:986-991`) — no selection/camera/playstate. → D7.
- **Registry**: single source of truth, CLI ≡ RPC ≡ MCP ≡ describe, parity CI-locked
  (`contract/registry.h:1-9,55-93,154-158`; `tests/test_registry_parity.cpp`). 41 verb specs, 4 reserved
  `implemented=false` → 37 implemented (`registry.cpp:220-949`); `subscribe/ack/unsubscribe` are operational, daemon-served
  (`:825-949`). MCP = pure projection `mcp_surface()`, no separate server.
- **Scopes**: `read_query/file_write/session_control/build_install`
  (`bridge/include/.../scope.h:19-27`), method→scope table (`:62-76`), enforcement in dispatcher,
  never in adapters; GUI panels clamped via `ExtensionBridge` shim ("a panel never holds the
  socket/token" — `gui/contract/shim.h:1-7`).
- **`session *` CLI verbs are a headless harness over a state FILE** (`registry.cpp:401-477`) —
  not live editor play control. GUI play control is in-process (`playbar_model.h`). → 05.
- **GUI writes bypass RPC today**: `OverrideWriteGateway` is implemented in-process over
  `compose::plan_write` + filesync; wiring it over the live daemon `edit` RPC is explicitly
  called out as an unfinished trailing-M5 surface (`inspector_panel.h:10-13`). → D22.

## 4. Render / present / input seams (`src/render/`, `src/packages/`)

- **RHI**: `create_wgpu_rhi()` → `wgpuCreateInstance` (`src/render/src/wgpu/wgpu_rhi.cpp:850-858`);
  the ONLY TU including `webgpu.h` (`:21`) — all surface code lands here. Offscreen-only by
  charter (confinement `wgpu_rhi.h:3-5`; surface-deferral charter `rhi.h:367-373`). Pin `v29.0.1.1`
  (`src/render/CMakeLists.txt`, `docs/native-webgpu-backend-decision.md:13,128`).
- **Offscreen render**: `render_offscreen_triangle_pixels` (RGBA8, copy→map-read;
  `offscreen_scene.h:75-135`); modes probe/render/sprite/lit/viewport/ui/worldpanel/curvedpanel/
  golden/bench (`offscreen_main.cpp:109-308`); viewport composite scene (3D+2D layers,
  `viewport_scene.h:54-158`). Persistent RTT: `DynamicTextureRegistry`
  (`render/ui/.../dynamic_texture.h:30-63`), `GpuUiProvider` (`provider.h:33-96`).
- **Extract**: `extract_render_world(World, tick, RenderSnapshot&)` (`extract.h:28`), double
  buffer `RenderDoubleBuffer` (`render_world.h:142-186`).
- **The two throwaway-but-measured reference implementations** (spikes are throwaway per repo
  charter; their DECISIONS are ratified in `compositor/surface.h`):
  - `spikes/webgpu/src/main.cpp` `runWindowed()` L506-604 — Win32 window + the full wgpu
    windowed present chain: `WGPUSurfaceSourceWindowsHWND`→`wgpuInstanceCreateSurface` (L524-530),
    `GetCapabilities`→`SurfaceConfigure` (L546-560, BGRA8/Fifo), `GetCurrentTexture`→submit→
    `SurfacePresent` (L575-590). PASS on-screen (FINDINGS proof matrix).
  - `spikes/cef-compositing/` — Windows accelerated-OSR composite, measured green (CEF 149,
    4/4 autotests): DXGI NT shared handle `OpenSharedResource1`+`CopyResource`, cached per
    handle (~11-deep pool) (`renderer_d3d11.cpp:294-331`, cache `renderer_d3d11.hpp:100`);
    premultiplied-alpha fullscreen-triangle composite (`renderAndPresent` L210-272, blend
    L152-161, UV=`visible_rect/coded_size` L245-250); software-OSR fallback ~40 lines apart
    (`updateUiFromBuffer` L333-362); input round-trip `wndProc`→`SendMouse*/SendKey*` L262-343;
    resize protocol L266-278. FINDINGS: engine FPS decoupled from CEF 60 Hz; accel ≈27 µs vs
    software ≈114 µs per-paint CPU; delivery = D3D11 shared handle (Win) / raw IOSurface (mac) /
    dmabuf (Linux, research-grade); caveats: single-thread pump, PET_POPUP not composited,
    DPI 1.0, sandbox off (FINDINGS.md L9-167).
    ⚠ **2026-07-19 — these measurements stand, but the Windows DECISION taken on them does not.**
    The spike's D3D11 shared-handle route needed a patched wgpu-native fork, which the owner
    **rejected**. Windows therefore ships the ≈114 µs software path knowingly (Editor-on-Windows
    only — NOT sanctioned for frame-rate-sensitive surfaces); the raw-IOSurface (mac) delivery
    survives intact on stock accessors. Do not read "accel ≈27 µs" as a mandate to build it.
- **Input packages** (reusable shape): `InputRouter` — pure `(context stack, raw events)→
  TickInputs`, install/push/pop/rebind (`packages/input/.../input_router.h:48-92`); UI capture
  stack `UiInputRouter` — focus/blur, `route_pointer` hit-test target-then-bubble, modal swallow
  vs overlay fall-through, single sink (`packages/ui/.../input_routing.h:62-112`,
  `src/input_routing.cpp:53`); `hit_test` back-to-front (`src/layout.cpp:139-176`). **No OS
  event pump feeds any of it.**
- **Kernel platform seam**: `Platform` = Clock+FileSystem+TaskRunner only, "touches no GPU or
  display" (`kernel/.../platform.h:4-5,55-60`) — no window/display seam exists.
- **Windows CI cannot run native-GPU windowed legs** (Session-0 teardown crashes —
  `offscreen_main.cpp:75-84`, `editor_host.cpp:376-385`); windowed Windows validation is
  off-CI / interactive-session only. → 09.

## 5. Build / packaging / CI as-built

- **CEF supply chain**: `tools/cef-prebuilt.json` — CEF **149.0.6+g0d0eeb6+chromium-149.0.7827.201**,
  minimal dist, SHA-256 per triple (win64 162 MB / linux64 311 MB / macarm64 123 MB download);
  fetched+verified by `tools/fetch_cef.py` via `context_acquire_cef()`
  (`cmake/ContextCef.cmake:29-115`); third-party publisher-authenticated carve-out (like
  V8/wgpu-native). Per-OS launch: self-re-exec exe (Win/Linux, `host/CMakeLists.txt:39-59`) /
  `.app`+helpers+framework (mac, `:61-113`). Gated behind `CONTEXT_BUILD_GUI_CEF` (default OFF,
  `src/CMakeLists.txt:31`).
- **Signing (reusable)**: pure PE/Mach-O signature report (`src/editor/build/signing.h:42-143`,
  never-silent `unsigned` state); Ed25519 trust root (`tools/verify_artifact.py`,
  `tools/trust-root/allowed_signers`, `src/common/verify_signature.*`); `release-sign.yml`:
  Azure Trusted Signing (Win, `:119-202`), codesign+notarytool JSON `status==Accepted` (mac,
  `:226-365`), custody model B, environment-protected. Stapling attaches only at `.dmg/.pkg/.app`
  container stage (`docs/export-adapters.md:197-203`) — no container exists yet.
- **Build pipeline**: pure `context_build` core (verify→toolchain→aot→transcode→pack→link→adapter,
  `src/editor/build/README.md:10-22`); export adapters linux/windows/macos/web
  (`docs/export-adapters.md:15-23`); artifacts = ustar tarballs, built + clean-host-smoked +
  discarded in CI (`ci.yml:867-1080`).
- **CI**: `ci.yml` 3-OS matrix; Windows legs on self-hosted MSVC runner (labels
  `["self-hosted","Windows","X64","context-engine"]`, `ci.yml:113-128`); sanitize = Linux
  ASan+UBSan / TSan (`ci.yml:394-437`); named exit-gate steps with exclusion pattern
  (`ci.yml:151,159-304`); fleet manifest `docs/ci-fleet-manifest.json` validated every run by
  `tools/check_fleet_manifest.py` (`ci.yml:100-104`); **`editor-cef-smoke` job**
  (`ci.yml:1702-1798`): CEF cache keyed on pin hash (`:1734-1738`), Linux xvfb install
  (`:1742-1748`) + boot under `xvfb-run` (`:1785-1788`),
  Linux-blocking a11y gate via `tools/a11y_scan.py` (`:1769-1777`). **No fleet-manifest row for
  editor-cef-smoke** (folded into m5-exit rows) — a new M9 gate needs its own row.
- **vcpkg**: default deps EMPTY (`src/vcpkg.json`); heavy prebuilts ride SHA-pinned
  `tools/fetch_*.py` + `tools/*-prebuilt.json` + `cmake/ContextDownload.cmake` — the precedent
  for any new pinned tool (incl. Node/bundler for editor-core).
- **Version SoT**: `project(ContextEngine VERSION 0.0.1)` (`src/CMakeLists.txt:15-16`) →
  `versions/<semver>/` side-by-side layout (R-VER-004, `:253-288`; `docs/versioned-install.md`).
- **Build-time budgets (a12)**: `bench/build-time-budget.json` (warm 300 s / cold 2400 s);
  budget bench builds CLI+runtime-server targets only (`ci.yml:651-712`) — the editor app enters
  it only if added to the measured target list.

## 6. Seam index (what M9 touches)

| M9 concern | Status | Seam (file:line) | Reference impl |
|---|---|---|---|
| Native window(s) | ABSENT | new Shell module; kernel `Platform` untouched (`platform.h:55-60`) | `spikes/webgpu/main.cpp:506-522`; `spikes/cef-compositing/main.cpp:582-599` |
| Swapchain present | STUB | implement `ISurface/ISwapchain` (`rhi.h:374-384`) in `wgpu_rhi.cpp` (sole `webgpu.h` TU, `:21`) | `spikes/webgpu/main.cpp:524-597` |
| OSR-texture import | ABSENT | add import to `IDevice` (`rhi.h:322-338`) + `wgpu_rhi.cpp`; driven by `SurfaceHandoff.shared_texture` (`surface.h:54-57`). ⚠ **AMENDED 2026-07-19:** ~~patched prebuilt required~~ — **macOS only** imports (STOCK `wgpuTextureGetNativeMetalTexture`, no fork); **Windows import is DEFERRED** (CPU-upload ships instead; seam kept, branch disabled — [wgpu-native#621](https://github.com/gfx-rs/wgpu-native/issues/621)); Linux software upload | `renderer_d3d11.cpp:294-331` (D3D11; ⚠ stock wgpu-native v29 C API has NO import — review B-F1 ground truth still exact, but the fork it implied is **REJECTED**; see 03 §3) |
| Composite pass | ABSENT | new wgpu fullscreen-triangle composite beside `render/ui/composite.h` | `renderer_d3d11.cpp:210-272` |
| CEF windowed-OSR host | WINDOWLESS | `editor_host.cpp:340-341` (`SetAsWindowless(0)`), `CefSettings :323-332`, `HostClient :86-121`, `OnPaint :97-100` | `spikes/cef-compositing/main.cpp:626-648` |
| Input pump | ABSENT | feed `InputRouter::route` (`input_router.h:78`) + `UiInputRouter` (`input_routing.h:95`) + `CefBrowserHost::SendMouse/KeyEvent` | `spikes/cef-compositing/main.cpp:262-352` |
| Camera/viewport render abstraction | ABSENT | new Camera/View in `render_world.h` + per-viewport RT via `DynamicTextureRegistry` pattern | — |
| Docking/layout/panel host | ABSENT | new TS editor-core; promote `ExtensionRegistry` (`gui/contract/registry.h:40-61`) to real roster (replacing hand-list `a11y/registry.cpp:21-113`) | Dockview |
| Session/layout persistence | SPECIFIED-UNBUILT | `undo_journal.h:129-136` pattern → layout/windows/undo go to the editor-owned `.editor/editor-state.json`; daemon keeps `.editor/session.json` (03 §1 split) | — |
| Multi-client daemon | SERIAL | `transport.h:14-17`, `kernel_server.h:37,70-82` | — |
| Client SDK | ABSENT | lift `wire_client.h/cpp` plumbing into `context_client`; add CMake install/export | — |
| Selection/camera as session state | PANEL-LOCAL | `MethodBackend` (new verbs) + `register_topic` (new topics) (`registry.cpp:956-1010`) | — |
| Wire write gateway | IN-PROCESS ONLY | `OverrideWriteGateway` (`inspector_panel.h:57-73`) over RPC `edit`/`edit-batch` | — |
| Sandbox/bootstrap packaging | OFF/DEFERRED | `ContextCef.cmake:75-78,:91`; installers net-new; `release-sign.yml` artifact `paths` unwired (`:20-21`) | — |

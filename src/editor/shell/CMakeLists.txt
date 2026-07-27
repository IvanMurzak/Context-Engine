# src/editor/shell/ — M9 e04: the native Shell (`context_editor`).
#
# The first time the engine draws an interactive window in production: a real OS window with the
# single-threaded owner loop, a windowed-OSR browser composited through e03's present path, the
# per-window compositor (incl. the PET_POPUP second layer), the OS input pump with region
# arbitration, real per-monitor DPI, and the C-F2 CPU present fallback.
#
# THE LAYERING, and why almost all of it is default-built:
#
#   context_editor_shell (STATIC, default-built, CEF-free, GPU-backend-free)
#       everything except the CEF calls: window seam + Win32 backend, DPI, input arbitration,
#       compositor, editor-state persistence, the owner loop. Builds and is fully unit-tested under
#       the local Ninja+Strawberry-GCC dev gate and on all three CI `build` legs — the same discipline
#       e03 applied to context_render_present, and for the same reason: CEF is a CI-only dependency
#       path, so anything written INSIDE it would be exercised by one CI job and nothing local.
#
#   context_editor (EXECUTABLE, default-built)
#       the app. Boots the shell over the real window backend when one exists, degrades to the
#       headless backend where it does not, and runs the owner loop. Links the browser binding when
#       CEF is on and the scripted host when it is not — so `context_editor` is a real, runnable,
#       LINKABLE target on every leg, which is what lets the D10 `editor-boundary` job build it.
#
#   context_editor_cef (STATIC, behind CONTEXT_BUILD_GUI_CEF)
#       the windowed-OSR CEF binding — the ONE piece that cannot build locally.
#
# D10: the Shell is an ORDINARY CLIENT. It links the published context_client SDK and the present
# path, and NOTHING from the EditorKernel's internal modules (editorkernel / filesync / derivation /
# compose / merge / assetdb / import / migrate / editor_pkg). That is enforced, not documented — see
# the configure-time gate in src/CMakeLists.txt (`context_assert_shell_boundary`) and its companion
# `editor-shell-boundary` ctest.
#
# ⚠ CORRECTED BY e05d1: this note previously said "and nothing from ... gui", which was never what
# the gate checked and is no longer what the Shell does. Design 04 §4 (D17) makes the C++ panel
# models the logic + a11y authority and has the Shell render them over the bridge, so hosting panels
# REQUIRES reaching the headless GUI libraries. The two the Shell links — `context_gui_uitree` and
# `context_gui_contract` — are D10-clean (their closures contain nothing forbidden), which is why the
# FORBIDDEN list needed no change. What stays out is any `context_gui_panel_*` library: those go on
# `context_editor_panels` (panels/) and reach only the EXECUTABLE, because two of them link the
# forbidden `context_compose`. The gate is the authority on this boundary; the prose above had drifted.

add_library(context_editor_shell STATIC
    src/dpi.cpp           # per-monitor DPI (replaces the spike's DPI-1.0 pin)
    src/input.cpp         # region arbitration + capture stack + focus-class routing (03 §6)
    src/editor_state.cpp  # .editor/editor-state.json — the Shell is its SINGLE writer (03 §1)
    src/window.cpp        # the seam, headless backend, the PURE Win32/X11/Cocoa decoders, the choice
    src/win32_window.cpp  # the Windows OS calls (early-returns to an honest gap elsewhere)
    src/x11_window.cpp    # e12a: the Linux X11/XWayland OS calls (ditto, off Linux)
    src/cocoa_window.cpp  # e12b: the OFF-macOS half of the Cocoa backend (.mm is APPLE-only below)
    src/browser.cpp       # the CEF-free browser seam + the scripted host
    src/compositor.cpp    # the layer stack, damage, resize protocol, both present paths (03 §4)
    src/app_scheme.cpp    # e05c: context-editor://app/ resolution + the strict CSP (04 §1)
    src/ext_scheme.cpp    # e13a-1: context-ext://<pkg>/ deny-by-default resolution + CSP (04 §5 / 08)
    src/ipc_bridge.cpp    # e05c: the privileged bridge envelope + token isolation (04 §1 / 08 §1)
    src/panel_host.cpp    # e05d1: the panel-agnostic panel.* surface over the e05b roster (04 §3-§4)
    src/package_sessions.cpp  # e13c-1: per-package BASELINE daemon sessions + panel.daemon.call (04 §5 / 08)
    src/editor_state_bridge.cpp  # e05d2: the editor.state.*/editor.regions.* surface (03 §1 / §6)
    src/keybindings_bridge.cpp   # e07c: the keybindings.get read/watch surface (05 §6 / 03 §6)
    src/user_config.cpp          # e06d: ~/.context/config.json - the SHELL is its single writer (C-F14)
    src/themes_bridge.cpp        # e06b: the themes.get watched-theme-directory surface (06 §4)
    src/session_bridge.cpp       # e08d: the session.state daemon play-state relay (05 §4 / D7 tier 1)
    src/daemon_lifecycle.cpp  # e14a: spawn-or-attach + exit policy + reconnect/read-only (D18)
    src/welcome.cpp       # e14c: the welcome.* surface — recents + folder picker + new-from-template (D13)
    src/folder_picker.cpp # e14c: the native OS folder dialog (Win32 real; honest gap elsewhere)
    src/banners.cpp       # e14d: notify-only update check + daemon-lost banner surfaces (07 §4 / 08)
    src/native_net.cpp    # e14d: the OS HTTPS client + browser open (Win32 real; honest gap elsewhere)
    src/window_registry.cpp  # e10a: the N-window vocabulary + the 03 §7 create-failure report
    src/window_bridge.cpp    # e10b: the window.* tear-out/move/seed/rehome surface (+ e10c drag.* surface)
    src/cross_window_drag.cpp # e10c: the Shell-mediated cross-window drag session + the RAII cursor capture
    src/write_notice.cpp     # e09b-3: a refused write -> an editor.ui fact in every window (05 §8)
    src/shell.cpp)        # WindowManager / EditorWindow / the single-threaded owner loop

# The version the e14d update banner reports as "running". PROJECT_VERSION is the ONE source of
# truth (src/CMakeLists.txt `project(... VERSION ...)`), so the banner can never disagree with the
# versioned install layout. PRIVATE: it is an implementation detail of banners.cpp's default, and a
# caller that wants a different value calls `set_current_version`.
#
# ⚠ It is NOT sent anywhere. The comparison against the latest published release happens locally
# (banners.h property (b)); this define exists so the editor knows what IT is, never so a server does.
target_compile_definitions(context_editor_shell PRIVATE
    "CONTEXT_EDITOR_VERSION=\"${PROJECT_VERSION}\"")

target_include_directories(context_editor_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_client: the Shell IS a client, and client types appear in its public headers
# (shell.h names client::AttachOptions). PUBLIC context_render_present: the compositor's public
# surface names OsrFrame / IPresentBlitter / CompositeUv.
#
# PUBLIC context_gui_uitree + context_gui_contract (e05d1): panel_host.h names uitree::Panel /
# uitree::Command and contract::Contribution in its public surface. BOTH ARE D10-CLEAN — neither is
# on the shell-boundary gate's FORBIDDEN list, and their own closures stay inside it
# (context_gui_uitree links nothing but context_warnings; context_gui_contract links context_bridge,
# which the gate deliberately does NOT forbid because context_client links it too). This is what lets
# the Shell host panels while the gate's FORBIDDEN list stays byte-identical.
#
# NOTE WHAT IS *NOT* HERE: no `context_gui_panel_*` library. The host is panel-agnostic (see
# panel_host.h), so panel libraries are linked by the EXECUTABLE that binds their providers —
# src/editor/shell/panels/ below. That layering stays load-bearing after e05d3: the hosted panel
# libraries are boundary-clean NOW, but keeping them off THIS target's closure is what keeps the
# runtime half panel-agnostic and the audited surface minimal (and any future not-yet-clean panel
# out of the gate's blast radius).
# PUBLIC context_common (e14a): daemon_lifecycle.h holds a context::common::process::ChildProcess by
# value, so its include dir must reach every consumer of this header. context_common is D10-CLEAN (it
# links only context_warnings — nothing kernel-internal), so adding it to the Shell's closure leaves
# the shell-boundary gate's FORBIDDEN list byte-identical and non-vacuous. The daemon itself is launched
# as a SUBPROCESS (a runtime dependency, not a link one), so nothing forbidden enters the closure.
target_link_libraries(context_editor_shell
    PUBLIC context_client context_common context_render_present context_gui_uitree context_gui_contract
    PRIVATE context_warnings)

# MSVC exception semantics (/EHsc), restored for the Shell family.
#
# CMake's MSVC defaults put /EHsc in CMAKE_CXX_FLAGS. With CONTEXT_BUILD_GUI_CEF=ON the CEF
# acquisition below (context_acquire_cef -> find_package(CEF) -> cef_variables.cmake) CLEARS
# CMAKE_CXX_FLAGS wholesale under the Ninja generator ("clear the CMake defaults to avoid excessive
# console warnings", CEF issue #2120) — and Ninja + cl IS the editor-cef-smoke Windows leg. Without
# /EHsc the unwinding paths in <chrono> / <filesystem> raise C4530, which context_warnings' /WX
# promotes to C2220. The default `build (windows-latest)` leg never sees this: CEF is OFF there, so
# CMake's defaults survive.
#
# SCOPE OF THE DAMAGE: the wipe is DIRECTORY-scoped, not configure-wide — it hits the directories
# that CALL the macro (this one, plus src/editor/cef/ and src/editor/gui/host/) and any subdirectory
# added after the call, such as cef/. It is retroactive within the directory: CMAKE_CXX_FLAGS is read
# at generate time, so a target declared above the call still loses the flag. Targets in OTHER
# directories (context_client, context_render_present, the kernel) keep their flags, which is why
# they compile fine on the CEF legs and why only this family needs repair.
#
# PUBLIC, so every target the wipe exposes inherits it through the link graph: the test + smoke
# executables, context_editor_cef (cef_shell.cpp pulls <chrono> and <filesystem> too) and
# context_editor. Left UNCONDITIONAL rather than gated on CONTEXT_BUILD_GUI_CEF so this family never
# silently depends on a CMake default that something else may wipe; a duplicate /EHsc in a CEF-OFF
# build is a no-op. Deliberately NOT project-wide and NOT on context_warnings — the pre-existing CEF
# targets (src/editor/cef/, src/editor/gui/host/) are green under CEF's own curated flags and stay
# untouched. CEF's MSVC set (/MP /Gy /GF /GR- /utf-8 /W4 /WX /wd... ) specifies no /EH flag at all,
# so this conflicts with nothing.
target_compile_options(context_editor_shell PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

if(WIN32)
    # RegisterClassExW / CreateWindowExW / TrackMouseEvent / the DPI entry points. CMake's default
    # Windows library set already includes user32, but naming it keeps the dependency visible in the
    # target rather than inherited from a toolchain default that a future generator might not carry.
    # ole32 + uuid: the e14c native folder picker (folder_picker.cpp) drives IFileOpenDialog through COM
    # (CoInitializeEx / CoCreateInstance / CoTaskMemFree + the CLSID/IID GUIDs). Boundary-clean — pure
    # OS-SDK libraries, no engine-internal edge, so the D10 shell-boundary gate is unaffected.
    # winhttp + shell32: the e14d update-check seam (native_net.cpp) — the OS HTTPS client and
    # ShellExecuteW for the downloads-page click-through. Both ship with the Windows SDK, so the
    # notify-only update banner adds NO third-party dependency, nothing to src/vcpkg.json, and
    # nothing to tools/license-allowlist.json (the standing owner-approval gate of design 08 §3 is
    # not reached). Boundary-clean, exactly like ole32/uuid above.
    target_link_libraries(context_editor_shell PRIVATE user32 ole32 uuid winhttp shell32)
endif()

# The Linux window backend (M9 e12a): XCreateWindow / the event pump / EWMH placement live in libX11
# — the platform's OWN windowing system, the direct analogue of user32 above, so nothing enters
# src/vcpkg.json or tools/license-allowlist.json and the design 08 §3 dependency gate is not reached
# (the same reasoning the winhttp/ole32 note above records).
#
# PROBED, NOT REQUIRED, for the reason spelled out at the matching block in src/render/present/: a
# hard requirement here would fail CONFIGURE on every ubuntu CI job that installs no X11 packages,
# reddening the whole rollup. Without the headers x11_window.cpp compiles to its honest
# "configured without the X11 development headers" refusal, which make_window_backend reports.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND AND TARGET X11::X11)
        target_link_libraries(context_editor_shell PRIVATE X11::X11)
        target_compile_definitions(context_editor_shell PRIVATE CONTEXT_SHELL_HAS_X11=1)
        message(STATUS "context_editor_shell: X11 found — the Linux window backend is REAL in this "
                       "build")
    else()
        message(STATUS "context_editor_shell: no X11 development headers — the Linux window backend "
                       "compiles to its honest missing-headers report (install libx11-dev for the "
                       "real one)")
    endif()
endif()

# The macOS window backend (M9 e12b): NSWindow + a CAMetalLayer-backed NSView, the NSEvent pump and
# backingScaleFactor DPI live in AppKit + QuartzCore — the platform's OWN windowing system, the
# direct analogue of user32 and libX11 above, so nothing enters src/vcpkg.json or
# tools/license-allowlist.json and the design 08 §3 dependency gate is not reached (the same
# reasoning the winhttp/ole32 and libX11 notes above record).
#
# UNCONDITIONAL on Apple, unlike the two PROBED blocks above: an SDK is not optional there the way an
# X11 development package is on a headless ubuntu runner, and CGSessionCopyCurrentDictionary already
# gives the backend a RUNTIME degrade for a box with no GUI session. The `.mm` is added here rather
# than in the add_library() list because CMake picks a compiler by file EXTENSION, and OBJCXX is
# enabled only under `if(APPLE)` in src/CMakeLists.txt — every other leg compiles the CEF-free
# cocoa_window.cpp stub instead, which is what keeps make_cocoa_window_backend a linkable symbol
# whose off-platform refusal the ctest asserts on all three legs.
#
# ⚠ NO CEF, NO .app BUNDLE. Those are e12c's; nothing here fetches or links a CEF prebuilt, and this
# block leaves `context_editor`'s CEF wiring (Windows/Linux only, further down) byte-identical.
if(APPLE)
    target_sources(context_editor_shell PRIVATE src/cocoa_window.mm)
    # AppKit for NSWindow/NSView/NSEvent, QuartzCore for CAMetalLayer (its class symbol lives there),
    # CoreGraphics for the CGSession GUI-session probe. Metal is named EXPLICITLY rather than left to
    # an SDK umbrella so a later GPU present path — which will want MTLDevice, not just the layer —
    # finds the dependency already declared on the target; the CPU fallback shipped here needs nothing
    # from it (a CAMetalLayer's `contents` is a plain CALayer property).
    target_link_libraries(context_editor_shell PRIVATE
        "-framework AppKit" "-framework QuartzCore" "-framework Metal"
        "-framework CoreGraphics" "-framework Foundation")
    # ARC for the .mm TU: the backend holds NSWindow / NSView / CAMetalLayer / delegate references by
    # value in a C++ class, and ARC is what makes those lifetimes correct without a hand-written
    # rule-of-five over Objective-C objects (mirrors src/render/'s metal_interop.mm).
    set_source_files_properties(src/cocoa_window.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

# Self-contained GCC runtime for every executable below. They use std::filesystem + stream IO, whose
# libstdc++ symbols live in a NEWER libstdc++-6.dll; on a MinGW host a stale libstdc++-6.dll earlier
# on PATH can shadow the toolchain's and be loaded first, yielding STATUS_ENTRYPOINT_NOT_FOUND (or a
# launch-time SegFault) even though build + link were clean. Gated to GNU so the macOS/Clang and
# Windows/MSVC CI legs are untouched (mirrors src/cli/ and src/editor/client/).
set(_context_shell_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest in the `editor-shell-*` family. That
# family is NOT a gate family, so the `build` job's general ctest step auto-runs it on all three OS
# legs and `--preset dev` builds it — no ci.yml `--target` bookkeeping, so the "Not Run = RED"
# tripwire does not apply.
#
# ../../render/tests is on the include path for render_test_rhi.h — the ONE GPU-free fake backend.
# The compositor tests drive the REAL composite path through it: fake device, fake surface, fake
# swapchain (incl. its Outdated/Lost state machine), and the recorded per-pass scissor/draw log.
set(_context_shell_tests
    test_dpi           # scale derivation, rounding, the never-collapse rule, DIP<->physical points
    test_input         # region arbitration, capture (modal vs overlay), focus-class key routing
    test_editor_state  # the single-writer document: debounce, atomic replace, malformed degrade
    test_window        # the headless backend + the PURE Win32 message decoder (all 3 OSes)
    test_compositor    # layer order, damage, resize protocol, PET_POPUP, both present paths
    test_app_scheme    # e05c: URL->asset resolution, traversal refusal, the strict CSP
    test_ext_scheme    # e13a-1: the context-ext:// adversarial suite - traversal, cross-package,
                       #         unknown/invalid package id, the pinned scheme options, the panel CSP
    test_ipc_bridge    # e05c: the T1 adversarial envelope suite + the 3 token-isolation controls
    test_panel_host    # e05d1: the panel-agnostic roster/render/command/gesture/D6-state surface
    test_package_sessions  # e13c-1: the ALLOWLIST, the hardcoded "read" scope proved against the REAL
                       #         dispatcher, lazy attach + the per-package pool + the sub-cap
    test_editor_state_bridge  # e05d2: layout/panels publish+restore round-trip + region-map emission
    test_keybindings_bridge   # e07c: file read/watch/generation + keybindings.get over the router
    test_themes_bridge        # e06b: directory watch/generation + themes.get over the router
    test_session_bridge       # e08d: the session.state relay - fact shape, honest unbound/throwing provider
    test_user_config          # e06d: the single-writer store - merge-preserving RMW, unique temp, config.*
    test_daemon_lifecycle  # e14a: exit-policy decision, client census, reconnect/read-only state
    test_welcome       # e14c: launch-mode rule, recents round-trip, welcome.* bridge + picker/CLI seams
    test_banners       # e14d: the update-check PRIVACY suite, version compare, both banner surfaces
    test_window_registry  # e10a: N windows by id, create/destroy, the retired-session lifetime rule
    test_window_bridge # e10b: the window.* relay - tear-out/move/seed/rehome parse + the loud-fail path
    test_cross_window_drag  # e10c: the drag session - capture RELEASED on every path + the drag.* relay
    test_ui_mirror     # e10d: the editor.ui cross-window mirror relay - broadcast-to-all incl. sender
    test_write_notice  # e09b-3: the LOUD write-notice relay - envelope shape, broadcast, the
                       #         never-a-window-id origin, and the honest unbound/throwing-provider paths
    test_smoke_window  # e12a-x11-legs + e12c-3: the --real-window seam - flag parse, the
                       # anti-degrade negatives, DIP conversion off 1.0, the THREE key-table sweeps
                       # (X11 keysyms, macOS virtual keys, and the cross-arm agreement), and the
                       # physical-pixels <-> Cocoa-points inverses (one extra link edge, added after
                       # context_editor_shell_smoke_support is defined below)
    test_shell)        # the owner loop end to end + the authenticated-attach guard

foreach(_t IN LISTS _context_shell_tests)
    add_executable(context_editor_shell_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editor_shell_${_t} PRIVATE context_editor_shell context_warnings)
    target_link_options(context_editor_shell_${_t} PRIVATE ${_context_shell_static_runtime})
    # ../../client/tests supplies `mock_channel.h` — the SCRIPTED WIRE the e13c-1 package-session
    # suite drives a REAL client::Client over. Reused rather than re-mocked for the reason that header
    # states itself: a mock more capable than the daemon hides real reader bugs (it hid an empty
    # granted_scopes() for six tasks), and the flat-attach-reply rule is written down once there.
    # src/editor/shell/panels/ already reuses it the same way.
    target_include_directories(context_editor_shell_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../client/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../render/tests)
    add_test(NAME editor-shell-${_t} COMMAND context_editor_shell_${_t})
endforeach()

# --- the SMOKE-TIER window seam (M9 e12a-x11-legs, issue #408) ------------------------------------
# How a live smoke chooses between the honest offscreen shell and a REAL OS window — the seam that
# lets the nine `editor-cef-smoke-shell*` scenarios run through a real X11 window on Linux instead of
# each hard-coding HeadlessWindowBackend + MemoryBlitter (and, in two of them, casting the base
# reference back down to the concrete headless backend to reach `post()`).
#
# A SEPARATE LIBRARY, deliberately NOT part of context_editor_shell: it is test-tier code, and
# issue #408 asks for a seam that does not widen the SHIPPING window contract. Nothing in
# context_editor_shell links it; only the smokes do. It is CEF-free, so it is compiled and
# unit-tested (editor-shell-test_smoke_window) on all three default `build` legs — the same
# discipline the rest of the Shell follows.
#
# ⚠ It adds an include ROOT (smoke/include). The pre-push audit's check 9 compiles a changed CEF TU
# standalone against the pinned CEF headers with a hand-maintained set of roots, so this one is
# recorded in the profile's `setup.md` § Preconditions (the `src/editor/shell/**` known-good set) —
# a check-9 `fatal error: context/editor/shell/smoke/smoke_window.h: No such file` is that gap, not
# a code break.
#
# smoke_inject_cocoa.cpp is in the UNCONDITIONAL source list and the `.mm` is added under if(APPLE)
# below — the always-linkable-symbol pair make_cocoa_window_backend uses, for the same reason: CMake
# picks a compiler by file EXTENSION and OBJCXX is enabled only on Apple, so Objective-C++ cannot hide
# behind a preprocessor guard in a `.cpp` every leg compiles.
add_library(context_editor_shell_smoke_support STATIC
    smoke/src/smoke_window.cpp
    smoke/src/smoke_inject_cocoa.cpp)
target_include_directories(context_editor_shell_smoke_support
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/smoke/include)
target_link_libraries(context_editor_shell_smoke_support
    PUBLIC context_editor_shell
    PRIVATE context_warnings)

# The X11 half (real-mode input injection) rides the SAME probe as the window backend above — and
# must, for the same reason: a hard requirement would fail CONFIGURE on every ubuntu job that
# installs no X11 packages. Without the headers `inject_event` compiles to its honest
# "no real-window injection in this build" refusal, which every caller treats as a failure.
if(UNIX AND NOT APPLE)
    # Called HERE rather than relied upon from the window-backend block above: find_package is
    # cached/idempotent, and depending on a call ~100 lines away would silently disable real-mode
    # injection if that block were ever reordered or extracted. Same shape as src/render/present/.
    find_package(X11)
    if(X11_FOUND AND TARGET X11::X11)
        target_link_libraries(context_editor_shell_smoke_support PRIVATE X11::X11)
        target_compile_definitions(context_editor_shell_smoke_support PRIVATE
            CONTEXT_SHELL_SMOKE_HAS_X11=1)
        message(STATUS "context_editor_shell_smoke_support: X11 found — --real-window input "
                       "injection is REAL in this build")
    else()
        # Said out loud because the whole anti-degrade thesis rests on telling a compiled-out X11
        # path from a present one: without this line a build that silently lost X11 looks identical
        # to one that has it, right up until nine live legs fail at run time.
        message(STATUS "context_editor_shell_smoke_support: no X11 development headers — "
                       "--real-window injection compiles to its honest refusal (install libx11-dev "
                       "for the real one)")
    endif()
endif()

# The macOS half (M9 e12c-3): real-mode injection through -[NSApplication postEvent:atStart:], which
# is IN-PROCESS and therefore needs no TCC/Accessibility grant a GitHub runner could not hold (see
# smoke_window.h for the measurement). UNCONDITIONAL on Apple, exactly like the window backend block
# above and for the same reason: an SDK is not optional there the way an X11 development package is on
# a headless ubuntu runner, and the Cocoa backend's CGSessionCopyCurrentDictionary probe already gives
# the RUNTIME degrade for a box with no GUI session.
#
# AppKit for NSEvent/NSApplication/NSWindow, QuartzCore for CALayer (the injection recovers the window
# by matching the backend's published CAMetalLayer against each NSWindow's contentView layer), and
# Foundation for NSString/NSProcessInfo. NO CEF, and nothing new in src/vcpkg.json or
# tools/license-allowlist.json — the platform's own windowing system, the same reasoning the AppKit
# block above records.
if(APPLE)
    target_sources(context_editor_shell_smoke_support PRIVATE smoke/src/smoke_inject_cocoa.mm)
    target_link_libraries(context_editor_shell_smoke_support PRIVATE
        "-framework AppKit" "-framework QuartzCore" "-framework Foundation")
    # ARC for the .mm TU, mirroring src/editor/shell/'s cocoa_window.mm and src/render/'s
    # metal_interop.mm. It holds NO Obj-C pointer members (only locals), which is why its __bridge of
    # the backend's layer handle is guarded by the native-kind check rather than by a lifetime rule —
    # see the file's own header.
    set_source_files_properties(smoke/src/smoke_inject_cocoa.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    message(STATUS "context_editor_shell_smoke_support: AppKit — --real-window input injection is "
                   "REAL in this build (macOS)")
endif()

# Its own T1 suite is a plain member of the `_context_shell_tests` family above, so the foreach
# already gave it its executable, its runtime flags, its include dirs and its non-gate
# `editor-shell-test_smoke_window` registration (`--preset dev` builds it and the general ctest step
# runs it on all three OS legs, with no ci.yml --target bookkeeping). Staying in the loop is what
# keeps a future family-wide change — a define, a property, a label — from silently skipping it.
# The ONE thing the loop cannot know is this extra link edge, which is why only it is spelled here.
# It asserts the seam's DECISIONS without opening a window or needing a display: the flag parse, the
# headless construction + present, DIP conversion at a non-identity scale, the three key-table
# sweeps, the physical-pixels <-> Cocoa-points inverses (which the live macOS smoke provably CANNOT
# catch a wrong SCALE in — see smoke_window.h), and — the load-bearing ones — that real mode REFUSES
# a headless backend, and refuses a window with no presentable native surface, rather than degrading
# onto `post()` / an in-memory present.
target_link_libraries(context_editor_shell_test_smoke_window
    PRIVATE context_editor_shell_smoke_support)

# --- the panel composition root (e05d1) ----------------------------------------------------------
# Added AFTER context_editor_shell (it links it) and BEFORE context_editor + the CEF smoke (they link
# it). Deliberately NOT inside the CONTEXT_BUILD_GUI_CEF branch: the panel host and its providers are
# CEF-free, so they build + unit-test on the local dev gate and all three default `build` legs — the
# same discipline the rest of the Shell follows.
add_subdirectory(panels)

# --- the Session-0-safe smoke (the BLOCKING CI requirement of e04) -------------------------------
# The self-hosted Windows CI runner has no interactive desktop and native-GPU windowed teardown
# crashes there (Session 0), so the blocking smoke drives the REAL shell loop over the HEADLESS
# window backend + software-OSR frames + e03's MemoryBlitter, and asserts the COMPOSITED PRESENT
# per-pixel. Nothing in it opens a window, creates a GPU device, or links CEF — which is exactly what
# makes it safe to run on every OS leg, including Session 0.
#
# Registered in the same non-gate `editor-shell-` family, so it rides the build job's general ctest
# step on all three OSes with no ci.yml enumeration.
add_executable(context_editor_shell_smoke smoke/shell_smoke_main.cpp)
target_link_libraries(context_editor_shell_smoke PRIVATE context_editor_shell context_warnings)
target_link_options(context_editor_shell_smoke PRIVATE ${_context_shell_static_runtime})
add_test(NAME editor-shell-smoke-session0 COMMAND context_editor_shell_smoke)
set_tests_properties(editor-shell-smoke-session0 PROPERTIES TIMEOUT 120)

# --- the LIVE WINDOWED Linux smoke (M9 e12a) -----------------------------------------------------
# The mirror image of the Session-0 smoke above: same real owner loop, same real compositor, same
# real panel models — but through a REAL X11 window, the REAL X11 present blitter, and the REAL X
# server as the event source. It is the DoD proof that the editor boots WINDOWED with live panels
# under Linux, and it links CEF nowhere, so it runs on the local dev host and costs the CI job
# nothing but a build.
#
# Linux-only by construction, and it SKIPS (ctest SKIP_RETURN_CODE 77) when there is no display —
# which is the ordinary state of `build (ubuntu-latest)` and both `sanitize` legs, where it must not
# fail. That skip is NOT allowed to become permanent: the editor-cef-smoke Linux leg (which has
# xvfb) runs the executable directly with --require-x11 --require-display, under which a compiled-out
# X11 path or a missing display is a hard failure. Registered in the non-gate `editor-shell-` family,
# so `--preset dev` builds it and the general ctest step runs it with no ci.yml --target bookkeeping.
#
# It links context_editor_panels (NOT context_editor_shell alone) for the same reason context_editor
# does: the panel composition root is what binds real providers, and "live panels" is only a checked
# claim if the real ones render.
#
# e12a-x11-legs: it now builds its window + present path through the SHARED smoke-tier seam
# (smoke/smoke_window.h) instead of its own copy, and asserts a pointer + key round trip through
# that seam's X-server injection. That is what makes this CEF-FREE, locally-runnable smoke the
# proving ground for the seam the nine CEF scenarios depend on.
if(UNIX AND NOT APPLE)
    add_executable(context_editor_shell_x11_smoke smoke/shell_x11_smoke_main.cpp)
    target_link_libraries(context_editor_shell_x11_smoke PRIVATE context_editor_panels
                                                                 context_editor_shell_smoke_support
                                                                 context_warnings)
    target_link_options(context_editor_shell_x11_smoke PRIVATE ${_context_shell_static_runtime})
    add_test(NAME editor-shell-x11-window COMMAND context_editor_shell_x11_smoke)
    set_tests_properties(editor-shell-x11-window PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# --- the LIVE WINDOWED macOS smoke (M9 e12c-3) ----------------------------------------------------
# The macOS twin of the X11 smoke above, and for the same reason: e12b landed the whole Cocoa backend
# but its own record states that "a window appears / a frame is visible is asserted NOWHERE", because
# macOS runs all nine `editor-cef-smoke-shell*` smokes HEADLESS. This is the DoD proof that the editor
# boots WINDOWED with live panels on macOS — a REAL NSWindow through the REAL make_window_backend, the
# REAL CALayer.contents present blitter, the REAL e05d1 panel models, and the REAL AppKit queue as the
# event source (the pointer + key samples it asserts come BACK out of the backend's own
# nextEventMatchingMask pump).
#
# CEF-FREE, deliberately: it is therefore unaffected by the CEF keychain class (issue #437 / x7), it
# rides the plain `build (macos-latest)` leg for its compile, and it runs on the local macOS dev host
# in seconds instead of a CI round.
#
# macOS-only by construction, and it SKIPS (ctest SKIP_RETURN_CODE 77) when there is no GUI (Aqua)
# session — the ordinary state of the `build (macos-latest)` leg, the only default leg this target is
# configured on at all (it lives inside `if(APPLE)`; both sanitize legs are ubuntu), where it must not
# fail. That
# skip is NOT allowed to become permanent: the editor-cef-smoke macOS leg runs the executable DIRECTLY
# with --require-cocoa --require-display, under which a missing session, a refused window and a missing
# OS blitter are all hard failures. The `ctest`-reports-a-SKIP-as-a-PASS trap e12a paid for applies
# verbatim here, which is why that step is a direct run and not a `ctest -R` line.
#
# It links context_editor_panels (NOT context_editor_shell alone) for the same reason context_editor
# does: the panel composition root is what binds real providers, and "live panels" is only a checked
# claim if the real ones render.
if(APPLE)
    add_executable(context_editor_shell_cocoa_smoke smoke/shell_cocoa_smoke_main.cpp)
    target_link_libraries(context_editor_shell_cocoa_smoke PRIVATE context_editor_panels
                                                                   context_editor_shell_smoke_support
                                                                   context_warnings)
    target_link_options(context_editor_shell_cocoa_smoke PRIVATE ${_context_shell_static_runtime})
    add_test(NAME editor-shell-cocoa-window COMMAND context_editor_shell_cocoa_smoke)
    set_tests_properties(editor-shell-cocoa-window PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# --- the windowed-OSR CEF binding (CI-only dependency path) --------------------------------------
# Added ONLY behind CONTEXT_BUILD_GUI_CEF, and AFTER src/editor/cef in src/CMakeLists.txt so the
# shared CEF acquisition (fetch + libcef_dll_wrapper) has already run there.
#
# The acquisition prologue is invoked HERE, in the directory that defines context_editor, rather than
# inside cef/. cmake/ContextCef.cmake is a MACRO precisely because find_package(CEF) populates a large
# surface of plain (non-CACHE) variables and macros — CEF_STANDARD_LIBS, CEF_BINARY_FILES,
# SET_EXECUTABLE_TARGET_PROPERTIES, COPY_FILES — in the INCLUDING scope. context_editor needs them,
# and a subdirectory's scope does not propagate back up. It is idempotent (the third call site).
if(CONTEXT_BUILD_GUI_CEF)
    context_acquire_cef(context_editor editor-cef-smoke)

    # --- ONE staging of the CEF payload for this whole directory tree (issue #360) ----------------
    # CEF_TARGET_OUT_DIR is set by SET_CEF_TARGET_OUT_DIR() inside the macro above, in THIS directory
    # scope — so it resolves to build/<preset>/editor/shell/<config>, and the cef/ subdirectory added
    # below inherits that same value. TEN executables therefore share one staging destination:
    # context_editor here, plus the nine live smokes in cef/. Each used to attach its OWN POST_BUILD
    # COPY_FILES() of the identical payload, which ninja then ran concurrently as it linked them in
    # parallel — a Windows sharing violation whenever two collided (issue #360; the CI job that builds
    # all nine smokes in one ninja invocation is where it bites). One stage target, and every consumer
    # depends on it, removes the race by construction. See cmake/ContextCef.cmake for the mechanism +
    # the evidence, and tools/check_cef_staging.py for the lint that keeps it that way.
    #
    # Windows/Linux only: macOS stages nothing next to the executable (COPY_MAC_FRAMEWORK embeds the
    # framework into each .app instead — see the macOS block below, which the configure-time audit at
    # the bottom of this file asserts never grows a stage target).
    if(OS_WINDOWS OR OS_LINUX)
        context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")
    endif()

    # --- macOS: the .app + per-process-type helper-bundle hosting model (M9 e12c-1, issue #436) ----
    #
    # WHY THIS EXISTS AT ALL. CEF cannot re-exec the main app as a subprocess on macOS, so every
    # subprocess type runs from its own "<App> Helper*.app" bundle inside the main app's
    # Contents/Frameworks, and the framework is LOADED at runtime (CefScopedLibraryLoader) rather than
    # linked. src/editor/cef/ and src/editor/gui/host/ already assemble exactly this shape and boot
    # green on `editor-cef-smoke (macos-latest)`; e12c-1 PORTS that proven model to the Shell rather
    # than inventing one. What it does NOT port is their `nullptr` CefApp — see
    # shell::cef::execute_helper_process (cef_shell.h) for why the Shell's helpers need the real app.
    #
    # DEFINED HERE, ABOVE add_subdirectory(cef), AND CALLED FROM BOTH: the nine live smokes are
    # declared in cef/ and `context_editor` here, and all ten need the identical assembly. A CMake function
    # does not change the directory context, so `add_executable` / `add_custom_command(TARGET ...)`
    # inside it act on the CALLER's directory — which is required, since add_custom_command(TARGET)
    # refuses a target created in another directory. The two paths the body needs are therefore
    # absolute variables rather than ${CMAKE_CURRENT_SOURCE_DIR}-relative literals (cef/ inherits them
    # as an ordinary subdirectory scope), or each caller would resolve them against its own directory.
    #
    # FIVE helper bundles per app, not three: CEF_HELPER_APP_SUFFIXES in the pinned distribution's
    # cmake/cef_variables.cmake lists the unsuffixed helper plus (Alerts), (GPU), (Plugin) and
    # (Renderer). docs/shell.md claimed three until this task measured it.
    #
    # ⚠ THE FRAMEWORK IS EMBEDDED PER BUNDLE (304 MB apiece, MEASURED). That is the owner's design
    # ruling: it is the only configuration proven green in CI. Since e12c-2 fanned the recipe out to
    # all nine smokes the Shell's own share is TEN bundles (the app + nine smokes) at 309 MB apiece —
    # that is each FINISHED .app, i.e. the 304 MB framework above plus its own binaries and five
    # helpers — MEASURED on an arm64 macOS host at 3.1 GB for this directory's Release output, inside a
    # WHOLE CEF-ON `src/build/dev` of 8.0 GB. docs/shell.md § 3 owns the runner-headroom comparison and
    # is the one place to RE-MEASURE if this list grows again; it is not restated here, because a figure
    # written down twice is a figure that drifts.
    # A symlinked Versions/A pointing at one staged copy was verified to still boot, but it
    # is held ONLY as an escape hatch if runner disk ever binds and must stay TEST-TIER: codesigning and
    # notarization reject a symlinked framework, so e15 packaging must not inherit it.
    if(OS_MAC)
        set(CONTEXT_SHELL_MAC_PLIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mac")
        set(CONTEXT_SHELL_MAC_HELPER_SOURCE
            "${CMAKE_CURRENT_SOURCE_DIR}/cef/src/shell_helper_mac.cpp")

        # Assemble ONE macOS CEF app: turn `main_target` into a MACOSX_BUNDLE, build its five helper
        # bundles, embed the framework, and copy the helpers into Contents/Frameworks.
        #
        # `bundle_identifier` is the main bundle's CFBundleIdentifier; each helper takes it plus its own
        # `.helper<suffix>`, so no two bundles share an identifier.
        function(context_shell_cef_mac_bundle main_target bundle_identifier)
            set(_app "${CEF_TARGET_OUT_DIR}/${main_target}.app")
            set(_helper_targets "")

            foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
                string(REPLACE ":" ";" _suffixes ${_suffixes})
                list(GET _suffixes 0 _name_suffix)
                list(GET _suffixes 1 _target_suffix)

                set(_htarget "${main_target}_Helper${_target_suffix}")
                # ⚠ LOAD-BEARING NAME. CEF resolves the helper executable from the MAIN executable's
                # own name — "<main exe> Helper<name suffix>" inside Contents/Frameworks — so an
                # OUTPUT_NAME that does not match makes the browser fail to spawn a renderer at
                # runtime, with a bundle that built perfectly. The configure-time audit at the bottom
                # of this file re-asserts this equality against the real target properties.
                set(_houtput "${main_target} Helper${_name_suffix}")

                add_executable(${_htarget} MACOSX_BUNDLE "${CONTEXT_SHELL_MAC_HELPER_SOURCE}")
                SET_EXECUTABLE_TARGET_PROPERTIES(${_htarget})
                add_dependencies(${_htarget} libcef_dll_wrapper)
                # context_editor_cef FIRST — it holds cef_shell.cpp, which is where
                # execute_helper_process and the ShellCefApp the helper must hand to CefExecuteProcess
                # live, and it is an archive whose CEF references have to be resolvable by something
                # later on the line (the single-left-to-right-pass rule documented in cef/). NO
                # libcef_lib: on macOS the framework is dlopen'd at runtime, never linked, which is why
                # the two precedent bundles link only the wrapper + CEF_STANDARD_LIBS too.
                target_link_libraries(${_htarget}
                    context_editor_cef libcef_dll_wrapper ${CEF_STANDARD_LIBS})
                target_include_directories(${_htarget} PRIVATE "${CEF_ROOT}")
                set_target_properties(${_htarget} PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${CONTEXT_SHELL_MAC_PLIST_DIR}/helper-Info.plist.in"
                    MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}.helper${_target_suffix}"
                    MACOSX_BUNDLE_BUNDLE_NAME "${_houtput}"
                    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
                    OUTPUT_NAME "${_houtput}")
                list(APPEND _helper_targets ${_htarget})
            endforeach()

            # MACOSX_BUNDLE as a PROPERTY, not an add_executable keyword: `context_editor` is declared
            # unconditionally further down (it must stay a linkable target on every leg, CEF or not),
            # so its bundle-ness can only be switched on here. CMake reads the property at generate
            # time, so setting it after the fact is equivalent.
            #
            # PROJECT_VERSION feeds both plist version strings — the ONE source of truth
            # (src/CMakeLists.txt `project(... VERSION ...)`), so the bundles cannot drift from it the
            # way the hardcoded 0.0.1 in the two older plist pairs can.
            set_target_properties(${main_target} PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_INFO_PLIST "${CONTEXT_SHELL_MAC_PLIST_DIR}/Info.plist.in"
                MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_identifier}"
                MACOSX_BUNDLE_BUNDLE_NAME "${main_target}"
                MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
                MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

            # The helpers must be BUILT before the app's POST_BUILD copies them in. Also what the
            # configure-time audit reads back (MANUALLY_ADDED_DEPENDENCIES) to prove no helper was
            # silently dropped.
            add_dependencies(${main_target} ${_helper_targets})

            # Embed the framework (with its Versions/A symlink layout) into Contents/Frameworks. ONE
            # embedding target per app path — the macOS analogue of the issue-#360 single-writer rule,
            # and what tools/check_cef_staging.py's macOS half enforces from the sources.
            COPY_MAC_FRAMEWORK(${main_target} "${CEF_BINARY_DIR}" "${_app}")

            # Each helper .app into the main app's Contents/Frameworks. These POST_BUILD commands are
            # all attached to the SAME target, so ninja serialises them — unlike the per-target copies
            # into one shared directory that were issue #360 on Windows/Linux.
            foreach(_htarget ${_helper_targets})
                add_custom_command(TARGET ${main_target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "$<TARGET_BUNDLE_DIR:${_htarget}>"
                            "${_app}/Contents/Frameworks/$<TARGET_BUNDLE_DIR_NAME:${_htarget}>"
                    VERBATIM)
            endforeach()
        endfunction()
    endif()

    add_subdirectory(cef)
endif()

# --- context_editor: the app -----------------------------------------------------------------
# Default-built on every leg. `CONTEXT_EDITOR_HAS_CEF` selects the real browser binding over the
# scripted host; with CEF off the editor still boots, runs the owner loop, and presents — it simply
# has no web content, which is the honest state of a build that has no browser in it.
add_executable(context_editor app/editor_main.cpp)
# context_editor_panels (e05d1) brings the panel providers — and with them the panel libraries
# this build can link. It carries context_editor_shell transitively (PUBLIC), so naming both
# would be redundant; the panels library is the one that must be here, because it is what makes
# the executable's D10-audited closure include the hosted panels.
target_link_libraries(context_editor PRIVATE context_editor_panels)
target_link_options(context_editor PRIVATE ${_context_shell_static_runtime})

# The e05c default asset root, compiled in so a dev build of `context_editor` finds editor-core with
# no arguments; `--app-root` overrides it, and PACKAGING (e15) will replace the default with an
# install-relative path. Deliberately a define and NOT an add_dependencies(context_editor
# context_editor_webui): the assets are a RUNTIME input, `context_editor_webui` is an ALL target so
# any ordinary `cmake --build` produces them anyway, and the D10 `editor-boundary` job — which
# builds context_editor from a hand-maintained `--target` list but never RUNS it — therefore needs
# no ci.yml change. The live CEF smoke, which does run and does load the assets, takes the real
# dependency instead (see cef/CMakeLists.txt).
if(NOT CONTEXT_WEBUI_ASSET_DIR)
    message(FATAL_ERROR
        "context_editor: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui must be added BEFORE "
        "src/editor/shell (src/CMakeLists.txt orders it so); it publishes the asset root as CACHE "
        "INTERNAL.")
endif()
target_compile_definitions(context_editor PRIVATE
    "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

# The GPU backend is opt-in (CONTEXT_BUILD_RENDER_WGPU, default OFF). With it the editor presents
# through the e03 swapchain; without it there is no way to create an adapter at all and the C-F2 CPU
# fallback is the whole path — a supported configuration, not a degraded build.
if(TARGET context_render_wgpu)
    target_link_libraries(context_editor PRIVATE context_render_wgpu)
    target_compile_definitions(context_editor PRIVATE CONTEXT_EDITOR_HAS_WGPU=1)
endif()

# Link the real browser everywhere the app can HOST one — since e12c-1 that is all three v1 platforms.
# Windows/Linux use CEF's self-re-exec model (the exe is its own subprocess), which is what
# src/editor/cef/ and src/editor/gui/host/ already do. macOS needs an .app bundle plus per-process-type
# helper bundles and the runtime-loaded framework, which the `elseif(OS_MAC)` branch below assembles
# through context_shell_cef_mac_bundle() — so `context_editor` is a REAL browser-hosting app on macOS
# now, and the old "mac builds CEF-free, the binding's compile is the coverage" carve-out is gone.
if(CONTEXT_BUILD_GUI_CEF AND (OS_WINDOWS OR OS_LINUX))
    if(NOT TARGET libcef_lib)
        ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    endif()
    # CEF's own curated warning set replaces context_warnings on a CEF-linking target — the repo's
    # -Wpedantic baseline is incompatible with the CEF headers + IMPLEMENT_REFCOUNTING macros (the
    # same carve-out src/editor/cef/ and src/editor/gui/host/ take). context_editor_shell still
    # links context_warnings PRIVATE, so the Shell's own code stays gated.
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor)
    add_dependencies(context_editor libcef_dll_wrapper)
    # context_editor_cef FIRST, before the CEF libs — the single-pass-ld archive-ordering rule
    # documented at the matching line in cef/CMakeLists.txt. Same archive, same CEF references, so
    # the same order is required here.
    target_link_libraries(context_editor PRIVATE
        context_editor_cef libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor PRIVATE "${CEF_ROOT}")
    target_compile_definitions(context_editor PRIVATE CONTEXT_EDITOR_HAS_CEF=1)

    # The libcef binary payload + resources are staged next to the exe by the ONE shared stage target
    # created above (issue #360) — never by a per-target POST_BUILD copy, which raced its three
    # siblings into this same directory.
    add_dependencies(context_editor context_editor_cef_stage)
elseif(CONTEXT_BUILD_GUI_CEF AND OS_MAC)
    # The macOS half (M9 e12c-1): the same CEF link, but the payload travels INSIDE the .app rather
    # than beside the executable, so there is no stage target to depend on — COPY_MAC_FRAMEWORK embeds
    # the framework and the five helper bundles carry the subprocess entry points.
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor)
    add_dependencies(context_editor libcef_dll_wrapper)
    # context_editor_cef FIRST, before the CEF libs — the same single-pass-ld archive-ordering rule as
    # the Windows/Linux branch. NO libcef_lib on macOS: the framework is dlopen'd at runtime.
    target_link_libraries(context_editor PRIVATE
        context_editor_cef libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor PRIVATE "${CEF_ROOT}")
    target_compile_definitions(context_editor PRIVATE CONTEXT_EDITOR_HAS_CEF=1)

    # Turn it into the real .app + its five helper bundles. Note the consequence, deliberately scoped:
    # `context_editor` is a MACOSX_BUNDLE only in a CEF-ON macOS configure. A CEF-OFF macOS build (the
    # default `build (macos-latest)` leg and the `editor-boundary` job) keeps the plain executable it
    # has always produced, because the bundle exists to satisfy CEF's process model — the SHIPPING
    # bundle, with its own identifier, icon and Dock presence, is e15 packaging's.
    context_shell_cef_mac_bundle(context_editor "dev.context.editor")
else()
    target_link_libraries(context_editor PRIVATE context_warnings)
endif()

# --- THE ONE ROSTER of CEF-hosting executables in this tree, read by BOTH configure-time audits -----
# ⚠ KEEP THIS COMPLETE. Both audits below iterate THE LIST, so a target missing from it is silently
# UNDER-AUDITED rather than caught — and that has already happened: context_editor_shell_uimirror_smoke
# (e10d-drill2) and context_editor_shell_iframe_smoke (e13a-2) both boot from ${CEF_TARGET_OUT_DIR} and
# both take the stage dependency, but neither was listed until e12a-x11-legs noticed. A new smoke
# belongs here, ONCE — e12c-2 collapsed two hand-maintained ten-name copies into this one list.
#
# WHY ONE LIST IS SOUND for two audits with different charters (Windows/Linux staging vs macOS bundle):
# every CEF-hosting Shell executable needs BOTH, on whichever platform it is configured for. For the
# SMOKES that holds by construction — context_configure_shell_cef_smoke() calls
# context_shell_cef_mac_bundle() unconditionally under its own if(OS_MAC), and requires BUNDLE_ID on
# every platform so a Windows-only author cannot skip macOS silently (see cef/CMakeLists.txt). For
# `context_editor`, member #1, it holds by HAND: its bundle comes from the OS_MAC branch above with a
# hand-written identifier, not through that function. So every SMOKE is locked together; `context_editor`
# alone is a maintained invariant — if a future executable is ever deliberately one-platform-only, this
# list is what has to split again.
set(_ctx_cef_shell_executables
    context_editor
    context_editor_shell_cef_smoke
    context_editor_shell_restore_smoke
    context_editor_shell_palette_smoke
    context_editor_shell_settings_smoke
    context_editor_shell_multiwindow_smoke
    context_editor_shell_tearout_smoke
    context_editor_shell_drag_smoke
    context_editor_shell_uimirror_smoke
    context_editor_shell_iframe_smoke
    context_editor_shell_inspector_fanout_smoke)

# --- the CEF single-staging invariant, enforced on the real build graph (issue #360) --------------
# Every executable that boots out of the shared ${CEF_TARGET_OUT_DIR} must depend on the ONE stage
# target — that dependency is the entire fix, and it is exactly the sort of thing a new smoke exe
# silently forgets. Reading MANUALLY_ADDED_DEPENDENCIES is ground truth about the graph CMake will
# generate, which no source scan can be, so this runs at CONFIGURE time and fails the CEF-ON configure
# on every leg before a single object is compiled. Its companion is the source-level lint below
# (`editor-shell-cef-staging`), which catches the other half — a re-introduced per-target POST_BUILD
# copy — and runs on the default CEF-OFF matrix where this block does not exist at all.
if(CONTEXT_BUILD_GUI_CEF AND (OS_WINDOWS OR OS_LINUX))
    foreach(_consumer IN LISTS _ctx_cef_shell_executables)
        if(NOT TARGET ${_consumer})
            message(FATAL_ERROR
                "CEF staging audit: expected target ${_consumer} to exist in a CONTEXT_BUILD_GUI_CEF "
                "Windows/Linux configure. If it was renamed or removed, update "
                "_ctx_cef_shell_executables — a silently skipped consumer is how issue #360 would come "
                "back unnoticed.")
        endif()
        get_target_property(_manual_deps ${_consumer} MANUALLY_ADDED_DEPENDENCIES)
        if(NOT _manual_deps OR NOT "context_editor_cef_stage" IN_LIST _manual_deps)
            message(FATAL_ERROR
                "CEF staging audit: ${_consumer} runs from \${CEF_TARGET_OUT_DIR} but does not depend "
                "on context_editor_cef_stage. Add add_dependencies(${_consumer} "
                "context_editor_cef_stage) — do NOT give it its own POST_BUILD COPY_FILES(), which is "
                "the concurrent-copy race of issue #360.")
        endif()
    endforeach()
endif()

# --- the macOS BUNDLE-COMPLETENESS audit, on the real build graph (M9 e12c-1, issue #436) ----------
# The macOS analogue of the staging audit above, and it exists for the same reason: the properties a
# macOS CEF app must carry are trivially assertable from the build graph and genuinely hard to prove
# from behaviour — a bundle missing one helper boots fine and then fails to spawn exactly one
# subprocess type, at run time, on one OS. Reading target properties is ground truth about what CMake
# will generate, which no source scan can be, so this runs at CONFIGURE time and fails a CEF-ON macOS
# configure before a single object is compiled.
#
# ⚠ IT ASSERTS ONLY REAL GRAPH FACTS, never that "the assembly function ran" — a self-declared marker
# property would pass for a bundle whose framework never got embedded. The two things it cannot see are
# covered elsewhere: the framework embed is a POST_BUILD command CMake exposes no read-back for, so its
# single-writer rule is enforced from the SOURCES by tools/check_cef_staging.py and its presence is
# proven by the live smokes (a bundle with no embedded framework fails LoadInMain and reds).
if(CONTEXT_BUILD_GUI_CEF AND OS_MAC)
    set(_ctx_cef_bundle_ids "")
    foreach(_bundle IN LISTS _ctx_cef_shell_executables)
        if(NOT TARGET ${_bundle})
            message(FATAL_ERROR
                "macOS CEF bundle audit: expected target ${_bundle} to exist in a "
                "CONTEXT_BUILD_GUI_CEF macOS configure. If it was renamed or removed, update "
                "_ctx_cef_shell_executables — a silently skipped bundle is how a half-assembled .app "
                "would ship unnoticed.")
        endif()
        get_target_property(_is_bundle ${_bundle} MACOSX_BUNDLE)
        if(NOT _is_bundle)
            message(FATAL_ERROR
                "macOS CEF bundle audit: ${_bundle} is not a MACOSX_BUNDLE. CEF loads its framework "
                "from <app>/Contents/Frameworks and spawns subprocesses from helper bundles beside it, "
                "so a bare executable cannot host a browser at all. Call "
                "context_shell_cef_mac_bundle(${_bundle} <identifier>).")
        endif()
        # NO TWO BUNDLES MAY SHARE A CFBundleIdentifier. context_shell_cef_mac_bundle()'s header
        # promises this, but it can only keep the promise WITHIN one app (each helper takes the main id
        # plus its own `.helper<suffix>`); nothing stopped two MAIN bundles being handed the same
        # BUNDLE_ID, and e12c-2 took the hand-written identifiers from three to ten. macOS keys
        # LaunchServices on the identifier, so a collision builds and installs perfectly and then
        # misbehaves at RUN time on ONE OS — the class this audit exists to move to configure time.
        get_target_property(_bundle_id ${_bundle} MACOSX_BUNDLE_GUI_IDENTIFIER)
        if(NOT _bundle_id)
            message(FATAL_ERROR
                "macOS CEF bundle audit: ${_bundle} has no MACOSX_BUNDLE_GUI_IDENTIFIER. Every bundle "
                "needs its own CFBundleIdentifier — pass BUNDLE_ID to "
                "context_configure_shell_cef_smoke() (scheme dev.context.editor.shell.<scenario>.smoke).")
        endif()
        if("${_bundle_id}" IN_LIST _ctx_cef_bundle_ids)
            message(FATAL_ERROR
                "macOS CEF bundle audit: ${_bundle} reuses CFBundleIdentifier '${_bundle_id}', which "
                "another bundle in this configure already claims. Every one must be distinct, since "
                "macOS keys LaunchServices on it and a collision fails only at run time. Give this one "
                "its own BUNDLE_ID (scheme dev.context.editor.shell.<scenario>.smoke).")
        endif()
        list(APPEND _ctx_cef_bundle_ids "${_bundle_id}")
        get_target_property(_main_output ${_bundle} OUTPUT_NAME)
        if(NOT _main_output)
            set(_main_output "${_bundle}")
        endif()
        get_target_property(_manual_deps ${_bundle} MANUALLY_ADDED_DEPENDENCIES)
        foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
            string(REPLACE ":" ";" _suffixes ${_suffixes})
            list(GET _suffixes 0 _name_suffix)
            list(GET _suffixes 1 _target_suffix)
            set(_htarget "${_bundle}_Helper${_target_suffix}")
            if(NOT TARGET ${_htarget})
                message(FATAL_ERROR
                    "macOS CEF bundle audit: ${_bundle} has no helper target ${_htarget}. CEF needs "
                    "one helper bundle per CEF_HELPER_APP_SUFFIXES entry (FIVE: the unsuffixed one "
                    "plus Alerts, GPU, Plugin, Renderer) — a missing one fails to spawn exactly that "
                    "process type, at run time.")
            endif()
            if(NOT _manual_deps OR NOT "${_htarget}" IN_LIST _manual_deps)
                message(FATAL_ERROR
                    "macOS CEF bundle audit: ${_bundle} does not depend on its helper ${_htarget}, so "
                    "the POST_BUILD copy into Contents/Frameworks can run before the helper is "
                    "linked. Add add_dependencies(${_bundle} ${_htarget}).")
            endif()
            # The load-bearing equality: CEF derives the helper executable path from the MAIN
            # executable's own name. A mismatch builds perfectly and then cannot spawn a renderer.
            get_target_property(_helper_output ${_htarget} OUTPUT_NAME)
            if(NOT _helper_output STREQUAL "${_main_output} Helper${_name_suffix}")
                message(FATAL_ERROR
                    "macOS CEF bundle audit: helper ${_htarget} has OUTPUT_NAME '${_helper_output}' "
                    "but CEF will look for '${_main_output} Helper${_name_suffix}' inside "
                    "${_main_output}.app/Contents/Frameworks. The two must match exactly.")
            endif()
        endforeach()
    endforeach()

    # And the negative half, which keeps the "macOS embeds per bundle" decision from silently acquiring
    # a second, racing payload writer: nothing may stage the CEF payload NEXT TO a macOS executable.
    #
    # ⚠ A SECOND LINE OF DEFENCE, and the planting round says so. Simply dropping the
    # `if(OS_WINDOWS OR OS_LINUX)` guard above does NOT reach this check —
    # context_cef_stage_payload() refuses first, because CEF_BINARY_FILES/CEF_RESOURCE_FILES are empty
    # on macOS (cmake/ContextCef.cmake). What this catches is the shape that refusal cannot see: a
    # HAND-ROLLED `add_custom_target(context_editor_cef_stage)` (or any other target claiming that
    # name) in a macOS configure, which is what a future "let me just stage it myself" refactor writes.
    # That is the plant it was proven non-vacuous against.
    if(TARGET context_editor_cef_stage)
        message(FATAL_ERROR
            "macOS CEF bundle audit: context_editor_cef_stage exists in a macOS configure. macOS "
            "stages nothing beside the executable — COPY_MAC_FRAMEWORK embeds the framework into each "
            ".app. Keep context_cef_stage_payload() under its if(OS_WINDOWS OR OS_LINUX) guard.")
    endif()
endif()

# --- the source-level companion lint (runs on EVERY default build leg, no CEF required) -----------
# A SOURCE scan over the CMake build files (the same tier as tools/check_no_raw_key_handlers.py):
# no CEF staging destination may have more than one writer, every consumer of a stage target must
# depend on it, and the staging path must contain no retry / sleep / ignored exit code. Registered in
# the non-gate `editor-shell-` family, so the build job's general ctest step runs it on all three OS
# legs with no ci.yml --target bookkeeping. Its own pytest lives in tools/tests/ (python-tests job).
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME editor-shell-cef-staging
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_cef_staging.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

# --- the C-F14 single-writer gate for ~/.context/config.json (M9 e06d) ---------------------------
# The SAME source-scan tier as the CEF staging lint above, for the same reason: "the Shell is the
# single writer" is trivially assertable and genuinely hard to prove from behaviour — a runtime test
# passes just as happily with a second writer present. This scan answers the questions a second writer
# would have to make false (one C++ writer; no client-side persistence in editor-core; one caller of
# `config.set`), and it FAILED on the pre-e06d tree, where welcome.cpp had its own writer. Registered
# in the non-gate `editor-shell-` family, so the build job's general ctest step runs it on all three OS
# legs with no ci.yml --target bookkeeping; its own pytest lives in tools/tests/ (python-tests job).
add_test(NAME editor-shell-config-writers
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_config_writers.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

# --- the #437 OSCrypt-keychain isolation gate for every CEF smoke --------------------------------
# The SAME source-scan tier as the two above, and for the starkest version of the same reason: the
# rule is ONE line per smoke, and its absence cannot fail any test. Chromium reads a MACHINE-GLOBAL
# `"<product> Safe Storage"` keychain item on a BLOCK_SHUTDOWN ThreadPool task; macOS binds that
# item's ACL to the CREATING executable's cdhash, so a smoke built anywhere else — or the SAME smoke
# after a rebuild — is off the ACL and securityd raises a modal SecurityAgent prompt no automated run
# can answer. `CefShutdown()` waits on the ThreadPool shutdown event, so the process never exits,
# AFTER printing its whole success verdict. A hung test asserts nothing, so no runtime assertion can
# catch the omission; and the hang is host-state-dependent (issue #437 passed on this very host four
# minutes before the keychain item was created), so CI green proves nothing either. Measured on one
# binary a minute apart: 5/5 hangs without the isolation, 5/5 passes with it.
#
# Registered in the non-gate `editor-shell-` family, so the build job's general ctest step runs it on
# all three OS legs with no ci.yml `--target` bookkeeping — and it needs NO CEF build, so it also runs
# on the CEF-OFF legs where the smokes themselves do not exist. Its own pytest lives in tools/tests/
# (python-tests job) and proves it non-vacuous by planting; that round found TWO real holes in the
# first revision, both GREEN, both shapes the next live scenario could write (e12c-2 fanned the
# EXISTING nine out to macOS and added no source; e12c-3 and every later T2 scenario will) — see the
# test file's docstring.
add_test(NAME editor-shell-cef-keychain
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_cef_keychain_isolation.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

# --- the C-F3 SESSION-FILE OWNERSHIP gate (M9 e09d, design 03 §1 / 07 §6) ------------------------
# The SAME source-scan tier as the config-writers gate above, for the same reason one step further
# out: `.editor/session.json` is the DAEMON's and `.editor/editor-state.json` is the SHELL's, and a
# second writer of either is a torn write that needs TWO PROCESSES RACING to be observable at all —
# so no runtime test in this repo can fail on it, however many are written. The scan answers the
# questions a second writer would have to make false (one C++ writer per file; each owner in its own
# process's subtree; the in-process compose/filesync override-write gateway named only by tests) plus
# an ANTI-VACUITY half for EVERY ONE of them, because each prohibition is trivially satisfied by a
# tree that deleted the thing it protects: the sole writer must still write THAT DOCUMENT (not merely
# contain write machinery — its own private helper's definition supplies that forever), the gateway
# TYPE must still be named by its owners, and the gateway LIBRARY must still be declared.
#
# Proven NON-VACUOUS by planting (conventions.md § "Authoring a SOURCE-SCAN gate") across TWO rounds,
# the second run against the finished first revision. Both found real gaps, in both directions —
# writers the scan missed AND a commented-out CMake link it wrongly reported, which would have redded
# all three build legs. The single enumeration is the `# FOUND BY PLANTING` markers in
# tools/tests/test_check_session_ownership.py (the python-tests job), each a regression case; no count
# is repeated here, because the counts in this comment, docs/shell.md and that file's docstring had
# already drifted into describing three different rounds.
#
# Registered in the non-gate `editor-shell-` family, so the build job's general ctest step runs it on
# all three OS legs with no ci.yml --target bookkeeping.
add_test(NAME editor-shell-session-ownership
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_session_ownership.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

# --- the e14d update-check PRIVACY gate (08 threat row: no identifiers, no telemetry) -------------
# The SAME source-scan tier as the two lints above, and here for the sharpest version of their
# reason: the C++ suite (editor-shell-test_banners) can prove the request VALUE carries no
# identifier, but it cannot see what the OS HTTPS client adds BELOW that value, cannot see a SECOND
# request builder it was never pointed at, and cannot see a renderer `fetch()` that would carry
# Chromium's User-Agent + Accept-Language beneath the JavaScript. Those three are exactly where the
# privacy commitment would be lost, so they are scanned from the sources. Registered in the non-gate
# `editor-shell-` family, so the build job's general ctest step runs it on all three OS legs with no
# ci.yml --target bookkeeping; its own pytest lives in tools/tests/ (python-tests job).
add_test(NAME editor-shell-release-request
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_release_request.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

# --- the L-41 external-BeginFrame gate (M9 e12b; cef#4033) ----------------------------------------
# The SAME source-scan tier as the three lints above, and here for the reason that makes this tier
# exist at all: THERE IS NO CALL SITE TO ASSERT AGAINST. L-41 forbids `SendExternalBeginFrame`, and
# nothing in the tree calls it — so no runtime test can fail if one appears, because an assertion can
# only observe behaviour that EXISTS. The commitment is enforced from the sources instead, which is
# also what lets it run with no CEF in the build: the API is CEF's, but the rule is ours.
#
# It carries a POSITIVE half as well as the two prohibitions, for the same reason: a prohibition
# passes trivially against a tree that deleted the thing it protects, so the gate also asserts the
# L-41 pin in compositor/surface.cpp is still there and still `false`.
#
# Proven NON-VACUOUS by planting (conventions.md § "Authoring a SOURCE-SCAN gate"): thirteen shapes
# spanning the call's variant axes — a chained getter, a call split across lines, a member-function
# POINTER that writes no paren at all, a runtime string, the enabling flag in three spellings, a
# post-hoc MUTATION of the sanctioned handoff in another file, the pin deleted, the pin flipped, plus
# the two false-positive controls the LIVE tree already contains (a prose mention and a printf format
# string). All thirteen matched expectation and the post-restore run came back green on a byte-exact
# tree; every one is pinned as a regression case in tools/tests/test_check_no_external_begin_frame.py.
#
# Registered in the non-gate `editor-shell-` family, so the build job's general ctest step runs it on
# all three OS legs with no ci.yml --target bookkeeping; its own pytest rides the python-tests job.
add_test(NAME editor-shell-no-external-begin-frame
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/../tools/check_no_external_begin_frame.py"
            --repo-root "${CMAKE_SOURCE_DIR}/..")

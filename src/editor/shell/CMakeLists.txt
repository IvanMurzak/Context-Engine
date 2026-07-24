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
    src/window.cpp        # the window seam, headless backend + the PURE Win32 message decoder
    src/win32_window.cpp  # the Windows OS calls (early-returns to an honest gap elsewhere)
    src/browser.cpp       # the CEF-free browser seam + the scripted host
    src/compositor.cpp    # the layer stack, damage, resize protocol, both present paths (03 §4)
    src/app_scheme.cpp    # e05c: context-editor://app/ resolution + the strict CSP (04 §1)
    src/ipc_bridge.cpp    # e05c: the privileged bridge envelope + token isolation (04 §1 / 08 §1)
    src/panel_host.cpp    # e05d1: the panel-agnostic panel.* surface over the e05b roster (04 §3-§4)
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
    test_ipc_bridge    # e05c: the T1 adversarial envelope suite + the 3 token-isolation controls
    test_panel_host    # e05d1: the panel-agnostic roster/render/command/gesture/D6-state surface
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
    test_shell)        # the owner loop end to end + the authenticated-attach guard

foreach(_t IN LISTS _context_shell_tests)
    add_executable(context_editor_shell_${_t} tests/${_t}.cpp)
    target_link_libraries(context_editor_shell_${_t} PRIVATE context_editor_shell context_warnings)
    target_link_options(context_editor_shell_${_t} PRIVATE ${_context_shell_static_runtime})
    target_include_directories(context_editor_shell_${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../../render/tests)
    add_test(NAME editor-shell-${_t} COMMAND context_editor_shell_${_t})
endforeach()

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
    # below inherits that same value. FOUR executables therefore share one staging destination:
    # context_editor here, plus the three live smokes in cef/. Each used to attach its OWN POST_BUILD
    # COPY_FILES() of the identical payload, which ninja then ran concurrently as it linked them in
    # parallel — a Windows sharing violation whenever two collided (issue #360; the CI job that builds
    # all three smokes in one ninja invocation is where it bites). One stage target, and every consumer
    # depends on it, removes the race by construction. See cmake/ContextCef.cmake for the mechanism +
    # the evidence, and tools/check_cef_staging.py for the lint that keeps it that way.
    #
    # Windows/Linux only: macOS stages nothing next to the executable (COPY_MAC_FRAMEWORK embeds the
    # framework into each .app instead), and the Shell has no macOS CEF executable until e12 anyway.
    if(OS_WINDOWS OR OS_LINUX)
        context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")
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

# Link the real browser only where the app can actually HOST one. Windows/Linux use CEF's
# self-re-exec model (the exe is its own subprocess), which is what src/editor/cef/ and
# src/editor/gui/host/ already do. macOS needs an .app bundle plus per-process-type helper bundles
# and the runtime-loaded framework; that packaging is e12's, so `context_editor` there builds
# CEF-free — the "mac/Linux compile-guarded until e12" DoD line. The BINDING (context_editor_cef)
# still compiles on macOS, which is what makes that guard a real compile check rather than a skip.
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
else()
    target_link_libraries(context_editor PRIVATE context_warnings)
endif()

# --- the CEF single-staging invariant, enforced on the real build graph (issue #360) --------------
# Every executable that boots out of the shared ${CEF_TARGET_OUT_DIR} must depend on the ONE stage
# target — that dependency is the entire fix, and it is exactly the sort of thing a new smoke exe
# silently forgets. Reading MANUALLY_ADDED_DEPENDENCIES is ground truth about the graph CMake will
# generate, which no source scan can be, so this runs at CONFIGURE time and fails the CEF-ON configure
# on every leg before a single object is compiled. Its companion is the source-level lint below
# (`editor-shell-cef-staging`), which catches the other half — a re-introduced per-target POST_BUILD
# copy — and runs on the default CEF-OFF matrix where this block does not exist at all.
if(CONTEXT_BUILD_GUI_CEF AND (OS_WINDOWS OR OS_LINUX))
    set(_ctx_cef_stage_consumers
        context_editor
        context_editor_shell_cef_smoke
        context_editor_shell_restore_smoke
        context_editor_shell_palette_smoke
        context_editor_shell_settings_smoke
        context_editor_shell_multiwindow_smoke
        context_editor_shell_tearout_smoke
        context_editor_shell_drag_smoke)
    foreach(_consumer IN LISTS _ctx_cef_stage_consumers)
        if(NOT TARGET ${_consumer})
            message(FATAL_ERROR
                "CEF staging audit: expected target ${_consumer} to exist in a CONTEXT_BUILD_GUI_CEF "
                "Windows/Linux configure. If it was renamed or removed, update this list — a silently "
                "skipped consumer is how issue #360 would come back unnoticed.")
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

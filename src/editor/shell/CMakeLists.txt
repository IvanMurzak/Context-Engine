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
    src/daemon_lifecycle.cpp  # e14a: spawn-or-attach + exit policy + reconnect/read-only (D18)
    src/welcome.cpp       # e14c: the welcome.* surface — recents + folder picker + new-from-template (D13)
    src/folder_picker.cpp # e14c: the native OS folder dialog (Win32 real; honest gap elsewhere)
    src/shell.cpp)        # WindowManager / EditorWindow / the single-threaded owner loop

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
    target_link_libraries(context_editor_shell PRIVATE user32 ole32 uuid)
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
    test_daemon_lifecycle  # e14a: exit-policy decision, client census, reconnect/read-only state
    test_welcome       # e14c: launch-mode rule, recents round-trip, welcome.* bridge + picker/CLI seams
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

    # Stage the libcef binary payload + resources next to the exe so it runs from the build tree.
    COPY_FILES(context_editor "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
    COPY_FILES(context_editor "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")
else()
    target_link_libraries(context_editor PRIVATE context_warnings)
endif()

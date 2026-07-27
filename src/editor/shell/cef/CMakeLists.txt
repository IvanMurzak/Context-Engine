# src/editor/shell/cef/ — M9 e04: the windowed-OSR CEF binding + the NINE LIVE smokes.
#
# The ONE CEF-dependent part of the Shell, and therefore a CI-ONLY dependency path like
# src/editor/cef/ and src/editor/gui/host/ on a host whose only CEF prebuilt is MSVC/Clang-ABI (the
# Windows dev gate): everything ABOVE this — compositor, layer stack, PET_POPUP, input arbitration,
# the owner loop — is CEF-free and unit-tested on all three default `build` legs.
#
# The parent (src/editor/shell/CMakeLists.txt) has already run the shared CEF-acquisition prologue
# (context_acquire_cef), so CEF_ROOT / libcef_dll_wrapper / the CEF macros / CEF_TARGET_OUT_DIR and
# — on macOS — context_shell_cef_mac_bundle() are all in scope here.
#
# WARNINGS POSTURE (mirrors src/editor/cef/ + src/editor/gui/host/): CEF-linking targets do NOT link
# context_warnings — CEF ships its own curated -Werror set, and the repo's -Wpedantic baseline is
# incompatible with the CEF headers + IMPLEMENT_REFCOUNTING macros. context_editor_shell, which this
# links, DOES link it, so the Shell's own code stays warnings-as-errors gated on the default matrix.
#
# The local pre-flight for this file on a CEF-less host is the `-fsyntax-only` gate the sibling
# README describes: compile cef_shell.cpp against the pinned CEF HEADERS (fetched + SHA-verified by
# tools/fetch_cef.py) with no prebuilt library. On the macOS host the real thing builds AND runs.

if(NOT CONTEXT_BUILD_GUI_CEF)
    return()
endif()

add_library(context_editor_cef STATIC src/cef_shell.cpp)

target_include_directories(context_editor_cef
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE "${CEF_ROOT}")
# context_editor_shell is PUBLIC: cef_shell.h names IBrowserHost, Extent2D and DpiScale.
target_link_libraries(context_editor_cef PUBLIC context_editor_shell)
add_dependencies(context_editor_cef libcef_dll_wrapper)

# --- e12a-x11-legs (#408): the whole family runs through a REAL X11 window on Linux ----------------
# Attached at REGISTRATION time rather than read from an environment variable, for three reasons: the
# Session-0 Windows leg and macOS keep the offscreen backend they require with no per-smoke branch; a
# build that quietly lost its X11 window backend or its X11 present blitter fails LOUDLY there
# instead of degrading (smoke_window.h's real mode never falls back); and no `std::getenv` is needed
# in nine TUs, which MSVC rejects as C4996 under /W4 /WX.
#
# macOS is HEADLESS here, exactly as Windows is: the live WINDOWED macOS proof — a real NSWindow,
# Cocoa event injection, the CEF-free cocoa smoke — is e12c-3, the exact mirror of the
# e12a -> e12a-x11-legs carve-out.
set(_ctx_smoke_window_args "")
if(OS_LINUX)
    set(_ctx_smoke_window_args --real-window)
endif()

# ==================================================================================================
# ONE declaration path for all nine live smokes (M9 e12c-2)
# ==================================================================================================
# WHY THIS FUNCTION EXISTS. Before e12c-2 each smoke carried ~35 near-verbatim lines — the same link
# recipe, the same staging dependency, the same webui asset dependency + compiled-in root, the same
# ctest registration — and the macOS branch was a SECOND copy of the same thing for the two smokes
# e12c-1 had ported. Fanning the macOS recipe out to the remaining seven by copy-paste would have
# landed ~250 more lines of it (the e12a-x11-legs retrospective had already filed the duplication as
# a follow-up). One function collapses both, and it does something copy-paste could not: it makes
# every per-smoke difference an ARGUMENT, so the differences are readable in one table below.
#
# ⚠ BUNDLE_ID IS REQUIRED ON EVERY PLATFORM, not just macOS, and that is the point. The reason the
# macOS leg shipped e12c-1 with only two of nine smokes is that a smoke could be declared with no
# macOS story at all. Requiring the identifier everywhere means a tenth smoke cannot land
# Windows-only: the argument list refuses it at configure time, on every leg.
#
# ⚠ WHAT DELIBERATELY STAYS AT THE CALL SITE, and why it is not an oversight:
#   * add_executable() + SET_EXECUTABLE_TARGET_PROPERTIES() — tools/check_cef_staging.py finds CEF
#     executables by scanning the SOURCES for `SET_EXECUTABLE_TARGET_PROPERTIES(<name>)`. It resolves
#     a `${var}` name through file-local `set()`, but a function PARAMETER is unresolvable by
#     construction, and the lint (correctly) refuses to skip a name it cannot resolve — it reports the
#     stage dependency as UNVERIFIED rather than assuming it. Keeping this call literal keeps all nine
#     executables visible to the lint. It also has to precede the macOS bundle assembly, which this
#     function performs.
#   * add_dependencies(<exe> context_editor_cef_stage) — the same lint reads this edge LITERALLY to
#     prove every executable booting out of the shared ${CEF_TARGET_OUT_DIR} depends on the ONE stage
#     target (issue #360). Nine literal lines under one platform guard below say exactly that.
# Both are the invariants a source-level lint exists to police, so they are spelled out rather than
# hidden behind an indirection the lint cannot follow.
#
# ⚠ "Not Run = RED" TRIPWIRE (CE #264), once, for the whole family: the `editor-cef-smoke` CI job
# builds a hand-maintained `--target` list, so every executable declared below is listed in ci.yml's
# editor-cef-smoke build step (and mirrored in the target profile's test.md § CI). The
# `ctest -R "^editor-cef-smoke-"` step would otherwise match a test whose executable was never built.
#
# ⚠ LINK ORDER IS LOAD-BEARING: context_editor_cef comes FIRST, BEFORE the CEF libs. GNU ld resolves
# a static archive in ONE left-to-right pass — it pulls an archive member only to satisfy an
# already-undefined symbol, and any NEW undefined symbol that member introduces can only be resolved
# by something LATER on the line. cef_shell.cpp lives inside this archive and calls into libcef, so
# listing the archive after libcef leaves its CEF references unresolvable (`undefined reference to
# cef_string_utf16_clear`). The sibling CEF exes (src/editor/cef/, src/editor/gui/host/) list their
# libs in the opposite order and link fine only because THEIR CEF-calling code is a direct object file
# of the executable, not an archive member — do not "harmonize" this line to match.
# context_editor_shell_smoke_support (e12a-x11-legs) and context_editor_panels (e05d1) join on the
# same side: the smokes bind the real panel providers so the live renderer has something to hydrate,
# and neither introduces CEF references of its own (every panel library is CEF-free), so their
# position relative to the CEF libs is not load-bearing — but keeping them left of them costs nothing.
#
# PER-OS SUBPROCESS MODEL, the one real platform difference in the body. On Windows/Linux the exe
# self-re-execs for CEF's subprocesses (the model src/editor/cef/ and src/editor/gui/host/ use) and
# the libcef payload is staged BESIDE it. macOS cannot self-re-exec: every subprocess type runs from
# its own helper bundle inside the main .app, with the framework LOADED at runtime
# (CefScopedLibraryLoader) rather than linked. So on macOS, item by item:
#   * no `libcef_lib` — the framework is dlopen'd, never linked, which is why the precedent bundles
#     link only the wrapper + the standard libs;
#   * no `context_editor_cef_stage` dependency — nothing is staged beside the executable, because
#     COPY_MAC_FRAMEWORK embeds the payload INSIDE each .app. The stage target does not even exist in
#     a macOS configure (its `if(OS_WINDOWS OR OS_LINUX)` guard in the parent), and the
#     configure-time macOS audit there asserts it never appears;
#   * no `WORKING_DIRECTORY` on the ctest — there is no staged payload next to the exe to be relative
#     to (the two precedent macOS registrations omit it for the same reason);
#   * the bundle assembly itself, via context_shell_cef_mac_bundle() from the parent directory.
#
# ⚠ `add_test(COMMAND <target>)` RESOLVES TO THE IN-BUNDLE EXECUTABLE on macOS —
# `<name>.app/Contents/MacOS/<name>` — because CMake substitutes the target's TARGET_FILE, and
# TARGET_FILE of a MACOSX_BUNDLE is the Mach-O inside it. That is what makes the restore smoke's
# `argv[0]` re-launch work here with NO macOS-specific code: the path it re-execs is already the
# bundled executable, so the helper framework search resolves. VERIFIED, not assumed —
# `editor-cef-smoke-boot` and `cef-substrate-boot` passed on macOS off exactly this resolution before
# e12c-1, and e12c-1's restore smoke then proved it for a two-phase re-launch.
#
# ⚠ THE macOS TEARDOWN WEDGE IS FIXED AT ITS CAUSE (issue #437) — history kept because the wrong
# reading of it was expensive. e12c-1 shipped its two macOS smokes `DISABLED TRUE`: each reached
# EVERY scenario milestone, printed `cef::shutdown() (CefShutdown) begin`, and then emitted nothing
# until ctest killed it. That report blamed `CefSettings.external_message_pump`; it was WRONG.
# Chromium reads a MACHINE-GLOBAL `"<product> Safe Storage"` keychain item on a BLOCK_SHUTDOWN
# ThreadPool task, macOS binds that item's ACL to the CREATING executable's cdhash, and every other
# binary — including the same target after a rebuild — is off the ACL, so securityd raises a modal
# SecurityAgent prompt no automated run can answer. `CefShutdown()` waits on the ThreadPool shutdown
# event, so it never returns, AFTER the smoke printed its whole success verdict. Every CEF smoke now
# sets `use_mock_keychain = true` (tools/check_cef_keychain_isolation.py fails the build if one
# stops; docs/cef-keychain-isolation.md carries the mechanism), which is why no registration below
# carries a `DISABLED` property. Nothing about teardown was weakened: `CefShutdown()` still runs in
# full and `-shell-restore` phase 1 still asserts that it RETURNED (`cef_shutdown_returned`), so the
# CE #319 lifetime invariant is proven on macOS too rather than skipped there.
function(context_configure_shell_cef_smoke)
    cmake_parse_arguments(SMOKE "" "TARGET;TEST;TIMEOUT;BUNDLE_ID" "EXTRA_LIBS" ${ARGN})

    foreach(_required TARGET TEST TIMEOUT BUNDLE_ID)
        if(NOT SMOKE_${_required})
            message(FATAL_ERROR
                "context_configure_shell_cef_smoke: ${_required} is required. Every live Shell CEF "
                "smoke needs a target, a ctest name, a timeout AND a macOS bundle identifier — the "
                "identifier is required on every platform on purpose, so a new smoke cannot land "
                "Windows/Linux-only the way the first nine did (M9 e12c-1 -> e12c-2).")
        endif()
    endforeach()
    if(SMOKE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "context_configure_shell_cef_smoke(${SMOKE_TARGET}): unrecognised arguments "
            "'${SMOKE_UNPARSED_ARGUMENTS}'. Extra link libraries go through EXTRA_LIBS.")
    endif()
    if(NOT TARGET ${SMOKE_TARGET})
        message(FATAL_ERROR
            "context_configure_shell_cef_smoke: ${SMOKE_TARGET} is not a target. The caller declares "
            "it with add_executable() + SET_EXECUTABLE_TARGET_PROPERTIES() first — see the ⚠ note on "
            "what stays at the call site and why tools/check_cef_staging.py needs it literal.")
    endif()

    add_dependencies(${SMOKE_TARGET} libcef_dll_wrapper)
    if(OS_MAC)
        target_link_libraries(${SMOKE_TARGET}
            context_editor_cef context_editor_shell_smoke_support context_editor_panels
            ${SMOKE_EXTRA_LIBS} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    else()
        target_link_libraries(${SMOKE_TARGET}
            context_editor_cef context_editor_shell_smoke_support context_editor_panels
            ${SMOKE_EXTRA_LIBS} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    endif()
    target_include_directories(${SMOKE_TARGET} PRIVATE "${CEF_ROOT}")

    # --- the e05c asset root (and why this is a DEPENDENCY, not just a define) ---------------------
    # Every smoke loads `context-editor://app/index.html[?ctx-smoke-…]`, so it needs editor-core's
    # built asset set to EXIST when it runs. The `editor-cef-smoke` CI job builds a hand-maintained
    # `--target` list, so nothing would otherwise build the assets there and the smoke would fail on a
    # 404 that looks like a scheme bug. Expressing it as add_dependencies() makes the asset set travel
    # with each target automatically — which is what keeps ci.yml's `--target` list (and the profile's
    # test.md § CI mirror of it) from needing the webui target added alongside every smoke.
    add_dependencies(${SMOKE_TARGET} context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "${SMOKE_TARGET}: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui must be added "
            "BEFORE src/editor/shell (src/CMakeLists.txt orders it so); it publishes the asset root "
            "as CACHE INTERNAL.")
    endif()
    target_compile_definitions(${SMOKE_TARGET} PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    if(OS_MAC)
        context_shell_cef_mac_bundle(${SMOKE_TARGET} "${SMOKE_BUNDLE_ID}")
        add_test(NAME ${SMOKE_TEST} COMMAND ${SMOKE_TARGET})
        set_tests_properties(${SMOKE_TEST} PROPERTIES TIMEOUT ${SMOKE_TIMEOUT})
    else()
        add_test(NAME ${SMOKE_TEST} COMMAND ${SMOKE_TARGET} ${_ctx_smoke_window_args})
        set_tests_properties(${SMOKE_TEST} PROPERTIES
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${SMOKE_TARGET}>"
            TIMEOUT ${SMOKE_TIMEOUT})
    endif()
endfunction()

# The logical CEF library target the non-macOS link lines above name. macOS never links it (the
# framework is dlopen'd at runtime), so it is not created there at all.
if(OS_WINDOWS OR OS_LINUX)
    if(NOT TARGET libcef_lib)
        ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    endif()
endif()

# ==================================================================================================
# The nine live smokes. Every one runs on all three v1 platforms since M9 e12c-2.
# ==================================================================================================
# The TIMEOUT column is the only budget knob and it tracks the number of sequential live browser
# boots a scenario performs: a single-boot smoke budgets 180, two boots plus their round-trip waits
# 300, three boots 420, and the two-CHILD-PROCESS restore controller 360.

# --- the LIVE CEF boot smoke (ctest editor-cef-smoke-shell) ---------------------------------------
# Boots a REAL windowless CEF browser through the REAL integrated pump and asserts its OnPaint frames
# reach the REAL compositor and are presented — the half the Session-0-safe smoke cannot cover.
add_executable(context_editor_shell_cef_smoke src/cef_shell_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_cef_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_cef_smoke
    TEST editor-cef-smoke-shell
    TIMEOUT 180
    BUNDLE_ID "dev.context.editor.shell.smoke")

# --- the LIVE boot -> dock -> restart -> restore smoke (ctest editor-cef-smoke-shell-restore) -----
# M9 e05d4: proves editor-core's arrangement survives a RESTART. The exe is a thin controller that
# runs ITSELF twice as two child processes (--ctx-restore-phase=1|2) against ONE project — phase 1
# persists a real docking arrangement then hard-exits, phase 2 (a fresh PROCESS + a fresh
# editor-state store LOADED from the file phase 1 wrote) reads it back and reapplies it. Two
# processes, not two browsers in one CefInitialize: a mid-process browser teardown SEGFAULTs on the
# Session-0 Windows runner AND ubuntu, and each single-browser phase is instead structurally
# identical to the proven-green boot smoke above (see the .cpp header). context_common joins the link
# line because it is the hardened std::system runner (issue #146) the controller re-launches its two
# phase children with; it introduces no CEF references of its own.
add_executable(context_editor_shell_restore_smoke src/cef_shell_restore_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_restore_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_restore_smoke
    TEST editor-cef-smoke-shell-restore
    TIMEOUT 360
    BUNDLE_ID "dev.context.editor.shell.restore.smoke"
    EXTRA_LIBS context_common)

# --- the LIVE command-driven palette smoke (ctest editor-cef-smoke-shell-palette) -----------------
# M9 e07d T2: boots the SAME live windowless browser as the boot smoke and then drives a scenario
# PURELY through the palette / command dispatch (open -> filter -> execute view.panel.close), whose
# editor.state.publish over the real bridge is the observable it asserts.
add_executable(context_editor_shell_palette_smoke src/cef_shell_palette_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_palette_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_palette_smoke
    TEST editor-cef-smoke-shell-palette
    TIMEOUT 180
    BUNDLE_ID "dev.context.editor.shell.palette.smoke")

# --- the LIVE settings-driven smoke (ctest editor-cef-smoke-shell-settings) -----------------------
# M9 e06d T2: boots the SAME live windowless browser and drives a REAL theme change through the REAL
# Settings panel (`?ctx-smoke-settings`), then asserts what only the SHELL can know — that it
# PERSISTED the choice to the user config (C-F14) and preserved the rest of the document. The config
# it writes is a TEMP file the smoke seeds and deletes, never the runner's own
# `~/.context/config.json`.
add_executable(context_editor_shell_settings_smoke src/cef_shell_settings_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_settings_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_settings_smoke
    TEST editor-cef-smoke-shell-settings
    TIMEOUT 180
    BUNDLE_ID "dev.context.editor.shell.settings.smoke")

# --- the LIVE MULTI-WINDOW smoke (ctest editor-cef-smoke-shell-multiwindow) -----------------------
# M9 e10a: the three DoD lines a CEF-free test structurally cannot reach — a SECOND real CEF browser
# booting its OWN fresh editor-core instance (proven by two distinct round-tripped handshake nonces),
# a REAL renderer `window.open` refused by `OnBeforePopup` with NO browser created, and a MID-PROCESS
# window destroy followed by another create. The deterministic 25-cycle create/destroy version is the
# CEF-free `editor-shell-test_window_registry`, which runs on all three default `build` legs; this
# proves the same discipline on the real CEF path, on the Session-0 Windows runner where CE #319
# faulted. THREE sequential live boots (window 0, window 1, and the one created after the
# mid-process destroy) plus the popup and repaint waits.
add_executable(context_editor_shell_multiwindow_smoke src/cef_shell_multiwindow_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_multiwindow_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_multiwindow_smoke
    TEST editor-cef-smoke-shell-multiwindow
    TIMEOUT 420
    BUNDLE_ID "dev.context.editor.shell.multiwindow.smoke")

# --- the LIVE TEAR-OUT + rehome smoke (ctest editor-cef-smoke-shell-tearout) ----------------------
# M9 e10b: the DoD lines a CEF-free test cannot reach — a `window.tear-out` producing a SECOND real
# CEF browser that boots its OWN editor-core and RESTORES the moved D6 state (proven because the live
# renderer's `window.seed` consumes the relay's boot seed), a LOUD create-failure result (03 §7), and
# a `window.move-to` rehome that window 0's LIVE editor-core drains on its `window.rehomed` poll.
add_executable(context_editor_shell_tearout_smoke src/cef_shell_tearout_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_tearout_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_tearout_smoke
    TEST editor-cef-smoke-shell-tearout
    TIMEOUT 300
    BUNDLE_ID "dev.context.editor.shell.tearout.smoke")

# --- the LIVE CROSS-WINDOW DRAG smoke (ctest editor-cef-smoke-shell-drag) -------------------------
# M9 e10c: the DoD line a CEF-free test cannot reach — the cross-window drop-zone query genuinely
# ROUND-TRIPS to a DIFFERENT window's LIVE editor-core over IPC (window 1 polls `drag.probe`,
# hit-tests its own layout, answers `drag.report-zone`), the drop rehomes through e10b's EXISTING
# move path, and the global cursor capture is released on drop AND cancel.
add_executable(context_editor_shell_drag_smoke src/cef_shell_drag_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_drag_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_drag_smoke
    TEST editor-cef-smoke-shell-drag
    TIMEOUT 300
    BUNDLE_ID "dev.context.editor.shell.drag.smoke")

# --- the LIVE CROSS-WINDOW editor.ui MIRROR smoke (ctest editor-cef-smoke-shell-uimirror) ---------
# M9 e10d-drill2: INHERITED DRILL 2, the DoD line a CEF-free test (or a fake) cannot reach — an
# `editor.ui` fact published in window 0's LIVE editor-core reaches window 1's LIVE editor-core
# through the REAL Shell mirror broadcast (window 1's bus reports `applied >= 1` over the real
# `ui.mirror-report` channel — CONVERGENCE), and does NOT echo back into window 0 (whose bus drops
# its own broadcast echo by origin — `applied == 0`, `suppressed >= 1` — the branch e08c's
# point-to-point ring drill never exercised end to end). `test_ui_mirror.cpp` proves the relay
# machinery and `uimirror_broadcast.test.ts` proves `receiveMirrored` in the fake-Shell tier; this
# proves the two live editor-cores actually converge across the real cross-origin hop.
add_executable(context_editor_shell_uimirror_smoke src/cef_shell_uimirror_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_uimirror_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_uimirror_smoke
    TEST editor-cef-smoke-shell-uimirror
    TIMEOUT 300
    BUNDLE_ID "dev.context.editor.shell.uimirror.smoke")

# --- the LIVE THIRD-PARTY PACKAGE PANEL smoke (ctest editor-cef-smoke-shell-iframe) ---------------
# M9 e13a-2: the DoD line no other tier can reach — a package panel's document really LOADS, PARSES
# and RUNS inside a sandboxed frame over the `context-ext://` scheme, under the strict panel CSP,
# while an UNMOUNTED package stays refused. A package panel lives on its own opaque origin, so
# neither the Shell nor editor-core can read it; the observable is therefore WHICH URLs the scheme
# handler served (`ext_served_urls()` / `ext_refused_urls()`, cef_shell.h), and the fixture is built
# so each URL implies a specific earlier step (see the .cpp header). Its CEF-free siblings are the
# `webui-ts-unit` `extpanel.test.ts` cases (the entry grammar + the RENDERED frame's sandbox
# attributes against real Dockview) and the `editor-shell-test_ext_scheme` suite (the resolver + the
# response policy); this proves the whole chain on the real browser. ONE live boot plus the panel's
# own module-graph round trips (index.html -> panel.css/panel.js -> its static import -> its dynamic
# import), each of which is a separate scheme-handler request.
add_executable(context_editor_shell_iframe_smoke src/cef_shell_iframe_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_iframe_smoke)
context_configure_shell_cef_smoke(
    TARGET context_editor_shell_iframe_smoke
    TEST editor-cef-smoke-shell-iframe
    TIMEOUT 240
    BUNDLE_ID "dev.context.editor.shell.iframe.smoke")

# --- the issue-#360 single-staging edge, one literal line per executable (Windows/Linux) ----------
# The libcef binary payload + resources are staged next to the exes by context_editor_cef_stage, the
# SINGLE stage target the parent directory creates. Every executable that boots from
# ${CEF_TARGET_OUT_DIR} takes this dependency instead of attaching its own POST_BUILD copy: nine
# concurrent copies of the identical payload into the identical directory is a Windows sharing
# violation waiting for ninja to schedule two links together, which is exactly what the intermittent
# `Error copying directory ... locales` failures were.
#
# ⚠ SPELLED OUT PER TARGET ON PURPOSE — see the function's ⚠ note. tools/check_cef_staging.py reads
# this edge from the SOURCES with the executable named literally; the configure-time audit in the
# parent (`_ctx_cef_stage_consumers`) re-asserts the same nine off the real build graph. macOS has no
# stage target at all (COPY_MAC_FRAMEWORK embeds the payload inside each .app), and the parent's
# macOS audit asserts one never appears.
if(OS_WINDOWS OR OS_LINUX)
    add_dependencies(context_editor_shell_cef_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_restore_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_palette_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_settings_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_multiwindow_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_tearout_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_drag_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_uimirror_smoke context_editor_cef_stage)
    add_dependencies(context_editor_shell_iframe_smoke context_editor_cef_stage)
endif()

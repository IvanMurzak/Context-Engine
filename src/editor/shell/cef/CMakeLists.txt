# src/editor/shell/cef/ — M9 e04: the windowed-OSR CEF binding + the LIVE boot smoke.
#
# The ONE CEF-dependent part of the Shell, and therefore a CI-ONLY dependency path like
# src/editor/cef/ and src/editor/gui/host/: the MSVC/Clang-ABI CEF prebuilt cannot link under the
# local Strawberry-GCC dev gate. Everything ABOVE this — compositor, layer stack, PET_POPUP, input
# arbitration, the owner loop — is CEF-free and unit-tested on all three default `build` legs.
#
# The parent (src/editor/shell/CMakeLists.txt) has already run the shared CEF-acquisition prologue
# (context_acquire_cef), so CEF_ROOT / libcef_dll_wrapper / the CEF macros are in scope here.
#
# WARNINGS POSTURE (mirrors src/editor/cef/ + src/editor/gui/host/): CEF-linking targets do NOT link
# context_warnings — CEF ships its own curated -Werror set, and the repo's -Wpedantic baseline is
# incompatible with the CEF headers + IMPLEMENT_REFCOUNTING macros. context_editor_shell, which this
# links, DOES link it, so the Shell's own code stays warnings-as-errors gated on the default matrix.
#
# The local pre-flight for this file is the `-fsyntax-only` gate the sibling README describes
# for a CI-only dependency path: compile cef_shell.cpp against the pinned CEF HEADERS (fetched +
# SHA-verified by tools/fetch_cef.py) with no prebuilt library. That catches every signature/type
# break locally instead of one CI round-trip at a time.

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

# --- the LIVE CEF boot smoke (ctest editor-cef-smoke-shell) --------------------------------------
# Boots a REAL windowless CEF browser through the REAL integrated pump and asserts its OnPaint frames
# reach the REAL compositor and are presented — the half the Session-0-safe smoke cannot cover. Runs
# in the existing `editor-cef-smoke-` family, so the per-OS `editor-cef-smoke` job's `ctest -R
# "^editor-cef-smoke-"` step picks it up with no regex change.
#
# ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained `--target` list, so this executable
# is listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md § CI).
# The regex would otherwise match a test whose executable was never built.
#
# Windows/Linux only: on those the exe self-re-execs for CEF's subprocesses, which is the model
# src/editor/cef/ and src/editor/gui/host/ already use. macOS needs the .app + per-process-type
# helper bundles + the loaded framework; that packaging is e12's, and macOS coverage here is the
# COMPILE of context_editor_cef above (the "mac/Linux compile-guarded until e12" DoD line).
if(OS_WINDOWS OR OS_LINUX)
    if(NOT TARGET libcef_lib)
        ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    endif()

    add_executable(context_editor_shell_cef_smoke src/cef_shell_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_cef_smoke)
    add_dependencies(context_editor_shell_cef_smoke libcef_dll_wrapper)
    # context_editor_cef comes FIRST, BEFORE the CEF libs. GNU ld resolves a static archive in ONE
    # left-to-right pass: it pulls an archive member only to satisfy an already-undefined symbol,
    # and any NEW undefined symbol that member introduces can only be resolved by something LATER
    # on the line. cef_shell.cpp lives inside this archive and calls into libcef, so listing the
    # archive after libcef leaves its CEF references unresolvable (`undefined reference to
    # cef_string_utf16_clear`). The sibling CEF exes (src/editor/cef/, src/editor/gui/host/) list
    # their libs in the opposite order and link fine only because THEIR CEF-calling code is a direct
    # object file of the executable, not an archive member — do not "harmonize" this line to match.
    #
    # context_editor_panels (e05d1) joins the line for the same archive-ordering reason and in the
    # same half: the smoke binds the real panel providers so the live renderer has something to
    # hydrate. It carries context_editor_shell transitively, and it introduces NO CEF references of
    # its own (every panel library is CEF-free), so its position relative to the CEF libs is not
    # load-bearing — but keeping it left of them costs nothing and matches the rule above.
    target_link_libraries(context_editor_shell_cef_smoke
        context_editor_cef context_editor_panels libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_cef_smoke PRIVATE "${CEF_ROOT}")

    # The libcef binary payload + resources are staged next to the exe by context_editor_cef_stage,
    # the SINGLE stage target the parent directory creates (issue #360). Every executable that boots
    # from ${CEF_TARGET_OUT_DIR} takes this dependency instead of attaching its own POST_BUILD copy:
    # four concurrent copies of the identical payload into the identical directory is a Windows
    # sharing violation waiting for ninja to schedule two links together, which is exactly what the
    # intermittent `Error copying directory ... locales` failures were.
    add_dependencies(context_editor_shell_cef_smoke context_editor_cef_stage)

    # --- the e05c asset root (and why this is a DEPENDENCY, not just a define) --------------------
    # The smoke now loads `context-editor://app/index.html`, so it needs editor-core's built asset
    # set to EXIST when it runs. The `editor-cef-smoke` CI job builds a hand-maintained `--target`
    # list, so nothing would otherwise build the assets there and the smoke would fail on a 404
    # that looks like a scheme bug. Expressing it as add_dependencies() makes the asset set travel
    # with this target automatically — which is exactly what keeps ci.yml's `--target` list (and
    # the profile's test.md § CI mirror of it) UNCHANGED by this task.
    add_dependencies(context_editor_shell_cef_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_cef_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui "
            "must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it so); it publishes "
            "the asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_cef_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell COMMAND context_editor_shell_cef_smoke)
    set_tests_properties(editor-cef-smoke-shell PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_cef_smoke>"
        TIMEOUT 180)

    # --- the LIVE boot -> dock -> restart -> restore smoke (ctest editor-cef-smoke-shell-restore) ---
    # M9 e05d4: proves editor-core's arrangement survives a RESTART. The exe is a thin controller that
    # runs ITSELF twice as two child processes (--ctx-restore-phase=1|2) against ONE project — phase 1
    # persists a real docking arrangement then hard-exits, phase 2 (a fresh PROCESS + a fresh
    # editor-state store LOADED from the file phase 1 wrote) reads it back and reapplies it. Two
    # processes, not two browsers in one CefInitialize: a mid-process browser teardown SEGFAULTs on the
    # Session-0 Windows runner AND ubuntu, and each single-browser phase is instead structurally
    # identical to the proven-green boot smoke above (see the .cpp header). Same `editor-cef-smoke-`
    # family + same link recipe as the boot smoke, so the per-OS `editor-cef-smoke` job's
    # `ctest -R "^editor-cef-smoke-"` step picks it up with no regex change. TIMEOUT covers two full
    # child-process boots run sequentially.
    #
    # ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained --target list, so this executable
    # is ALSO listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md
    # § CI). The regex would otherwise match a test whose executable was never built.
    add_executable(context_editor_shell_restore_smoke src/cef_shell_restore_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_restore_smoke)
    add_dependencies(context_editor_shell_restore_smoke libcef_dll_wrapper)
    # Same archive-before-libcef link order as context_editor_shell_cef_smoke above, and for the same
    # single-left-to-right-pass reason: context_editor_cef holds cef_shell.cpp's CEF references, so it
    # must precede the CEF libs. context_editor_panels and context_common are CEF-free and join on the
    # same side — context_common (the hardened std::system runner, issue #146) is what the controller
    # re-launches its two phase children with, and introduces no CEF references of its own.
    target_link_libraries(context_editor_shell_restore_smoke
        context_editor_cef context_editor_panels context_common
        libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_restore_smoke PRIVATE "${CEF_ROOT}")

    # Same single-staging dependency as the boot smoke above (issue #360).
    add_dependencies(context_editor_shell_restore_smoke context_editor_cef_stage)

    # Same webui asset dependency + compiled-in root as the boot smoke: the restore smoke loads
    # `context-editor://app/index.html`, so it needs editor-core's built asset set to exist when it
    # runs. Expressed as add_dependencies() so the asset set travels with this target automatically,
    # which keeps ci.yml's --target list from needing the webui target added alongside this one.
    add_dependencies(context_editor_shell_restore_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_restore_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui "
            "must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it so); it publishes "
            "the asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_restore_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell-restore COMMAND context_editor_shell_restore_smoke)
    set_tests_properties(editor-cef-smoke-shell-restore PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_restore_smoke>"
        TIMEOUT 360)

    # --- the LIVE command-driven palette smoke (ctest editor-cef-smoke-shell-palette) --------------
    # M9 e07d T2: boots the SAME live windowless browser as the boot smoke and then drives a scenario
    # PURELY through the palette / command dispatch (open -> filter -> execute view.panel.close), whose
    # editor.state.publish over the real bridge is the observable it asserts. Same `editor-cef-smoke-`
    # family + same link recipe (context_editor_cef holds cef_shell.cpp's CEF refs, so it precedes the
    # CEF libs — the single-left-to-right-pass reason; context_editor_panels binds the real providers so
    # the live renderer has panels to close, and introduces no CEF refs of its own) as the boot smoke,
    # so the per-OS `editor-cef-smoke` job's `ctest -R "^editor-cef-smoke-"` step picks it up with no
    # regex change.
    #
    # ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained --target list, so this executable is
    # ALSO listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md § CI).
    # The regex would otherwise match a test whose executable was never built.
    add_executable(context_editor_shell_palette_smoke src/cef_shell_palette_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_palette_smoke)
    add_dependencies(context_editor_shell_palette_smoke libcef_dll_wrapper)
    target_link_libraries(context_editor_shell_palette_smoke
        context_editor_cef context_editor_panels libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_palette_smoke PRIVATE "${CEF_ROOT}")

    # Same single-staging dependency as the boot smoke above (issue #360).
    add_dependencies(context_editor_shell_palette_smoke context_editor_cef_stage)

    # Same webui asset dependency + compiled-in root as the boot/restore smokes: the palette smoke loads
    # `context-editor://app/index.html?ctx-smoke-palette`, so it needs editor-core's built asset set to
    # exist when it runs. Expressed as add_dependencies() so the asset set travels with this target
    # automatically, keeping ci.yml's --target list from needing the webui target added alongside it.
    add_dependencies(context_editor_shell_palette_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_palette_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui "
            "must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it so); it publishes the "
            "asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_palette_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell-palette COMMAND context_editor_shell_palette_smoke)
    set_tests_properties(editor-cef-smoke-shell-palette PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_palette_smoke>"
        TIMEOUT 180)

    # --- the LIVE settings-driven smoke (ctest editor-cef-smoke-shell-settings) --------------------
    # M9 e06d T2: boots the SAME live windowless browser and drives a REAL theme change through the
    # REAL Settings panel (`?ctx-smoke-settings`), then asserts what only the SHELL can know — that it
    # PERSISTED the choice to the user config (C-F14) and preserved the rest of the document. The
    # config it writes is a TEMP file the smoke seeds and deletes, never the runner's own
    # `~/.context/config.json`. Same `editor-cef-smoke-` family + the same link recipe as the palette
    # smoke (context_editor_cef precedes the CEF libs for the single-left-to-right-pass reason;
    # context_editor_panels binds the real providers so the live renderer has a populated dock).
    #
    # ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained --target list, so this executable is
    # ALSO listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md § CI).
    # The regex would otherwise match a test whose executable was never built.
    add_executable(context_editor_shell_settings_smoke src/cef_shell_settings_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_settings_smoke)
    add_dependencies(context_editor_shell_settings_smoke libcef_dll_wrapper)
    target_link_libraries(context_editor_shell_settings_smoke
        context_editor_cef context_editor_panels libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_settings_smoke PRIVATE "${CEF_ROOT}")

    # Same single-staging dependency as the sibling smokes above (issue #360).
    add_dependencies(context_editor_shell_settings_smoke context_editor_cef_stage)

    # Same webui asset dependency + compiled-in root as the sibling smokes.
    add_dependencies(context_editor_shell_settings_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_settings_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — src/editor/webui "
            "must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it so); it publishes the "
            "asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_settings_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell-settings COMMAND context_editor_shell_settings_smoke)
    set_tests_properties(editor-cef-smoke-shell-settings PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_settings_smoke>"
        TIMEOUT 180)

    # --- the LIVE MULTI-WINDOW smoke (ctest editor-cef-smoke-shell-multiwindow) --------------------
    # M9 e10a: the three DoD lines a CEF-free test structurally cannot reach — a SECOND real CEF
    # browser booting its OWN fresh editor-core instance (proven by two distinct round-tripped
    # handshake nonces), a REAL renderer `window.open` refused by `OnBeforePopup` with NO browser
    # created, and a MID-PROCESS window destroy followed by another create. The deterministic
    # 25-cycle create/destroy version is the CEF-free `editor-shell-test_window_registry`, which runs
    # on all three default `build` legs; this proves the same discipline on the real CEF path, on the
    # Session-0 Windows runner where CE #319 faulted. Same `editor-cef-smoke-` family + the same link
    # recipe as the sibling smokes (context_editor_cef precedes the CEF libs — the
    # single-left-to-right-pass rule; context_editor_panels binds the real providers so each window's
    # editor-core has panels to hydrate, and introduces no CEF refs of its own).
    #
    # ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained --target list, so this executable
    # is ALSO listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md
    # § CI). The regex would otherwise match a test whose executable was never built.
    #
    # TIMEOUT covers THREE sequential live browser boots (window 0, window 1, and the one created
    # after the mid-process destroy) plus the popup and repaint waits — the sibling single-boot
    # smokes budget 180s each.
    add_executable(context_editor_shell_multiwindow_smoke src/cef_shell_multiwindow_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_multiwindow_smoke)
    add_dependencies(context_editor_shell_multiwindow_smoke libcef_dll_wrapper)
    target_link_libraries(context_editor_shell_multiwindow_smoke
        context_editor_cef context_editor_panels libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_multiwindow_smoke PRIVATE "${CEF_ROOT}")

    # Same single-staging dependency as the sibling smokes above (issue #360).
    add_dependencies(context_editor_shell_multiwindow_smoke context_editor_cef_stage)

    # Same webui asset dependency + compiled-in root as the sibling smokes: both windows load
    # `context-editor://app/index.html`, so editor-core's built asset set must exist when it runs.
    add_dependencies(context_editor_shell_multiwindow_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_multiwindow_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — "
            "src/editor/webui must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it "
            "so); it publishes the asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_multiwindow_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell-multiwindow
        COMMAND context_editor_shell_multiwindow_smoke)
    set_tests_properties(editor-cef-smoke-shell-multiwindow PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_multiwindow_smoke>"
        TIMEOUT 420)

    # --- the LIVE TEAR-OUT + rehome smoke (ctest editor-cef-smoke-shell-tearout) -------------------
    # M9 e10b: the DoD lines a CEF-free test cannot reach — a `window.tear-out` producing a SECOND
    # real CEF browser that boots its OWN editor-core and RESTORES the moved D6 state (proven because
    # the live renderer's `window.seed` consumes the relay's boot seed), a LOUD create-failure result
    # (03 §7), and a `window.move-to` rehome that window 0's LIVE editor-core drains on its
    # `window.rehomed` poll. Same `editor-cef-smoke-` family + the same link recipe as the multiwindow
    # smoke (context_editor_cef precedes the CEF libs — the single-left-to-right-pass rule;
    # context_editor_panels binds the real providers so each window's editor-core has panels to host).
    #
    # ⚠ "Not Run = RED" TRIPWIRE: that job builds a hand-maintained --target list, so this executable
    # is ALSO listed in ci.yml's editor-cef-smoke build step (and mirrored in the profile's test.md
    # § CI). The regex would otherwise match a test whose executable was never built.
    #
    # TIMEOUT covers TWO sequential live browser boots (window 0 + the torn-out window 1) plus the
    # seed-consumption and rehome-drain waits — the sibling single-boot smokes budget 180s each.
    add_executable(context_editor_shell_tearout_smoke src/cef_shell_tearout_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_tearout_smoke)
    add_dependencies(context_editor_shell_tearout_smoke libcef_dll_wrapper)
    target_link_libraries(context_editor_shell_tearout_smoke
        context_editor_cef context_editor_panels libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(context_editor_shell_tearout_smoke PRIVATE "${CEF_ROOT}")

    # Same single-staging dependency as the sibling smokes above (issue #360).
    add_dependencies(context_editor_shell_tearout_smoke context_editor_cef_stage)

    # Same webui asset dependency + compiled-in root as the sibling smokes: both windows load
    # `context-editor://app/index.html`, so editor-core's built asset set must exist when it runs.
    add_dependencies(context_editor_shell_tearout_smoke context_editor_webui)
    if(NOT CONTEXT_WEBUI_ASSET_DIR)
        message(FATAL_ERROR
            "context_editor_shell_tearout_smoke: CONTEXT_WEBUI_ASSET_DIR is empty — "
            "src/editor/webui must be added BEFORE src/editor/shell (src/CMakeLists.txt orders it "
            "so); it publishes the asset root as CACHE INTERNAL.")
    endif()
    target_compile_definitions(context_editor_shell_tearout_smoke PRIVATE
        "CONTEXT_WEBUI_ASSET_DIR=\"${CONTEXT_WEBUI_ASSET_DIR}\"")

    add_test(NAME editor-cef-smoke-shell-tearout
        COMMAND context_editor_shell_tearout_smoke)
    set_tests_properties(editor-cef-smoke-shell-tearout PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_tearout_smoke>"
        TIMEOUT 300)
endif()

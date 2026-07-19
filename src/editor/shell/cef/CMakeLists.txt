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
    target_link_libraries(context_editor_shell_cef_smoke
        libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} context_editor_cef)
    target_include_directories(context_editor_shell_cef_smoke PRIVATE "${CEF_ROOT}")

    # Stage the libcef binary payload + resources next to the exe so it boots from the build tree.
    COPY_FILES(context_editor_shell_cef_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"
               "${CEF_TARGET_OUT_DIR}")
    COPY_FILES(context_editor_shell_cef_smoke "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}"
               "${CEF_TARGET_OUT_DIR}")

    add_test(NAME editor-cef-smoke-shell COMMAND context_editor_shell_cef_smoke)
    set_tests_properties(editor-cef-smoke-shell PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:context_editor_shell_cef_smoke>"
        TIMEOUT 180)
endif()

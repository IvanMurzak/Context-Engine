# src/editor/cef/ — M5-F0a: CEF supply-chain + build substrate (issue #150; L-15 / L-41 / R-UI-007 /
# R-SEC-009). Promotes the spikes/cef-compositing CONTEXT_BUILD_SPIKE_CEF de-risk to a PRODUCTION build
# option: fetch + SHA-256-verify the pinned CEF prebuilt (tools/fetch_cef.py + tools/cef-prebuilt.json,
# verify-before-use fail-closed), build libcef_dll_wrapper from the pinned distribution, link a MINIMAL
# headless boot substrate, and register a ctest that boots a CEF browser subprocess headless + exits 0.
# This is the BUILD SUBSTRATE only — NO editor host, NO panels, NO RPC, NO R-EDIT-001 (that is M5-F0b /
# src/editor/gui/, which will LINK this substrate).
#
# Like V8 (src/runtime/js/) and wgpu-native (src/render/) this is a CI-ONLY dependency path: the CEF
# prebuilt is MSVC-ABI on Windows / Clang-ABI elsewhere, so the local Strawberry-GCC Windows dev gate
# CANNOT fetch/link/boot it. GATED behind CONTEXT_BUILD_GUI_CEF (default OFF, declared in
# src/CMakeLists.txt); the default 3-OS build matrix and the local dev gate never fetch CEF. The
# dedicated `cef-substrate` CI job configures -DCONTEXT_BUILD_GUI_CEF=ON on all 3 desktop OS.
#
# WARNINGS POSTURE (deliberate): unlike every other Context target this substrate does NOT link
# context_warnings. CEF ships its OWN curated warnings-as-errors flag set (SET_EXECUTABLE_TARGET_
# PROPERTIES: /W4 /WX on MSVC, -Wall -Werror [-Wextra on mac] + the -Wno-* CEF headers require) — the
# repo's -Wpedantic / -Wall -Wextra baseline is INCOMPATIBLE with CEF's headers + IMPLEMENT_REFCOUNTING
# macros. This mirrors how the spike + cefsimple build. CEF's -Werror still catches real defects in our
# boot code; third-party CEF headers are covered by CEF's own -Wno-* set.

if(NOT CONTEXT_BUILD_GUI_CEF)
    return()
endif()

# Fetch + verify the pinned CEF prebuilt, set its CACHE knobs (/MD runtime, no sandbox/ATL, the /Z7
# CEF_DEBUG_INFO_FLAG C1090-PDB-race fix), find_package(CEF), and build libcef_dll_wrapper — the
# shared acquisition prologue also used by src/editor/gui/host/ (Context-Engine#214;
# cmake/ContextCef.cmake documents why it is a MACRO, not a function, and the CACHE knobs it sets).
context_acquire_cef(context_cef cef-substrate)

set(_cef_boot_target context_cef_boot_smoke)

if(OS_WINDOWS OR OS_LINUX)
    # --- Windows / Linux: a single self-re-exec'ing executable (subprocesses reuse this exe) -------
    ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

    add_executable(${_cef_boot_target} src/cef_boot_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(${_cef_boot_target})
    add_dependencies(${_cef_boot_target} libcef_dll_wrapper)
    target_link_libraries(${_cef_boot_target} libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(${_cef_boot_target} PRIVATE "${CEF_ROOT}")

    # Stage the libcef binary payload + resources next to the exe so it boots from the build tree.
    COPY_FILES(${_cef_boot_target} "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
    COPY_FILES(${_cef_boot_target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

    add_test(NAME cef-substrate-boot COMMAND ${_cef_boot_target})
    set_tests_properties(cef-substrate-boot PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_cef_boot_target}>"
        TIMEOUT 180)

elseif(OS_MAC)
    # --- macOS: an .app bundle + per-process-type helper bundles + the embedded framework ----------
    # CEF cannot re-exec the main app as a subprocess on macOS; every subprocess type runs from a
    # dedicated "<App> Helper*.app" bundle. The framework is LOADED at runtime (CefScopedLibraryLoader),
    # never linked. This mirrors the CEF cefsimple sample exactly.
    set(_cef_helper_target context_cef_boot_smoke_Helper)
    set(_cef_helper_output_name "context_cef_boot_smoke Helper")
    set(_cef_app "${CEF_TARGET_OUT_DIR}/${_cef_boot_target}.app")

    # One helper bundle per CEF_HELPER_APP_SUFFIXES entry ("<name>:<target>:<plist>" suffixes).
    set(_cef_helper_targets "")
    foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
        string(REPLACE ":" ";" _suffixes ${_suffixes})
        list(GET _suffixes 0 _name_suffix)
        list(GET _suffixes 1 _target_suffix)

        set(_htarget "${_cef_helper_target}${_target_suffix}")
        set(_houtput "${_cef_helper_output_name}${_name_suffix}")
        add_executable(${_htarget} MACOSX_BUNDLE src/cef_boot_smoke_helper_mac.cpp)
        SET_EXECUTABLE_TARGET_PROPERTIES(${_htarget})
        add_dependencies(${_htarget} libcef_dll_wrapper)
        target_link_libraries(${_htarget} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
        target_include_directories(${_htarget} PRIVATE "${CEF_ROOT}")
        set_target_properties(${_htarget} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/helper-Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "dev.context.cef.boot.smoke.helper${_target_suffix}"
            MACOSX_BUNDLE_BUNDLE_NAME "${_houtput}"
            OUTPUT_NAME "${_houtput}")
        list(APPEND _cef_helper_targets ${_htarget})
    endforeach()

    add_executable(${_cef_boot_target} MACOSX_BUNDLE src/cef_boot_smoke.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(${_cef_boot_target})
    add_dependencies(${_cef_boot_target} libcef_dll_wrapper ${_cef_helper_targets})
    target_link_libraries(${_cef_boot_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(${_cef_boot_target} PRIVATE "${CEF_ROOT}")
    set_target_properties(${_cef_boot_target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.context.cef.boot.smoke"
        MACOSX_BUNDLE_BUNDLE_NAME "${_cef_boot_target}")

    # Embed the CEF framework (with the Versions/A symlink layout) into Contents/Frameworks.
    COPY_MAC_FRAMEWORK(${_cef_boot_target} "${CEF_BINARY_DIR}" "${_cef_app}")

    # Copy each helper .app into the main app's Contents/Frameworks.
    foreach(_htarget ${_cef_helper_targets})
        add_custom_command(TARGET ${_cef_boot_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:${_htarget}>"
                    "${_cef_app}/Contents/Frameworks/$<TARGET_BUNDLE_DIR_NAME:${_htarget}>"
            VERBATIM)
    endforeach()

    add_test(NAME cef-substrate-boot
             COMMAND "${_cef_app}/Contents/MacOS/${_cef_boot_target}")
    set_tests_properties(cef-substrate-boot PROPERTIES TIMEOUT 180)
endif()

# src/editor/gui/host/ — M5-F0b: the CEF editor HOST + the per-OS editor-cef-smoke boot (issue #152;
# R-EDIT-001 / R-UI-007 / L-41 / R-SEC-007). Boots CEF headless, registers the built-in placeholder
# panel through the R-EDIT-001 extension contract, attaches the capability-scoped bridge + runs one
# command, selects the L-41 surface mode, renders the placeholder panel, and exits 0.
#
# CI-ONLY dependency path (behind CONTEXT_BUILD_GUI_CEF, the same toggle the F0a substrate uses): the
# MSVC/Clang-ABI CEF prebuilt cannot link under the local Strawberry-GCC dev gate, so this target is
# built + booted ONLY by the editor-cef-smoke CI job on all 3 desktop OS. The default matrix + local
# dev gate never enter it (src/editor/gui/CMakeLists.txt only add_subdirectory(host) under the toggle).
#
# SHARED CEF ACQUISITION (deliberate): src/editor/cef/ (processed FIRST — it is add_subdirectory'd
# above this in src/CMakeLists.txt) already fetches + verifies the pinned CEF prebuilt and builds the
# libcef_dll_wrapper + libcef_lib targets. This file RE-RUNS the acquisition prologue (idempotent —
# fetch_cef.py has a stamp; the cache knobs are CACHE ... FORCE; find_package(CEF) re-populates the CEF
# macros/vars in THIS directory scope) but GUARDS the two target-creating calls (add_subdirectory
# libcef_dll / ADD_LOGICAL_TARGET) with `if(NOT TARGET ...)` so processing both subdirs in one
# configure never double-defines libcef_dll_wrapper. A future cleanup could hoist this into a shared
# cmake/ContextCef.cmake module consumed by both editor/cef and editor/gui/host (dedup opportunity).
#
# WARNINGS POSTURE (mirrors src/editor/cef/): this host does NOT link context_warnings — CEF ships its
# own curated -Werror set (SET_EXECUTABLE_TARGET_PROPERTIES), and the repo's -Wpedantic baseline is
# incompatible with the CEF headers + IMPLEMENT_REFCOUNTING macros. The three headless gui libs it
# links (uitree/contract/compositor) link context_warnings PRIVATE, so no -Wpedantic leaks here; they
# are ALSO fully warnings-as-errors gated on the default matrix where they build under GCC.

if(NOT CONTEXT_BUILD_GUI_CEF)
    return()
endif()

# --- resolve the host triple (must match a tools/cef-prebuilt.json key; identical to editor/cef) ---
set(_cef_triple "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    if(NOT MSVC)
        message(FATAL_ERROR "CONTEXT_BUILD_GUI_CEF on Windows needs the MSVC-ABI CEF prebuilt; build \
with MSVC (the editor-cef-smoke CI job does — VsDevCmd + Ninja + cl on the self-hosted runner).")
    endif()
    set(_cef_triple "x86_64-pc-windows-msvc")
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_cef_triple "aarch64-apple-darwin")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
    set(_cef_triple "x86_64-unknown-linux-gnu")
else()
    message(FATAL_ERROR "CONTEXT_BUILD_GUI_CEF: no CEF prebuilt pinned for \
${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} (tools/cef-prebuilt.json has Linux-x64 / macOS-ARM64 / \
Win-x64).")
endif()

# --- fetch + verify the pinned CEF prebuilt (idempotent; same dest as editor/cef so CEF_ROOT matches)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_cef_manifest "${CMAKE_SOURCE_DIR}/../tools/cef-prebuilt.json")
set(_cef_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_cef.py")
set(_cef_dest "${CMAKE_BINARY_DIR}/_cef/${_cef_triple}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_cef_fetch}"
            --manifest "${_cef_manifest}" --triple "${_cef_triple}" --dest "${_cef_dest}"
    RESULT_VARIABLE _cef_rc OUTPUT_VARIABLE _cef_out ERROR_VARIABLE _cef_err)
if(NOT _cef_rc EQUAL 0)
    message(FATAL_ERROR
        "context_gui_host: CEF prebuilt fetch/verify FAILED (rc=${_cef_rc}) — refusing to build "
        "against an unverified CEF (R-SEC-009 fail-closed).\nstdout: ${_cef_out}\nstderr: ${_cef_err}")
endif()

# --- consume the CEF distribution's own CMake (idempotent cache knobs; mirrors editor/cef) ----------
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
set(USE_ATL OFF CACHE BOOL "" FORCE)
set(CEF_ROOT "${_cef_dest}" CACHE INTERNAL "Pinned CEF distribution root")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
SET_CEF_TARGET_OUT_DIR()

# The shared libcef_dll_wrapper target is built by src/editor/cef (processed first). Guard so it is
# never defined twice when both subdirs are processed in one CONTEXT_BUILD_GUI_CEF configure.
if(NOT TARGET libcef_dll_wrapper)
    add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll_wrapper")
endif()

set(_host_target context_gui_host)
# The headless gui libs this host is BUILT ON (R-EDIT-001: every built-in panel built on the contract).
set(_host_gui_libs context_gui_uitree context_gui_contract context_gui_compositor)

if(OS_WINDOWS OR OS_LINUX)
    # --- Windows / Linux: a single self-re-exec'ing executable (subprocesses reuse this exe) --------
    if(NOT TARGET libcef_lib)
        ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
    endif()

    add_executable(${_host_target} src/editor_host.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(${_host_target})
    add_dependencies(${_host_target} libcef_dll_wrapper)
    target_link_libraries(${_host_target}
        libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS} ${_host_gui_libs})
    target_include_directories(${_host_target} PRIVATE "${CEF_ROOT}")

    # Stage the libcef binary payload + resources next to the exe so it boots from the build tree.
    COPY_FILES(${_host_target} "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")
    COPY_FILES(${_host_target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

    add_test(NAME editor-cef-smoke-boot COMMAND ${_host_target})
    set_tests_properties(editor-cef-smoke-boot PROPERTIES
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_host_target}>"
        TIMEOUT 180)

elseif(OS_MAC)
    # --- macOS: an .app bundle + per-process-type helper bundles + the embedded framework -----------
    # (Mirrors src/editor/cef/CMakeLists.txt exactly — CEF cannot re-exec the main app as a
    # subprocess on macOS; every subprocess type runs from a dedicated "<App> Helper*.app" bundle and
    # the framework is LOADED at runtime via CefScopedLibraryLoader, never linked.)
    set(_host_helper_target context_gui_host_Helper)
    set(_host_helper_output_name "context_gui_host Helper")
    set(_host_app "${CEF_TARGET_OUT_DIR}/${_host_target}.app")

    set(_host_helper_targets "")
    foreach(_suffixes ${CEF_HELPER_APP_SUFFIXES})
        string(REPLACE ":" ";" _suffixes ${_suffixes})
        list(GET _suffixes 0 _name_suffix)
        list(GET _suffixes 1 _target_suffix)

        set(_htarget "${_host_helper_target}${_target_suffix}")
        set(_houtput "${_host_helper_output_name}${_name_suffix}")
        add_executable(${_htarget} MACOSX_BUNDLE src/editor_host_helper_mac.cpp)
        SET_EXECUTABLE_TARGET_PROPERTIES(${_htarget})
        add_dependencies(${_htarget} libcef_dll_wrapper)
        target_link_libraries(${_htarget} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
        target_include_directories(${_htarget} PRIVATE "${CEF_ROOT}")
        set_target_properties(${_htarget} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/helper-Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "dev.context.gui.host.helper${_target_suffix}"
            MACOSX_BUNDLE_BUNDLE_NAME "${_houtput}"
            OUTPUT_NAME "${_houtput}")
        list(APPEND _host_helper_targets ${_htarget})
    endforeach()

    add_executable(${_host_target} MACOSX_BUNDLE src/editor_host.cpp)
    SET_EXECUTABLE_TARGET_PROPERTIES(${_host_target})
    add_dependencies(${_host_target} libcef_dll_wrapper ${_host_helper_targets})
    target_link_libraries(${_host_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS} ${_host_gui_libs})
    target_include_directories(${_host_target} PRIVATE "${CEF_ROOT}")
    set_target_properties(${_host_target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.context.gui.host"
        MACOSX_BUNDLE_BUNDLE_NAME "${_host_target}")

    COPY_MAC_FRAMEWORK(${_host_target} "${CEF_BINARY_DIR}" "${_host_app}")

    foreach(_htarget ${_host_helper_targets})
        add_custom_command(TARGET ${_host_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "$<TARGET_BUNDLE_DIR:${_htarget}>"
                    "${_host_app}/Contents/Frameworks/$<TARGET_BUNDLE_DIR_NAME:${_htarget}>"
            VERBATIM)
    endforeach()

    add_test(NAME editor-cef-smoke-boot
             COMMAND "${_host_app}/Contents/MacOS/${_host_target}")
    set_tests_properties(editor-cef-smoke-boot PROPERTIES TIMEOUT 180)
endif()

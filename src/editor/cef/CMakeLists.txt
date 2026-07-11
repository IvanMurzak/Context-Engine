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

# --- resolve the host triple (must match a tools/cef-prebuilt.json key) ------------------------
set(_cef_triple "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    if(NOT MSVC)
        message(FATAL_ERROR "CONTEXT_BUILD_GUI_CEF on Windows needs the MSVC-ABI CEF prebuilt; build \
with MSVC (the cef-substrate CI job does — VsDevCmd + Ninja + cl on the self-hosted runner).")
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

# --- fetch + verify the pinned CEF prebuilt (verify-before-use, fail-closed) --------------------
# FindPython3 resolves the real interpreter (incl. the `py` launcher on the self-hosted Windows
# runner), robust to the python3-vs-py alias difference — same as src/runtime/js/.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(_cef_manifest "${CMAKE_SOURCE_DIR}/../tools/cef-prebuilt.json")
set(_cef_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_cef.py")
set(_cef_dest "${CMAKE_BINARY_DIR}/_cef/${_cef_triple}")
message(STATUS "context_cef: fetching + verifying pinned CEF prebuilt for ${_cef_triple}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_cef_fetch}"
            --manifest "${_cef_manifest}" --triple "${_cef_triple}" --dest "${_cef_dest}"
    RESULT_VARIABLE _cef_rc OUTPUT_VARIABLE _cef_out ERROR_VARIABLE _cef_err)
if(NOT _cef_rc EQUAL 0)
    message(FATAL_ERROR
        "context_cef: CEF prebuilt fetch/verify FAILED (rc=${_cef_rc}) — refusing to build against "
        "an unverified CEF (R-SEC-009 fail-closed).\nstdout: ${_cef_out}\nstderr: ${_cef_err}")
endif()
string(STRIP "${_cef_out}" _cef_out)
message(STATUS "context_cef: ${_cef_out}")

# --- consume the CEF distribution's own CMake (FindCEF + libcef_dll_wrapper) --------------------
# The pinned distribution ships FindCEF.cmake / cef_variables.cmake / cef_macros.cmake — the sanctioned
# way to build against it. Knobs go in the cache BEFORE find_package(CEF):
#   * USE_SANDBOX OFF — the boot substrate runs no_sandbox (the M138+ Windows sandbox bootstrap.exe
#     launch model is an F0b packaging concern, L-41 / ROADMAP §1 M5 note); OFF also drops the
#     CEF_USE_SANDBOX/BOOTSTRAP defines + cef_sandbox link.
#   * CEF_RUNTIME_LIBRARY_FLAG /MD — match this repo's dynamic MSVC runtime (CEF defaults to /MT, which
#     LNK2038-clashes), mirroring the spike.
#   * USE_ATL OFF — the boot substrate needs no ATL; keep the toolchain requirement minimal.
set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
set(USE_ATL OFF CACHE BOOL "" FORCE)
set(CEF_ROOT "${_cef_dest}" CACHE INTERNAL "Pinned CEF distribution root")
# The CEF binary distribution ships its OWN CMake as a MODULE package (cmake/FindCEF.cmake), NOT a
# CONFIG package — so CMake can only locate FindCEF.cmake if ${CEF_ROOT}/cmake is on CMAKE_MODULE_PATH
# BEFORE find_package(CEF). (FindCEF.cmake itself re-appends that dir for its internal cef_variables /
# cef_macros includes, but that runs only AFTER the module is found.) Mirrors spikes/cef-compositing.
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# Output binaries into a config subdir (CEF's convention; also where libcef + resources are staged).
SET_CEF_TARGET_OUT_DIR()

# Build libcef_dll_wrapper from the pinned distribution (its OWN target with CEF's warning posture).
add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll_wrapper")

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

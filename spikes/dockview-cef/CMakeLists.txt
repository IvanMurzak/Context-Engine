# spikes/dockview-cef — M9 spike s1: Dockview v7 ratification inside CEF 149 under strict CSP +
# custom scheme + sandboxed iframes. THROWAWAY proof code (ratified DECISIONS + FINDINGS.md are the
# deliverable). Reuses spikes/cef-compositing's CEF-149 acquisition/gating idioms.
#
# GATING (mirrors cef-compositing so CI stays a pure configure-exercise):
#   * Entered under CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_DOCKVIEW (see ../../src/CMakeLists.txt).
#   * With CONTEXT_BUILD_SPIKE_DOCKVIEW=OFF (the CI bench job's CONTEXT_BUILD_SPIKES=ON) this file
#     early-returns registering NOTHING — identical to cef-compositing. CI is untouched, no download.
#   * With CONTEXT_BUILD_SPIKE_DOCKVIEW=ON it registers the CEF-FREE python probe self-check
#     (the R-QA-013 runnable ctest, headless Chromium — needs no CEF), and, on Windows+MSVC ONLY,
#     builds the CEF-149 host (the ~162 MB pinned CEF download, NEVER in CI, like cef-compositing).
#
# Local runs:
#   # 1) the headless-Chromium probe self-check (measures probes 1-4,6; no CEF, any OS):
#   cmake -S src --preset dev -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON && ctest --preset dev -R context-spike-dockview
#   # 2) the CEF-149 host (probe-1 custom-scheme + probe-5 process-isolation residuals; Win/MSVC):
#   cmake -S src -B src/build/dvspike -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON
#   cmake --build src/build/dvspike --config Release --target dockview-cef-spike
#   src/build/dvspike/spikes/dockview-cef/Release/dockview-cef-spike.exe

if(NOT CONTEXT_BUILD_SPIKE_DOCKVIEW)
    message(STATUS "[dockview-cef] CONTEXT_BUILD_SPIKE_DOCKVIEW=OFF -- spike skipped "
                   "(configure-only exercise; the CEF binary distribution is never downloaded in CI).")
    return()
endif()

# ---- Runnable ctest self-check (R-QA-013): CEF-FREE headless-Chromium probe driver ----------
# Measures probes 1-4 + 6 in a real Chromium engine. --allow-skip => exit 0 (SKIP) where no
# Chromium-family browser exists, so the self-check never falsely reds a host without one.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME context-spike-dockview-probes
             COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_probes.py"
                     --allow-skip --timeout 90)
    set_tests_properties(context-spike-dockview-probes PROPERTIES TIMEOUT 150 LABELS "spike;dockview")
else()
    message(STATUS "[dockview-cef] Python3 not found -- the probe self-check ctest is not registered.")
endif()

# ---- CEF-149 host (custom-scheme + process-isolation residuals): Windows/MSVC only, never CI ----
if(NOT (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(STATUS "[dockview-cef] CEF host builds on 64-bit Windows+MSVC only; skipping the exe on "
                   "this toolchain (the python probe self-check above is still registered).")
    return()
endif()

include(FetchContent)

# CEF pin: identical to spikes/cef-compositing + tools/cef-prebuilt.json (149.0.6 / chromium
# 149.0.7827.201, windows64 minimal, Spotify CDN — BSD-3-Clause, in tools/license-allowlist.json).
set(_cef_url "https://cef-builds.spotifycdn.com/cef_binary_149.0.6%2Bg0d0eeb6%2Bchromium-149.0.7827.201_windows64_minimal.tar.bz2")
set(_cef_sha1 "fe8f461b743f03dc640e998ae08264407d8bc2c9")

set(CONTEXT_SPIKE_CEF_ARCHIVE "" CACHE FILEPATH
    "Pre-downloaded CEF .tar.bz2 (skips the ~162 MB CDN download; same SHA1 enforced)")
if(CONTEXT_SPIKE_CEF_ARCHIVE)
    set(_cef_fetch_url "${CONTEXT_SPIKE_CEF_ARCHIVE}")
else()
    set(_cef_fetch_url "${_cef_url}")
endif()

# CEF CACHE knobs must be set BEFORE MakeAvailable (the CEF distro's own CMakeLists defines
# libcef_dll_wrapper): /MD to match this repo's dynamic MSVC runtime; sandbox OFF (the host runs
# CefSettings.no_sandbox=1 — the app-packaging sandbox model is orthogonal to this spike).
set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "CEF MSVC runtime flag" FORCE)
option(USE_SANDBOX "" OFF)

FetchContent_Declare(cef_dockview
    URL "${_cef_fetch_url}"
    URL_HASH SHA1=${_cef_sha1}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(cef_dockview)

set(CEF_ROOT "${cef_dockview_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_executable(dockview-cef-spike src/main.cpp)
target_compile_definitions(dockview-cef-spike PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# SYSTEM keeps the repo /W4 /WX baseline on OUR sources while exempting CEF's own headers.
target_include_directories(dockview-cef-spike SYSTEM PRIVATE "${CEF_ROOT}")
target_include_directories(dockview-cef-spike PRIVATE src)
target_link_libraries(dockview-cef-spike PRIVATE context_warnings libcef_lib libcef_dll_wrapper)
add_dependencies(dockview-cef-spike libcef_dll_wrapper)

# Stage the CEF runtime payload + the web/ harness next to the exe (the exe serves web/ over the
# context-editor:// scheme via a relative path from its own dir).
set(_out_dir "$<TARGET_FILE_DIR:dockview-cef-spike>")
add_custom_command(TARGET dockview-cef-spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Release"   "${_out_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_ROOT}/Resources" "${_out_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${_out_dir}/web"
    COMMENT "dockview-cef-spike: staging CEF binaries/resources + web/ harness")

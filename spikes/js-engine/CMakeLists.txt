# spikes/js-engine — §2d embedded-JS-engine spike (DESIGN-DECISIONS.md §2d; R-LANG-008/009/011,
# R-OBS-005). THROWAWAY proof code. Built ONLY when CONTEXT_BUILD_SPIKES=ON — the default CI
# matrix never configures or builds this directory.
#
# Backends behind one seam (include/ctx/js_engine.hpp):
#   * QuickJS (quickjs-ng 0.15.1, MIT) — interpreter-class. Same upstream+version the pinned
#     vcpkg baseline's `quickjs-ng` port carries (declared in vcpkg.json feature "spikes" for the
#     license gate); acquired here via pinned FetchContent so the spike builds without a vcpkg
#     toolchain, matching this repo's vcpkg-optional skeleton.
#   * V8 (13.0.245.25, BSD-3-Clause) — JIT-class. PREBUILT NuGet monolith (pmed/v8-nuget,
#     win-x64/MSVC v143 only). Deliberate, recorded deviation from L-42 from-source: the vcpkg
#     baseline's `v8` port is 9.1.269.39 (2021) and its gn/python2 from-source build is exactly
#     the embedding-cost data point FINDINGS.md records. L-42 governs shipped deps, not
#     throwaway spikes.

include(FetchContent)

option(CONTEXT_SPIKE_JSENGINE_QUICKJS "js-engine spike: build the QuickJS backend" ON)
# The V8 prebuilt is MSVC-v143-only: its import libs use MSVC name mangling, so GCC/Clang-GNU
# on Windows cannot link it (measured: MinGW ld fails with undefined v8:: references).
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND MSVC)
    option(CONTEXT_SPIKE_JSENGINE_V8 "js-engine spike: build the V8 backend (prebuilt NuGet, win-x64 MSVC only)" ON)
else()
    option(CONTEXT_SPIKE_JSENGINE_V8 "js-engine spike: build the V8 backend (prebuilt NuGet, win-x64 MSVC only)" OFF)
endif()

set(SPIKE_SOURCES src/bench_main.cpp)

# --- QuickJS backend -----------------------------------------------------------------------
if(CONTEXT_SPIKE_JSENGINE_QUICKJS)
    find_package(qjs CONFIG QUIET)  # vcpkg toolchain path (manifest feature "spikes")
    if(NOT qjs_FOUND)
        set(QJS_BUILD_EXAMPLES OFF)
        set(QJS_BUILD_LIBC OFF)
        set(BUILD_TESTING OFF)
        FetchContent_Declare(quickjs_ng
            URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.15.1.tar.gz
            URL_HASH SHA256=c4e813951b7c46845096a948e978c620b11ab4cf5fd622ca09c727ec31f42623
            SYSTEM)
        FetchContent_MakeAvailable(quickjs_ng)
    endif()
    list(APPEND SPIKE_SOURCES src/engine_quickjs.cpp)
endif()

# --- V8 backend (prebuilt NuGet monolith) ---------------------------------------------------
if(CONTEXT_SPIKE_JSENGINE_V8)
    if(NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND MSVC))
        message(FATAL_ERROR "CONTEXT_SPIKE_JSENGINE_V8 uses the win-x64 MSVC-v143 NuGet package only "
                            "(MSVC import libs; not linkable from GCC/MinGW).")
    endif()
    set(CONTEXT_SPIKE_V8_DIR "" CACHE PATH
        "Pre-extracted v8-v143-x64 + v8.redist-v143-x64 NuGet roots (dev under /v8, redist under /v8redist); empty = download")
    if(CONTEXT_SPIKE_V8_DIR)
        set(_v8_dev "${CONTEXT_SPIKE_V8_DIR}/v8")
        set(_v8_redist "${CONTEXT_SPIKE_V8_DIR}/v8redist")
    else()
        set(_v8_nuget_root "${CMAKE_BINARY_DIR}/_deps/v8-nuget")
        set(_v8_dev "${_v8_nuget_root}/v8")
        set(_v8_redist "${_v8_nuget_root}/v8redist")
        if(NOT EXISTS "${_v8_dev}/include/v8.h")
            message(STATUS "js-engine spike: downloading v8-v143-x64 13.0.245.25 NuGet packages (~53 MB)")
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/v8-v143-x64/13.0.245.25/v8-v143-x64.13.0.245.25.nupkg"
                "${_v8_nuget_root}/v8.nupkg"
                EXPECTED_HASH SHA256=ecc0c861324daa683dae32be3f6e517af9741b2174ce48a98999a5c3a1b902ed
                STATUS _dl_status)
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/v8.redist-v143-x64/13.0.245.25/v8.redist-v143-x64.13.0.245.25.nupkg"
                "${_v8_nuget_root}/v8redist.nupkg"
                EXPECTED_HASH SHA256=6ae157305deb3e0136fc66e0d72df4904117d8fb2b10a6b492150e306ee61055
                STATUS _dl_status2)
            file(ARCHIVE_EXTRACT INPUT "${_v8_nuget_root}/v8.nupkg" DESTINATION "${_v8_dev}")
            file(ARCHIVE_EXTRACT INPUT "${_v8_nuget_root}/v8redist.nupkg" DESTINATION "${_v8_redist}")
        endif()
    endif()

    add_library(ctx_spike_v8 INTERFACE)
    target_include_directories(ctx_spike_v8 SYSTEM INTERFACE "${_v8_dev}/include")
    # Defines mirror the package's .props (must match how the DLLs were built).
    target_compile_definitions(ctx_spike_v8 INTERFACE
        V8_31BIT_SMIS_ON_64BIT_ARCH
        V8_COMPRESS_POINTERS
        V8_ENABLE_SANDBOX
        $<$<CONFIG:Debug>:V8_ENABLE_CHECKS>)
    set(_v8_libcfg $<IF:$<CONFIG:Debug>,Debug,Release>)
    target_link_libraries(ctx_spike_v8 INTERFACE
        "${_v8_dev}/lib/${_v8_libcfg}/v8.dll.lib"
        "${_v8_dev}/lib/${_v8_libcfg}/v8_libbase.dll.lib"
        "${_v8_dev}/lib/${_v8_libcfg}/v8_libplatform.dll.lib"
        dbghelp shlwapi winmm)
    list(APPEND SPIKE_SOURCES src/engine_v8.cpp)
endif()

# --- the bench executable --------------------------------------------------------------------
add_executable(context-spike-jsengine ${SPIKE_SOURCES})
target_include_directories(context-spike-jsengine PRIVATE include)
target_link_libraries(context-spike-jsengine PRIVATE context_warnings)
if(MSVC)
    # /external:W0 — keep /W4 /WX for spike code while third-party headers stay quiet.
    # /Zc:__cplusplus — v8config.h gates on the real __cplusplus value, which MSVC lies about
    # by default.
    target_compile_options(context-spike-jsengine PRIVATE /external:W0 /Zc:__cplusplus)
endif()

if(CONTEXT_SPIKE_JSENGINE_QUICKJS)
    target_compile_definitions(context-spike-jsengine PRIVATE CTX_SPIKE_HAS_QUICKJS)
    target_link_libraries(context-spike-jsengine PRIVATE qjs)
endif()

if(CONTEXT_SPIKE_JSENGINE_V8)
    target_compile_definitions(context-spike-jsengine PRIVATE CTX_SPIKE_HAS_V8)
    target_link_libraries(context-spike-jsengine PRIVATE ctx_spike_v8)
    add_custom_command(TARGET context-spike-jsengine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${_v8_redist}/lib/${_v8_libcfg}"
            "$<TARGET_FILE_DIR:context-spike-jsengine>"
        COMMENT "js-engine spike: staging V8 runtime DLLs + icudtl.dat next to the executable")
endif()

# Smoke test — registered only when the spike is built (CI's default matrix never sees it).
add_test(NAME context-spike-jsengine-quickjs-smoke
         COMMAND context-spike-jsengine --engine=quickjs --bench=zerocopy)

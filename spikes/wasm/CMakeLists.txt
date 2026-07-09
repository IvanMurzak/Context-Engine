# spikes/wasm — L-8 / R-LANG-003 WASM native-tier spike (R-LANG-005/008, R-KERNEL-001,
# R-SEC-001/002). THROWAWAY proof code. Built when CONTEXT_BUILD_SPIKES=ON or via the
# stand-alone CONTEXT_BUILD_SPIKE_WASM=ON toggle (src/CMakeLists.txt) — the default CI matrix
# never configures or builds this directory. NO vcpkg dependency: both runtimes are acquired
# here (recorded L-42 deviations for a throwaway spike, license-cleared in
# tools/license-allowlist.json; see FINDINGS.md "Deviations").
#
# Runtimes behind one seam (include/ctx/wasm_engine.hpp):
#   * wasmtime 46.0.1 (Apache-2.0 WITH LLVM-exception) — Cranelift JIT + Winch baseline-JIT +
#     Pulley interpreter. PREBUILT C-API archive (hash-pinned GitHub release; a from-source
#     build is a full Rust toolchain — exactly the embedding-cost data point FINDINGS records).
#   * WAMR 2.4.5 (Apache-2.0 WITH LLVM-exception) — fast-interpreter + AOT; the iOS-class
#     no-JIT answer. Built FROM SOURCE in-tree via the documented runtime_lib.cmake embedding
#     recipe (pinned FetchContent tarball; ~1 min build).
#   * wamrc 2.4.5 (prebuilt, hash-pinned) — the WAMR AOT compiler; precompiles the guest
#     module to module.aot at build time where a prebuilt exists (x86_64 win/linux/macos-intel;
#     macOS arm64 has NO prebuilt → the AOT leg is honestly gated off there). Codegen is
#     PINNED to the x86-64-v2 baseline (see _ctx_wamrc_flags below + aot_pin_guard.cmake) —
#     host-CPU default codegen was the Context-Engine#24 CI SIGILL flake.
#
# The guest module (guest/module.wasm, ~2 KB) is COMMITTED — freestanding wasm32, zero imports;
# rebuilding it needs a wasm32-capable clang (guest/build_module.py records the pinned
# toolchain + flags). Building/running the spike itself needs no wasm toolchain anywhere.

include(FetchContent)

# Resilient prebuilt downloader (retry/backoff + fail-closed SHA verify, Context-Engine#129).
# Guarded include so this spike works both standalone and via the src/ spikes preset.
if(NOT COMMAND context_download)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ContextDownload.cmake")
endif()

option(CONTEXT_SPIKE_WASM_WASMTIME "wasm spike: build the wasmtime backend (prebuilt C API)" ON)
option(CONTEXT_SPIKE_WASM_WAMR "wasm spike: build the WAMR backend (from source)" ON)

# AOT leg: gated on a wamrc prebuilt existing for this host (x86_64 Windows/Linux/macOS-Intel).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64" AND (WIN32 OR CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin"))
    set(_ctx_wamrc_available ON)
else()
    set(_ctx_wamrc_available OFF)
endif()
option(CONTEXT_SPIKE_WASM_AOT
       "wasm spike: precompile the guest with a prebuilt wamrc and bench WAMR AOT mode"
       ${_ctx_wamrc_available})

set(SPIKE_SOURCES src/bench_main.cpp)

# --- wasmtime backend (prebuilt C API, hash-pinned per platform) -----------------------------
if(CONTEXT_SPIKE_WASM_WASMTIME)
    set(_wasmtime_ver "46.0.1")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND MSVC)
        set(_wasmtime_plat "x86_64-windows")
        set(_wasmtime_ext "zip")
        set(_wasmtime_sha "5f09e0eb900d83afbbc2dd71183ff3de37ae99c79397acd22c0524f6f32361b8")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_wasmtime_plat "aarch64-macos")
        set(_wasmtime_ext "tar.xz")
        set(_wasmtime_sha "f2a33c780715cbe740ca888ed3ee38a197d970800ca1b117b338db3aed819333")
    elseif(APPLE)
        set(_wasmtime_plat "x86_64-macos")
        set(_wasmtime_ext "tar.xz")
        set(_wasmtime_sha "c376c153dfd13f281f7f6cf1f9d5ad18b283ab093cf6ae9142612f4f9915f707")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(_wasmtime_plat "x86_64-linux")
        set(_wasmtime_ext "tar.xz")
        set(_wasmtime_sha "4b7e7acf08467de6147f11c8fb71c4db7623035064c096976aa969d54171fed4")
    else()
        message(FATAL_ERROR "wasm spike: no pinned wasmtime C-API prebuilt for this platform; "
                            "set CONTEXT_SPIKE_WASM_WASMTIME=OFF")
    endif()

    set(CONTEXT_SPIKE_WASMTIME_DIR "" CACHE PATH
        "Pre-extracted wasmtime C-API root (contains include/ + lib/); empty = download")
    if(CONTEXT_SPIKE_WASMTIME_DIR)
        set(_wasmtime_root "${CONTEXT_SPIKE_WASMTIME_DIR}")
    else()
        set(_wasmtime_name "wasmtime-v${_wasmtime_ver}-${_wasmtime_plat}-c-api")
        set(_wasmtime_dl "${CMAKE_BINARY_DIR}/_deps/wasmtime-capi")
        set(_wasmtime_root "${_wasmtime_dl}/${_wasmtime_name}")
        if(NOT EXISTS "${_wasmtime_root}/include/wasmtime.h")
            message(STATUS "wasm spike: downloading wasmtime ${_wasmtime_ver} C API (${_wasmtime_plat})")
            context_download(
                URL "https://github.com/bytecodealliance/wasmtime/releases/download/v${_wasmtime_ver}/${_wasmtime_name}.${_wasmtime_ext}"
                PATH "${_wasmtime_dl}/${_wasmtime_name}.${_wasmtime_ext}"
                EXPECTED_SHA256 "${_wasmtime_sha}"
                DESCRIPTION "wasmtime ${_wasmtime_ver} C API (${_wasmtime_plat})")
            file(ARCHIVE_EXTRACT
                INPUT "${_wasmtime_dl}/${_wasmtime_name}.${_wasmtime_ext}"
                DESTINATION "${_wasmtime_dl}")
        endif()
    endif()

    add_library(ctx_spike_wasmtime INTERFACE)
    target_include_directories(ctx_spike_wasmtime SYSTEM INTERFACE "${_wasmtime_root}/include")
    if(WIN32)
        # Shared: link the import lib, stage wasmtime.dll next to the exe (21.6 MB — the
        # dynamic-footprint number in FINDINGS; lib/wasmtime.lib static is 80 MB MSVC-archive).
        target_link_libraries(ctx_spike_wasmtime INTERFACE "${_wasmtime_root}/lib/wasmtime.dll.lib")
    else()
        # Static: deterministic on CI (no rpath juggling); required system libs per the
        # wasmtime C-API embedding docs.
        find_package(Threads REQUIRED)
        target_link_libraries(ctx_spike_wasmtime INTERFACE
            "${_wasmtime_root}/lib/libwasmtime.a" Threads::Threads ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(ctx_spike_wasmtime INTERFACE m)
        endif()
    endif()
    list(APPEND SPIKE_SOURCES src/engine_wasmtime.cpp)
endif()

# --- WAMR backend (from source — the documented runtime_lib.cmake embedding recipe) ----------
if(CONTEXT_SPIKE_WASM_WAMR)
    # SOURCE_SUBDIR points at a dir with no CMakeLists.txt: populate WITHOUT add_subdirectory
    # (WAMR's root CMakeLists carries install rules + '-lm -ldl' link items that break MSVC).
    FetchContent_Declare(wamr
        URL https://github.com/bytecodealliance/wasm-micro-runtime/archive/refs/tags/WAMR-2.4.5.tar.gz
        URL_HASH SHA256=1ab09d51099f276ca4a1d6629f6b589aab2bd0caa01445e05031a4bed22c199b
        SOURCE_SUBDIR doc
        SYSTEM)
    FetchContent_MakeAvailable(wamr)

    set(WAMR_ROOT_DIR ${wamr_SOURCE_DIR})
    # WAMR is C (plus one invoke-native asm file) and its config_common.cmake try_runs a .c
    # probe. The C language is enabled at the TOP level (src/CMakeLists.txt) when this spike
    # is on: enable_language(C) from inside a subdirectory breaks C-feature detection under a
    # chainloaded vcpkg toolchain ("No known features for C compiler" at generate time), while
    # a missing C language makes Ninja on linux/macos silently DROP every .c source from the
    # target (all wasm_runtime_* symbols gone at link; MSBuild masks both). ASM stays here —
    # it has no feature detection, so subdirectory enabling is safe.
    if(WIN32)
        set(WAMR_BUILD_PLATFORM "windows")
        enable_language(ASM_MASM)
    else()
        if(APPLE)
            set(WAMR_BUILD_PLATFORM "darwin")
        else()
            set(WAMR_BUILD_PLATFORM "linux")
        endif()
        enable_language(ASM)
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(WAMR_BUILD_TARGET "AARCH64")
    else()
        set(WAMR_BUILD_TARGET "X86_64")
    endif()
    set(WAMR_BUILD_INTERP 1)       # the no-JIT story (R-LANG-005)
    set(WAMR_BUILD_FAST_INTERP 1)  # WAMR's production interpreter mode
    set(WAMR_BUILD_AOT 1)          # AOT image loader (wamrc output); adds no compiler
    set(WAMR_BUILD_JIT 0)          # LLVM JIT: out of spike scope (needs an LLVM build)
    set(WAMR_BUILD_FAST_JIT 0)
    set(WAMR_BUILD_LIBC_BUILTIN 0) # guest is freestanding: ZERO imports, no libc at all
    set(WAMR_BUILD_LIBC_WASI 0)    # no WASI by default — the R-SEC-002 posture under test
    set(WAMR_BUILD_MULTI_MODULE 0)
    set(WAMR_BUILD_LIB_PTHREAD 0)
    set(WAMR_BUILD_SIMD 0)         # guest built without -msimd128; fast-interp has no SIMD

    # Windows-AOT determinism hardening (Context-Engine#24 / L-62 sub-note / R-SEC-009): with
    # AOT on Windows, WAMR's iwasm_aot.cmake git-fetches zydis (SHA-pinned upstream) and
    # zycore (UNPINNED — floats on the repo's default branch) at configure time for its
    # hw-bounds-check trap handler. Pre-declare zycore pinned to the exact commit that zydis
    # e14a0789 records as its dependencies/zycore submodule, so CI compiles the
    # upstream-tested pair instead of whatever zycore's default branch holds that day. The
    # FIRST FetchContent_Declare for a name wins — WAMR's later unpinned declare is a no-op.
    # (FINDINGS.md "Deviations" #3; a shipped WAMR port still owes vendoring per L-62.)
    if(WIN32)
        FetchContent_Declare(zycore
            GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
            GIT_TAG dd2211a023c2e6d383709fcb401a92943cb5e6b1)
    endif()

    include(${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)

    # FINDING (recorded in FINDINGS.md): WAMR unconditionally compiles its implementation of
    # the STANDARD wasm C API (wasm_c_api.c: wasm_engine_new, wasm_config_delete, ...), which
    # collides at link time with wasmtime's exports of the exact same symbols — two WASM
    # runtimes in one process is an ODR conflict on the standardized API. The spike talks to
    # WAMR through its native wasm_export.h API only, so the C-API TU is dropped.
    list(FILTER WAMR_RUNTIME_LIB_SOURCE EXCLUDE REGEX "wasm_c_api\\.c$")

    add_library(ctx_spike_wamr STATIC ${WAMR_RUNTIME_LIB_SOURCE})
    target_include_directories(ctx_spike_wamr SYSTEM PUBLIC ${WAMR_ROOT_DIR}/core/iwasm/include)
    if(WIN32)
        target_link_libraries(ctx_spike_wamr PUBLIC ws2_32 ntdll)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(ctx_spike_wamr PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(ctx_spike_wamr PUBLIC m)
        endif()
    endif()
    list(APPEND SPIKE_SOURCES src/engine_wamr.cpp)
endif()

# --- the bench executable ---------------------------------------------------------------------
add_executable(context-spike-wasm ${SPIKE_SOURCES})
target_include_directories(context-spike-wasm PRIVATE include)
target_link_libraries(context-spike-wasm PRIVATE context_warnings)
if(MSVC)
    # /external:W0 — keep /W4 /WX for spike code while third-party headers stay quiet.
    target_compile_options(context-spike-wasm PRIVATE /external:W0)
endif()

if(CONTEXT_SPIKE_WASM_WASMTIME)
    target_compile_definitions(context-spike-wasm PRIVATE CTX_SPIKE_HAS_WASMTIME)
    target_link_libraries(context-spike-wasm PRIVATE ctx_spike_wasmtime)
    if(WIN32)
        add_custom_command(TARGET context-spike-wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_wasmtime_root}/lib/wasmtime.dll"
                "$<TARGET_FILE_DIR:context-spike-wasm>"
            COMMENT "wasm spike: staging wasmtime.dll next to the executable")
    endif()
endif()

if(CONTEXT_SPIKE_WASM_WAMR)
    target_compile_definitions(context-spike-wasm PRIVATE CTX_SPIKE_HAS_WAMR)
    target_link_libraries(context-spike-wasm PRIVATE ctx_spike_wamr)
endif()

# Stage the committed guest module next to the exe (the benches' default --module path).
add_custom_command(TARGET context-spike-wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/guest/module.wasm"
        "$<TARGET_FILE_DIR:context-spike-wasm>"
    COMMENT "wasm spike: staging guest module.wasm next to the executable")

# --- AOT precompile (prebuilt wamrc, hash-pinned) ----------------------------------------------
set(_ctx_aot_wired OFF)
if(CONTEXT_SPIKE_WASM_AOT AND CONTEXT_SPIKE_WASM_WAMR)
    set(_wamrc_ver "2.4.5")
    if(WIN32)
        set(_wamrc_asset "wamrc-${_wamrc_ver}-x86_64-windows-2022.tar.gz")
        set(_wamrc_sha "0a16889b090a337d720f9c07f0dec0e6f758c4e781b7ae4ac6739ac3cd3b655c")
        set(_wamrc_exe "wamrc.exe")
    elseif(APPLE)
        set(_wamrc_asset "wamrc-${_wamrc_ver}-x86_64-macos-15-intel.tar.gz")
        set(_wamrc_sha "419d41b8b9b29bb8bb0e86e0409725abab81aa693daa479e502fadd0c206db69")
        set(_wamrc_exe "wamrc")
    else()
        set(_wamrc_asset "wamrc-${_wamrc_ver}-x86_64-ubuntu-22.04.tar.gz")
        set(_wamrc_sha "fedda99950a70f7dc45470ce88e12bf9e435b9482058ae97ce2cadd760e2329f")
        set(_wamrc_exe "wamrc")
    endif()

    set(CONTEXT_SPIKE_WAMRC "" CACHE FILEPATH
        "Path to a wamrc executable; empty = download the pinned prebuilt")
    if(CONTEXT_SPIKE_WAMRC)
        set(_wamrc "${CONTEXT_SPIKE_WAMRC}")
    else()
        set(_wamrc_dl "${CMAKE_BINARY_DIR}/_deps/wamrc")
        set(_wamrc "${_wamrc_dl}/${_wamrc_exe}")
        if(NOT EXISTS "${_wamrc}")
            message(STATUS "wasm spike: downloading wamrc ${_wamrc_ver} prebuilt")
            context_download(
                URL "https://github.com/bytecodealliance/wasm-micro-runtime/releases/download/WAMR-${_wamrc_ver}/${_wamrc_asset}"
                PATH "${_wamrc_dl}/${_wamrc_asset}"
                EXPECTED_SHA256 "${_wamrc_sha}"
                DESCRIPTION "wamrc ${_wamrc_ver} (${_wamrc_asset})")
            file(ARCHIVE_EXTRACT INPUT "${_wamrc_dl}/${_wamrc_asset}" DESTINATION "${_wamrc_dl}")
            # The tarball may nest the binary; normalize to ${_wamrc_dl}/${_wamrc_exe}.
            if(NOT EXISTS "${_wamrc}")
                file(GLOB_RECURSE _wamrc_found "${_wamrc_dl}/*${_wamrc_exe}")
                list(GET _wamrc_found 0 _wamrc_nested)
                file(COPY_FILE "${_wamrc_nested}" "${_wamrc}")
            endif()
        endif()
    endif()

    # Honest gating: verify the prebuilt actually EXECUTES on this host (a distro-mismatched
    # binary should disable the AOT leg with a loud warning, not fail the whole build).
    execute_process(COMMAND "${_wamrc}" --version
                    RESULT_VARIABLE _wamrc_rc OUTPUT_QUIET ERROR_QUIET)
    if(_wamrc_rc EQUAL 0)
        set(_ctx_aot_wired ON)
        # Deterministic codegen pin — the Context-Engine#24 root cause. Without --target/--cpu,
        # wamrc compiles for the BUILD host: cpu = LLVMGetHostCPUName() (a family/model-derived
        # name) with an EMPTY explicit feature string (aot_llvm.c), so LLVM derives the feature
        # set from the CPU MODEL's defaults — not from live CPUID. On the CPU-heterogeneous
        # GitHub runner fleet some VMs mask ISA their model name implies (e.g. AVX-512 on
        # Ice-Lake-class Xeons), and WAMR's AOT loader validates arch/endian/bitwidth but NEVER
        # the ISA feature level (aot_loader.c check_machine_info) — so an overshooting image
        # loads cleanly and dies with SIGILL mid-run, intermittently (per-VM lottery), on the
        # very machine that compiled it. Pinning target+cpu makes codegen host-independent:
        # x86-64-v2 (SSE4.2/POPCNT-level baseline) executes on every x86_64 runner in the fleet
        # (Intel Nehalem+ / all AMD EPYC). Keep these flags in lockstep with
        # aot_pin_guard.cmake, which re-derives and byte-verifies the image (R-QA-013 guard).
        # NOTE (perf): FINDINGS.md §1/§4 AOT numbers predate this pin (host-CPU codegen on the
        # Ryzen box); the smoke gate is correctness-only, so the pin does not affect them.
        set(_ctx_wamrc_flags --opt-level=3 --target=x86_64 --cpu=x86-64-v2)
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/module.aot"
            COMMAND "${_wamrc}" ${_ctx_wamrc_flags} -o "${CMAKE_CURRENT_BINARY_DIR}/module.aot"
                    "${CMAKE_CURRENT_SOURCE_DIR}/guest/module.wasm"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/guest/module.wasm"
            COMMENT "wasm spike: wamrc AOT-precompiling guest module.wasm -> module.aot (pinned x86-64-v2)")
        add_custom_target(context-spike-wasm-aot ALL
            DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/module.aot")
        add_custom_command(TARGET context-spike-wasm POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/module.aot"
                "$<TARGET_FILE_DIR:context-spike-wasm>"
            COMMENT "wasm spike: staging module.aot next to the executable")
        add_dependencies(context-spike-wasm context-spike-wasm-aot)
    else()
        message(WARNING "wasm spike: wamrc prebuilt does not run on this host (exit ${_wamrc_rc}) "
                        "— the WAMR-AOT leg is disabled; interp/JIT legs are unaffected.")
    endif()
endif()

# Smoke tests — registered only when the spike is built (the default CI matrix never sees
# them). `smoke` runs the full correctness battery (zero-copy visibility, traps, checksum
# parity) without the timed benches.
if(CONTEXT_SPIKE_WASM_WASMTIME)
    add_test(NAME context-spike-wasm-wasmtime-smoke
             COMMAND context-spike-wasm --runtime=wasmtime --bench=smoke)
endif()
if(CONTEXT_SPIKE_WASM_WAMR)
    add_test(NAME context-spike-wasm-wamr-smoke
             COMMAND context-spike-wasm --runtime=wamr --bench=smoke)
    if(_ctx_aot_wired)
        add_test(NAME context-spike-wasm-wamr-aot-smoke
                 COMMAND context-spike-wasm --runtime=wamr-aot --bench=smoke)
        # R-QA-013 regression guard for the #24 fix: aot_pin_guard.cmake re-compiles the guest
        # with its OWN hardcoded copy of the pinned flags (intentional redundancy with
        # _ctx_wamrc_flags above) and byte-compares the result against the image the build rule
        # produced. If the build rule's pin ever drifts (flag dropped/renamed → host-dependent
        # codegen again), the two images differ on any host whose CPU defaults are not exactly
        # the pinned baseline — so the regression fails HERE, deterministically, instead of
        # resurfacing as an intermittent SIGILL on the CI fleet.
        add_test(NAME context-spike-wasm-aot-pin-guard
                 COMMAND ${CMAKE_COMMAND}
                     "-DWAMRC=${_wamrc}"
                     "-DMODULE_WASM=${CMAKE_CURRENT_SOURCE_DIR}/guest/module.wasm"
                     "-DMODULE_AOT=${CMAKE_CURRENT_BINARY_DIR}/module.aot"
                     "-DREFERENCE_AOT=${CMAKE_CURRENT_BINARY_DIR}/module.pin-ref.aot"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/aot_pin_guard.cmake")
    endif()
endif()

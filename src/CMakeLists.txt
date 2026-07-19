# Context Game Engine — top-level build.
#
# Build files live in src/ (owner ruling 2026-07-02: the repo root stays minimal), so configure
# from the repo root with an explicit source dir:
#     cmake -S src --preset dev
# Sibling directories (spikes/, bench/, cmake/) are reached via ../ with explicit binary dirs.
#
# NOTE: vcpkg is deliberately NOT required to configure or build this skeleton. The manifest
# (src/vcpkg.json) activates only when the vcpkg toolchain file is passed explicitly
# (-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake); a plain configure works
# on a machine with no vcpkg at all.

cmake_minimum_required(VERSION 3.25)

project(ContextEngine
    VERSION 0.0.1
    DESCRIPTION "Context Game Engine — AI-first, file-authoritative, headless-complete"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CONTEXT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (baseline: ON)" ON)
option(CONTEXT_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (non-MSVC only)" OFF)
option(CONTEXT_TSAN "Build with ThreadSanitizer (non-MSVC only; mutually exclusive with CONTEXT_SANITIZE — TSan cannot combine with ASan in one binary)" OFF)
option(CONTEXT_DETERMINISTIC "Build the OPT-IN deterministic flavor (M6-F0a, R-SIM-005 / L-54): strict-FP engine-wide on the sim path (no fast-math; FP contraction pinned OFF), so the build PRODUCES a verified deterministic:true attestation. OFF by default — the routine dev/CI build stays non-deterministic; the dedicated `deterministic` preset + CI job turn it ON." OFF)
option(CONTEXT_BUILD_SPIKES "Build the M0 de-risking spike targets under spikes/ (opt-in; the default CI matrix stays spike-free; some spikes require the vcpkg toolchain with VCPKG_MANIFEST_FEATURES=spikes)" OFF)
option(CONTEXT_BUILD_SPIKE_WASM "Build ONLY spikes/wasm (subset of CONTEXT_BUILD_SPIKES; acquires its own runtimes — needs no vcpkg toolchain), so CI can exercise the WASM spike without configuring the vcpkg-dependent spikes" OFF)
option(CONTEXT_BUILD_SPIKE_CEF "Build the CEF accelerated-OSR compositing spike (L-41) — downloads the pinned ~162 MB CEF binary distribution; Windows/MSVC-only, local runs only, NEVER enabled in CI" OFF)
option(CONTEXT_BUILD_SPIKE_DOCKVIEW "Build the M9 s1 Dockview-in-CEF ratification spike (spikes/dockview-cef) — registers the CEF-free headless-Chromium probe self-check and, on Windows/MSVC only, the CEF-149 host (the same pinned ~162 MB CEF download as CONTEXT_BUILD_SPIKE_CEF). Local runs only, NEVER enabled in CI" OFF)
option(CONTEXT_BUILD_GUI_CEF "Build the M5 editor-GUI CEF build substrate (src/editor/cef/, issue #150) — fetches + SHA-verifies the pinned CEF prebuilt and boots it headless. A CI-only dependency path like V8 / wgpu-native (the MSVC/Clang-ABI prebuilt cannot link under the local Strawberry-GCC dev gate); default OFF so the default matrix + local dev gate never fetch CEF. The dedicated cef-substrate CI job sets it ON." OFF)

# spikes/wasm builds WAMR (a C library), and the CEF editor-GUI substrate's own bundled cmake
# (_cef/.../cmake/cef_variables.cmake) runs CHECK_C_COMPILER_FLAG on Linux — both need the C
# language. It must be enabled HERE — at top-level scope, BEFORE any add_subdirectory — not inside a
# subdirectory (e.g. src/editor/cef/). Enabling C mid-tree under a chainloaded vcpkg toolchain leaves
# earlier-processed directories with a stale no-C-compiler snapshot and generation fails with
# "No known features for C compiler".
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_WASM OR CONTEXT_BUILD_GUI_CEF)
    enable_language(C)
endif()

# M9 e03: the macOS ACCELERATED OSR import (src/render/src/wgpu/metal_interop.mm) is Objective-C++,
# so the OBJCXX language must be enabled for the wgpu render backend on Apple. Enabled HERE for the
# same reason C is above — turning a language on mid-tree (inside src/render/) leaves every
# already-processed directory holding a stale "no such compiler" snapshot and generation fails.
#
# Deliberately NOT gated on CONTEXT_BUILD_RENDER_WGPU: that option is declared 700 lines below in
# src/render/CMakeLists.txt, so reading it here sees a value only when the CI job happens to pass
# -D on the command line. A default-ON flip (or a preset cacheVariable) would silently skip the
# enable and fail generation on macOS with "no OBJCXX compiler". Enabling a language is cheap.
if(APPLE)
    enable_language(OBJCXX)
    # The engine's language contract (C++20, extensions OFF) applies to Objective-C++ too. Without
    # these, CMake emits NO -std= flag for a .mm and Apple clang falls back to its default gnu++17 —
    # a different standard WITH GNU extensions, on the one CI leg that compiles this file.
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

# Warnings-as-errors baseline: the `context_warnings` interface target (../cmake/ContextWarnings.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextWarnings.cmake)

# Resilient prebuilt downloader: context_download() = retry/backoff + fail-closed SHA verify
# (../cmake/ContextDownload.cmake, Context-Engine#129). Included here so every src/ subdirectory
# (e.g. src/render's wgpu-native fetch) can call it; the spikes that configure STANDALONE include
# it themselves under a `if(NOT COMMAND context_download)` guard.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextDownload.cmake)

# Shared CEF-acquisition macro: context_acquire_cef() (../cmake/ContextCef.cmake, Context-Engine#214) —
# fetch/verify the pinned CEF prebuilt + find_package(CEF) + build libcef_dll_wrapper. Included here
# (unconditionally, like the two modules above) so BOTH CONTEXT_BUILD_GUI_CEF call sites — src/editor/cef/
# (M5-F0a) and src/editor/gui/host/ (M5-F0b) — share one definition instead of duplicating the prologue.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextCef.cmake)

# Threads: the bridge IPC transport uses <thread> (backoff sleep) and the cross-process tests spawn a
# server thread; on Linux/GCC that requires -pthread. Threads::Threads is a no-op on the Windows/MSVC
# and MinGW legs (native threads), so the toolchain matrix is untouched.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(CONTEXT_SANITIZE AND CONTEXT_TSAN)
    message(FATAL_ERROR "CONTEXT_SANITIZE and CONTEXT_TSAN are mutually exclusive: ThreadSanitizer cannot be combined with ASan/UBSan in one binary (use the separate `sanitize` and `tsan` presets).")
endif()

# ASan exists on MSVC but UBSan does not, and MSVC has no ThreadSanitizer; both sanitizer
# presets are scoped to non-MSVC toolchains (design lock L-38: sanitizer/TSan CI runs on
# Linux/Clang).
if(MSVC AND (CONTEXT_SANITIZE OR CONTEXT_TSAN))
    message(FATAL_ERROR "CONTEXT_SANITIZE and CONTEXT_TSAN are only supported with non-MSVC toolchains (Clang/GCC).")
endif()

if(CONTEXT_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CONTEXT_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# M6-F0a — the OPT-IN DETERMINISTIC BUILD FLAVOR (R-SIM-005 / L-54, issue #170).
#
# Determinism is a WHOLE-BUILD property, not a per-system flag (R-SIM-005): strict-FP flags are set
# ENGINE-WIDE on the sim path (no fast-math anywhere the sim can reach) and FMA / floating-point
# contraction is PINNED (uniformly OFF, never compiler-discretionary), with the sim path forbidden
# from platform `libm` (a rule the F0b deterministic transcendental lib will enforce structurally).
# Applied here as global compile options — BEFORE any add_subdirectory — so every engine target
# inherits them (the sanitizer blocks above use the same global-add pattern). The per-compiler
# spellings differ: clang / Apple clang / GCC take `-ffp-contract=off`; MSVC takes `/fp:strict`. The
# EXACT applied-flag string is recorded into CONTEXT_DETERMINISTIC_APPLIED_FLAGS so src/runtime/
# determinism/ can bake it into the PRODUCED attestation (never a self-declared bit). Default OFF: the
# routine dev/CI build stays non-deterministic; the `deterministic` preset + its CI job turn it ON.
set(CONTEXT_DETERMINISTIC_APPLIED_FLAGS "")
if(CONTEXT_DETERMINISTIC)
    if(MSVC)
        set(CONTEXT_DETERMINISTIC_APPLIED_FLAGS "/fp:strict")
    else()
        # -ffast-math is deliberately NOT added (and MUST NOT be): the produced attestation VERIFIES
        # its absence via the compiler-set __FAST_MATH__ macro. GCC/Clang default FP contraction to
        # `fast` at -O, so pinning it OFF is meaningful — it is what makes an FMA non-discretionary.
        set(CONTEXT_DETERMINISTIC_APPLIED_FLAGS "-ffp-contract=off")
    endif()
    separate_arguments(_context_deterministic_flag_list NATIVE_COMMAND
        "${CONTEXT_DETERMINISTIC_APPLIED_FLAGS}")
    add_compile_options(${_context_deterministic_flag_list})
    add_compile_definitions(CONTEXT_DETERMINISTIC=1)
endif()

enable_testing()

# spikes/ lives OUTSIDE the source dir (sibling of src/), so every spike is added with an
# explicit binary dir; the ${CMAKE_BINARY_DIR}/spikes/<name> layout keeps output paths
# identical to the pre-move tree (the CI bench job depends on <binaryDir>/spikes/parse-bench/...).
function(add_spike name)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../spikes/${name} ${CMAKE_BINARY_DIR}/spikes/${name})
endfunction()

# M0: the only always-on target is the hello spike.
add_spike(hello)

# M1: the microkernel core — the first real engine component (src/kernel/). R-KERNEL-001's ~6
# stable interfaces; src/editor/, src/runtime/, src/packages/ gain their targets in later M1 tasks.
add_subdirectory(kernel)

# Shared cross-cutting infrastructure (src/common/, issue #146): the ONE hardened std::system
# subprocess + scratch-temp-file runner (context_common) consolidating the copies that had diverged
# across the shadercc WGSL leg, the esbuild + tsgo TS drivers, and the real-git merge test. Stdlib +
# the warnings baseline only — no engine-domain deps — so it builds on EVERY toolchain. Wired here,
# right after kernel and deliberately far from the editor/runtime/render clusters and the file tail,
# so concurrent sibling tasks inserting into those clusters merge cleanly (consumers name the
# `context_common` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(common)

# M4 wave-1: the broad-phase spatial acceleration structure (src/packages/spatial/, R-SIM-007, issue
# #115) — the FIRST real first-party package under src/packages/. An incrementally-updated dynamic
# AABB tree over transform-bearing entities: O(log N) insert/update/remove (never rebuilt per frame)
# and O(result + log N) radius/AABB queries, designed as the ONE index shared by render culling
# (L-39), spatial queries (R-CLI-006), and asset streaming (R-ASSET-003) in later waves. It is a
# PACKAGE, not kernel core (R-KERNEL-001 stays minimal): it composes on context_kernel's Entity and
# the kernel never links back. Wired here immediately after add_subdirectory(kernel) — its only dep —
# and deliberately far from the editor/runtime clusters and the file tail, so the concurrent sibling
# task inserting add_subdirectory(render) elsewhere merges cleanly (consumers name the
# `context_spatial` target, which CMake resolves at generate time regardless of add_subdirectory order).
add_subdirectory(packages/spatial)

# M6-F0b foundation: the shared no-alloc DETERMINISTIC sim-math library (src/packages/simmath/,
# R-SIM-008 math half, issue #172) — the ONE fixed-point vector/quaternion/transform + deterministic
# transcendental (sin/cos/tan/sqrt/atan/atan2) library every M6 sim package composes on. Integer-only
# by law (Q16 int64, no float, no platform libm), so its results fold into the L-54 hierarchical state
# hash bit-identically across the determinism matrix. It is a PACKAGE, not kernel core (R-KERNEL-001 /
# L-60): it composes on context_kernel and the kernel never links back. Wired here immediately after
# packages/spatial (its sibling package) and deliberately far from the editor/runtime clusters and the
# file tail, so concurrent sibling tasks inserting elsewhere merge cleanly (consumers name the
# `context_simmath` target, which CMake resolves at generate time regardless of add_subdirectory order).
add_subdirectory(packages/simmath)

# M2: the REAL canonical-JSON serializer (src/editor/serializer/, issue #42) — R-FILE-001/L-32:
# strict JSON parse -> NFC-normalized canonical bytes + the canonical-content hash. Dependency-free
# (stdlib only); wired BEFORE editor/derivation, whose canonical-parse node delegates to it.
add_subdirectory(editor/serializer)

# M1: the EditorKernel file-sync layer (src/editor/filesync/) — atomic IO + watch-hash-reconcile +
# crash-recovery intent log (R-FILE-002/004, R-QA-010), built on the kernel platform seams.
add_subdirectory(editor/filesync)

# M1: the EditorKernel incremental derivation graph (src/editor/derivation/) — file changes ->
# canonical parse -> derived World (L-19/L-22), with derivation-side backpressure (R-FILE-013) and the
# read-your-writes generation barrier (R-CLI-006), built on the file-sync layer (its input) + kernel.
# Wired immediately after filesync (its dependency) so concurrent sibling edits to this file merge cleanly.
add_subdirectory(editor/derivation)

# M0 de-risking spikes are OPT-IN (CONTEXT_BUILD_SPIKES=ON): the default CI matrix stays
# untouched. spikes/ecs takes its dependencies from the vcpkg manifest's `spikes` feature;
# spikes/js-engine acquires its pinned backends itself (FetchContent + NuGet — see its README);
# spikes/webgpu downloads the pinned wgpu-native prebuilt itself (no vcpkg port exists) and is
# also standalone-configurable for its Emscripten web leg — see its CMakeLists.txt.
if(CONTEXT_BUILD_SPIKES)
    add_spike(ecs)
    add_spike(parse-bench)
    add_spike(js-engine)
    add_spike(webgpu)
endif()

# spikes/wasm acquires its own pinned runtimes (wasmtime prebuilt C API + WAMR from source) and
# needs no vcpkg toolchain, so it also has a stand-alone toggle the spikes CI job uses.
# (The C language WAMR needs is enabled at the top of this file, before any add_subdirectory.)
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_WASM)
    add_spike(wasm)
endif()

# spikes/cef-compositing (L-41) is DOUBLY gated: CEF's binary distribution is a ~162 MB
# download, so the spike hides behind its own CONTEXT_BUILD_SPIKE_CEF switch and is never
# built in CI. With CONTEXT_BUILD_SPIKES=ON but the CEF switch OFF (the CI bench job) the
# directory is entered and returns early — configure-only exercise, no download. With
# CONTEXT_BUILD_SPIKE_CEF=ON alone (the local Windows run) the spike builds WITHOUT dragging
# in the vcpkg-dependent spikes above.
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_CEF)
    add_spike(cef-compositing)
endif()

# spikes/dockview-cef (M9 s1) is gated the same way as cef-compositing: with CONTEXT_BUILD_SPIKES=ON
# but the dockview switch OFF (the CI bench job) the directory is entered and returns early
# (configure-only, no download, nothing registered — CI untouched). CONTEXT_BUILD_SPIKE_DOCKVIEW=ON
# registers the CEF-free python probe self-check and, on Windows/MSVC only, builds the CEF-149 host.
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_DOCKVIEW)
    add_spike(dockview-cef)
endif()

# M2 wave 2: the engine schema vocabulary + units law + per-kind versioned schemas (R-DATA-006,
# src/editor/schema/, issue #47) — x-ctx-type/x-ctx-storage/x-ctx-ref, the ONE tagged-union
# convention, SI+radians x-ctx-units, the compiled engine kind schemas (scene, project), and the
# document validator the derivation validate node runs. Wired here — deliberately apart from the
# serializer/derivation block above — so concurrent sibling tasks touching src/editor/ and this
# file merge cleanly (its consumers name the `context_schema` target, which CMake resolves at
# generate time regardless of add_subdirectory order).
add_subdirectory(editor/schema)

# M3 task 2b-ii: declarative component-type authoring (src/editor/component/, R-LANG-010, issue #86)
# — schema-first component definitions compiled into a runtime storage layout + published schema and
# registered into the kernel World's data-driven archetype storage (L-60, no native rebuild). Depends
# on context_schema (x-ctx-* law), context_serializer (parse + publish), context_kernel (raw World).
# Wired here — deliberately apart from the serializer/schema block above — so concurrent sibling tasks
# touching src/editor/ and this file merge cleanly (its consumers name the `context_component` target,
# which CMake resolves at generate time regardless of add_subdirectory order).
add_subdirectory(editor/component)

# M1 public-contract cluster (R-CLI-007/008/009/013): the single registry that generates
# CLI ≡ RPC ≡ MCP ≡ introspection (src/editor/contract/). Wired here — deliberately apart from
# add_subdirectory(kernel) above — so concurrent sibling tasks touching src/editor/ and src/ merge
# cleanly.
add_subdirectory(editor/contract)

# The EditorKernel bridge (M1): daemon-per-worktree + single-instance lock (R-BRIDGE-001) + JSON-RPC
# 2.0 RPC dispatcher with R-SEC-007 scope enforcement + the client-facing R-BRIDGE-008 event stream.
# Dispatches OVER the contract registry (src/editor/contract/) and forwards context_kernel's log
# topic. Wired here — apart from add_subdirectory(kernel) above — so concurrent sibling tasks touching
# src/kernel/ and src/ merge cleanly.
add_subdirectory(editor/bridge)

# The EditorKernel composition (M1): wires filesync + derivation + kernel World + bridge into ONE
# runnable headless attach path (src/editor/editorkernel/). Added AFTER its five composed deps
# (filesync, derivation, kernel, contract, bridge) so their targets exist for its PUBLIC link.
add_subdirectory(editor/editorkernel)

# The `context` CLI (M1, src/cli/) — consumes the contract registry (CLI ≡ RPC ≡ MCP surface) and
# context_kernel, plus context_editorkernel for the `context editor smoke` in-process driver. Added
# AFTER contract + editorkernel so its link deps exist.
add_subdirectory(cli)

# M9 e02 — context_client (src/editor/client/): the client SDK the CLI rides and the FIRST
# installed/exported library (D10 / A-F1). Added AFTER cli deliberately: its cross-process e2e test
# add_dependencies() on the `context` binary, which must therefore already exist. The CLI's own
# `target_link_libraries(context_cli PRIVATE context_client)` above names the target, which CMake
# resolves at generate time regardless of add_subdirectory order (the convention used throughout
# this file). PRIVATE matters: no client type appears in the CLI's own public headers, so the CLI
# does not re-export the SDK.
add_subdirectory(editor/client)

# M1 test-system foundations (R-QA-010): the deterministic fault-injection harness (src/testing/) —
# a reusable context_testing support lib + scenario tests driving the injectable kernel + file-sync +
# bridge seams under a seeded fault schedule. Wired here — apart from add_subdirectory(kernel) above —
# so concurrent sibling tasks touching src/editor/ and src/ merge cleanly.
add_subdirectory(testing)

# ---------------------------------------------------------------------------
# R-VER-004 — versioned install layout (docs/versioned-install.md).
#
# Every engine version installs SIDE-BY-SIDE under <prefix>/versions/<semver>/ and NEVER
# flat — a first release that installed flat would foreclose side-by-side installs forever,
# so the LAYOUT is the day-one contract. The resolver/fetcher/launcher machinery lands with
# the second release; this is the minimal install/package skeleton that stakes out the
# convention now. R-SEC-009: when the on-demand version fetcher lands, a fetched version is
# verify-before-use / fail closed via tools/verify_artifact.py before it is ever executed.
# ---------------------------------------------------------------------------
# R-VER-004 side-by-side install root: <prefix>/versions/<semver>/. A PLAIN (non-cache)
# variable so it is recomputed from PROJECT_VERSION on EVERY configure. A
# `set(... CACHE STRING ...)` without FORCE is a no-op once the cache entry exists, so
# re-configuring an existing build dir after a version bump would strand this at the OLD
# version while context-version.txt / CPACK_PACKAGE_VERSION (rewritten below from
# PROJECT_VERSION every configure) track the new one — the versioned dir name and its own
# marker would then disagree, the exact invariant cmake/versioned-install-check.cmake guards.
# The layout is a fixed day-one contract, so it is intentionally NOT overridable via -D.
set(CONTEXT_INSTALL_SUBDIR "versions/${PROJECT_VERSION}")

# Machine-readable version marker: a resolver enumerates installed versions by listing
# <prefix>/versions/*/ — the day-one hook the future launcher resolves the R-VER-003 pin
# against.
set(_context_version_marker "${CMAKE_CURRENT_BINARY_DIR}/context-version.txt")
file(WRITE "${_context_version_marker}"
     "name=Context Engine\nversion=${PROJECT_VERSION}\nlayout=R-VER-004\n")

# M0 has one always-on target (context-hello); it stands in as the installed payload so the
# layout is proven by a real file landing under versions/<semver>/bin/. Engine components
# (kernel/editor/runtime/packages) join this install set as they gain targets from M1 — the
# microkernel library (context_kernel) is the first to do so.
#
# M9 e02 (A-F1 / D10) adds the first EXPORTED CONSUMER SURFACE: context_client, plus the ONE other
# module its public headers actually name — context_contract (json / handshake / envelope). An
# out-of-tree consumer — the editor shell, e05's JS client host, any third-party client — builds
# against THOSE headers and nothing else; the `editor-boundary` CI job proves it by compiling a real
# consumer against the installed tree with a transitive include-graph check.
#
# A STATIC library carries none of its dependencies' objects, so the install set must be the whole
# static-link CLOSURE or a consumer link fails on undefined symbols: context_client -> bridge,
# contract; bridge -> contract, kernel; contract -> schema, component; schema/component ->
# serializer. The other FIVE (bridge, kernel, schema, component, serializer) are LINK-only payload —
# their headers are deliberately NOT installed (see below), so they are invisible to a consumer's
# include graph. That asymmetry IS the boundary: link what the implementation needs, expose only what
# the contract publishes.
#
# bridge and kernel are in that link-only group ON PURPOSE, and it is worth saying why since the SDK
# is built over them: NO installed header names a bridge or kernel type. `client.cpp` includes
# bridge/transport.h, but that is an IMPLEMENTATION include, which is exactly the kind of dependency
# a published boundary is supposed to hide. Publishing them would put bridge::Daemon (start(), the
# .editor lock, set_method_backend) and the whole microkernel — World, scheduler, entity, component,
# command_buffer — on the supported surface, so a "client" could instantiate a daemon in-process:
# precisely the inversion D10 exists to prevent. Widening this later is free; narrowing it after
# e04/e05 build against it is a breaking change.
install(TARGETS context-hello
                context_client context_bridge context_contract context_kernel
                context_schema context_component context_serializer
    RUNTIME DESTINATION "${CONTEXT_INSTALL_SUBDIR}/bin"
    LIBRARY DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib"
    ARCHIVE DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib")
install(FILES "${_context_version_marker}" DESTINATION "${CONTEXT_INSTALL_SUBDIR}")

# The PUBLIC header surface — exactly these two modules, nothing else. Adding a directory here
# widens the published boundary, so it is a deliberate, reviewable act (D10). Keep this list and
# ALLOWED_MODULES in tools/check_include_graph.py in lockstep: the gate is what makes the narrowness
# enforceable rather than aspirational.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/editor/client/include/"
                  "${CMAKE_CURRENT_SOURCE_DIR}/editor/contract/include/"
    DESTINATION "${CONTEXT_INSTALL_SUBDIR}/include"
    FILES_MATCHING PATTERN "*.h")

# The GENERATED client schema (built by the context_client_schema target): the artifact downstream
# client generators consume — e05's JS client above all — so it ships WITH the SDK rather than being
# re-derived (or worse, hand-transcribed) by each consumer.
install(FILES "${CONTEXT_CLIENT_SCHEMA_GENERATED}"
    DESTINATION "${CONTEXT_INSTALL_SUBDIR}/share/context")

# The CMake package an out-of-tree consumer finds: `find_package(ContextClient REQUIRED)` then
# `target_link_libraries(<t> PRIVATE Context::context_client)`.
#
# The config declares its IMPORTED targets by hand rather than via install(EXPORT). That is
# deliberate, not a shortcut: install(EXPORT) exports a static library's PRIVATE deps too (recorded
# as $<LINK_ONLY:…>), which would drag context_schema / context_component / context_warnings into
# the exported set and force every one of them to be export-ready — including reworking four sibling
# CMakeLists to $<BUILD_INTERFACE:>/$<INSTALL_INTERFACE:> include dirs. Worse, it would publish
# internal targets as part of the package's public surface, which is exactly what D10 forbids.
# Hand-authoring keeps the consumer-visible surface EXACTLY `Context::context_client`.

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ContextClientConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
# PACKAGE_* variables the config resolves relative to its own installed location, so the whole
# versions/<semver>/ tree stays relocatable (R-VER-004: side-by-side installs, no absolute paths
# baked into a shipped package).
set(CONTEXT_CLIENT_INSTALL_INCLUDEDIR "${CONTEXT_INSTALL_SUBDIR}/include")
set(CONTEXT_CLIENT_INSTALL_LIBDIR "${CONTEXT_INSTALL_SUBDIR}/lib")
set(CONTEXT_CLIENT_INSTALL_DATADIR "${CONTEXT_INSTALL_SUBDIR}/share/context")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextClientConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ContextClientConfig.cmake"
    INSTALL_DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib/cmake/ContextClient"
    PATH_VARS CONTEXT_CLIENT_INSTALL_INCLUDEDIR CONTEXT_CLIENT_INSTALL_LIBDIR
              CONTEXT_CLIENT_INSTALL_DATADIR)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ContextClientConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ContextClientConfigVersion.cmake"
    DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib/cmake/ContextClient")

# CPack packaging: the produced archive carries the SAME versions/<semver>/ internal layout
# (it consumes the install() rules above), so `cpack -G ZIP` and `cmake --install` agree by
# construction. Kept minimal — a built-in ZIP archive, no external packaging tools.
set(CPACK_PACKAGE_NAME "context-engine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE.md")
set(CPACK_GENERATOR "ZIP")
include(CPack)

# M2 wave 4: schema-aware structural three-way merge (src/editor/merge/, R-FILE-012 / L-26/L-50/L-33,
# issue #59) — the convergence primitive for worktree-per-agent parallelism: a field-path-granular,
# id-identity 3-way merge over authored JSON plus the git merge driver (`context new` writes the
# .gitattributes mapping; L-27 keeps git OUT of the engine — the driver is invoked BY git). Wired
# here as its OWN stanza, deliberately apart from the editor cluster above and immediately before
# editor/migrate, so concurrent M2 sibling tasks inserting into this file merge cleanly (consumers
# name the `context_merge` target, which CMake resolves at generate time regardless of order).
add_subdirectory(editor/merge)

# M2 wave 3: per-component-payload schema versioning + parse-time migration (src/editor/migrate/,
# L-37 / R-DATA-004, issue #52) — the registered migration set (the R-FILE-005 pass-0 stratum's
# migration half, whose set hash keys pass-1 derivation cache entries), the in-memory
# migrate_document engine, and the R-QA-011 fixture corpus. Wired here — deliberately far from the
# editor add_subdirectory cluster above — so concurrent sibling tasks inserting into this file
# merge cleanly (its consumers name the `context_migrate` target, which CMake resolves at generate
# time regardless of add_subdirectory order).
add_subdirectory(editor/migrate)

# CI proof (runs under `ctest --preset dev` on all three OS legs of the CI build matrix):
# stage a real install into a build-local prefix and assert it landed under
# versions/<semver>/ and NOT flat. Fails the build on any R-VER-004 layout violation.
add_test(NAME versioned-install-layout
    COMMAND "${CMAKE_COMMAND}"
        -D "CONTEXT_BINARY_DIR=${CMAKE_BINARY_DIR}"
        -D "CONTEXT_EXPECTED_VERSION=${PROJECT_VERSION}"
        -D "CONTEXT_INSTALL_SUBDIR=${CONTEXT_INSTALL_SUBDIR}"
        -D "CONTEXT_BUILD_CONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/versioned-install-check.cmake")

# CI proof for the resilient prebuilt downloader (context_download, Context-Engine#129): exercises
# the success / fail-closed-on-wrong-pin / retry-then-fail paths OFFLINE via file:// URLs, so it
# runs on the local GCC dev gate AND every OS leg of the CI build matrix (no network, no prebuilt).
add_test(NAME download-retry-selfcheck
    COMMAND "${CMAKE_COMMAND}"
        -D "CONTEXT_DOWNLOAD_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextDownload.cmake"
        -D "FIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/tests/fixtures"
        -D "WORK_DIR=${CMAKE_BINARY_DIR}/download-retry-selfcheck"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/tests/test_download_retry.cmake")

# M2 wave 4: the importer framework (src/editor/import/, issue #60) — package-based glTF/PNG/WAV
# importers -> canonical engine descriptors, run-deterministic (R-ASSET-001 double-run byte-compare
# gate) + isolated (the R-SEC-006 v1 slice: shared R-SEC-008 path jail + scrubbed env R-SEC-010 + no
# ambient network) + the exhaustive R-FILE-010 cache key + a per-platform transcode skeleton; mesh
# import reserves UV2 (R-REND-006). OWN stanza inserted immediately before assetdb — merge hygiene:
# concurrent M2 wave-4 siblings insert their own stanzas elsewhere, so this never collides at the
# file tail (consumers name `context_import`, resolved at generate time regardless of order).
add_subdirectory(editor/import)

# M2 wave 3: the asset database (src/editor/assetdb/, issue #53) — GUID + sidecar <asset>.meta.json
# identity (L-36/R-ASSET-002), dual-form + entity references with the real x-ctx-ref meta lookup
# behind the schema module's RefTargetResolver seam (L-34/R-DATA-006), meta-first move/rename +
# raw-move GUID healing (R-FILE-003/004), and the bounded lazy GUID index (R-FILE-011(e)). Wired
# here — deliberately apart from the src/editor/ cluster above — so the concurrent M2 sibling tasks
# inserting into this file merge cleanly (consumers name the `context_assetdb` target, which CMake
# resolves at generate time regardless of add_subdirectory order).
add_subdirectory(editor/assetdb)

# M2 wave 4: the first content asset KINDS' semantics (src/editor/kinds/, R-2D-003 tilemap +
# R-I18N-001 string-table, issue #61) — the L-33 split-nudge over tilemap chunk cell payloads +
# stable-id/region checks, and the string-table CLDR plural rules + fallback-chain resolution +
# semantic validation. The per-kind SCHEMAS live in editor/schema/ (engine_schemas()); this is the
# behaviour on top. Wired here — deliberately apart from the editor/schema block above — so concurrent
# sibling tasks touching src/editor/ and this file merge cleanly (consumers name the `context_kinds`
# target, which CMake resolves at generate time regardless of add_subdirectory order).
add_subdirectory(editor/kinds)

# M8.5 a18: the tilemap authoring WRITE path (src/editor/tilemap/, R-2D-003 GUI half / R-CLI-001 /
# L-20 / L-33) — the ONE cell-edit core both the tile-painting GUI (gesture-end commit) and the
# `context tilemap paint|fill` CLI verbs call, so GUI == CLI parity holds by construction. Builds ON
# filesync's L-33 sidecar family planner + the canonical serializer; the ctx:tilemap SCHEMA stays in
# editor/schema/ and the content rules in editor/kinds/ (the M2 format is untouched). Wired here right
# after kinds — deliberately apart from the GUI cluster — so concurrent sibling tasks touching
# src/editor/ and this file merge cleanly (consumers name the `context_tilemap_edit` target, resolved
# at generate time regardless of add_subdirectory order).
add_subdirectory(editor/tilemap)

# M2 wave 3: the scene-composition READ path (src/editor/compose/, L-35/R-DATA-002, issue #51) —
# id-path overrides with innermost-out precedence, stable intra-file ids (L-33), the scene-root
# entity, the R-FILE-011(e) depth-cap/fan-in/cycle diagnostics, deterministic composed identity
# (L-37), and the R-CLI-006 provenance chains. The derivation graph's compose node links this
# target. Wired here — deliberately apart from the serializer/derivation and schema blocks above —
# so concurrent sibling tasks touching src/editor/ and this file merge cleanly (consumers name the
# `context_compose` target, which CMake resolves at generate time regardless of order).
add_subdirectory(editor/compose)

# M8 task a01: the build-side chunked content-pack WRITER (src/editor/pack/, R-ASSET-005 / L-35 /
# L-37, docs/chunk-pack-format.md) — freezes pack format v1 and packs flatten-emitted content units
# (context_compose) into a deterministic, GUID-addressed v1 archive (directory + per-chunk hashes +
# format/engine version header). Wired here right after compose (its input) — deliberately apart from
# the runtime cluster below so concurrent sibling tasks touching src/editor/ and this file merge
# cleanly (consumers name the `context_pack` target, resolved at generate time regardless of order).
add_subdirectory(editor/pack)

# M8 task a05: the build orchestration core (src/editor/build/, R-BUILD-002 / R-BUILD-007) — the pure,
# deterministic spine of the per-agent build pipeline (verify → toolchain → aot → transcode → pack →
# link → adapter). Wired here right after pack (its final chain link) — deliberately apart from the
# runtime cluster below so concurrent sibling tasks touching src/editor/ and this file merge cleanly
# (consumers name the `context_build` target, resolved at generate time regardless of order). It binds
# context_compose + context_import + context_pack; the CLI (add_subdirectory(cli) above) links it.
add_subdirectory(editor/build)

# M2 wave 4: player save-game groundwork (src/runtime/save/, R-DATA-005 / L-37, issue #66) — the
# FIRST src/runtime/ (RuntimeKernel) target. RuntimeKernel's OWN serialization (R-FILE-009 — never
# authored files): a per-component schemaVersion save header + composed-identity-addressed entity
# records (L-37) + a minimal save-migration runner that REUSES the editor's per-payload migration
# mechanism (context_migrate::migrate_payload) so a shipped build loads older saves through exactly
# the parse-time mechanism. OWN stanza inserted here — deliberately apart from the editor cluster
# above and before the M1 exit-gate block — so it never collides at the file tail (consumers name
# the `context_save` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(runtime/save)

# M8 task a02: the RuntimeKernel async streaming content LOADER (src/runtime/content/, R-ASSET-005 /
# R-ASSET-003 / R-FILE-009 / L-24, docs/chunk-pack-format.md) — the runtime consumer of the a01 v1
# pack: async load/instantiate/unload of packed content units by GUID, a memory-budgeted streaming
# scheduler with proximity-driven decisions (R-SIM-007), and the one loading seam fed BOTH by a pack
# and the live in-editor derived units. Parses the pack via the frozen byte-layout constants
# (context_pack_format) + the canonical serializer only — never context_compose (L-24: no editor code
# in a RuntimeKernel build). OWN stanza next to its runtime/save sibling — apart from the editor
# cluster + the exit-gate block below — so concurrent sibling tasks inserting into this file merge
# cleanly (consumers name the `context_runtime_content` target, resolved at generate time regardless
# of add_subdirectory order).
add_subdirectory(runtime/content)

# M6-F0a foundation: the deterministic-build ATTESTATION (src/runtime/determinism/, R-SIM-005 / L-54,
# anchored in R-SEC-009, issue #170) — the engine-PRODUCED `deterministic:true` attestation, emitted
# from the actually-applied, verified strict-FP flags (the CONTEXT_DETERMINISTIC flag block above),
# never a self-declared manifest bit. Mints the determinism.attestation_* fail-closed codes (registered
# in the contract error catalog). OWN stanza — deliberately apart from the editor cluster + the
# exit-gate block below — so concurrent sibling tasks inserting into this file merge cleanly (consumers
# name the `context_determinism` target, resolved at generate time regardless of add_subdirectory order).
# Placed before runtime/session so the determinism foundation reads as the first runtime/ target.
add_subdirectory(runtime/determinism)

# M3 entry: the headless simulation session + versioned replay artifact + hierarchical determinism
# hashing (src/runtime/session/, R-QA-005 / L-54, issue #74) — the deterministic headless harness the
# whole M3 automated-QA / determinism story builds on: fixed-tick session control (step-N / seed /
# synthetic input + action injection / record + replay / state-hash query), the monotonic simTick
# counter, the hierarchical canonical state hash (root + per-system + per-archetype + trace), and the
# versioned schema'd replay-artifact kind. OWN stanza — apart from the editor cluster + the exit-gate
# block below — so it never collides at the file tail (consumers name the `context_session` target,
# resolved at generate time regardless of add_subdirectory order). Registers the `determinism-*`
# ctest family the blocking CI determinism gate runs across the 3-OS matrix.
add_subdirectory(runtime/session)

# M6 P1: the deterministic fixed-point rigid-body 3D physics package (src/packages/physics3d/,
# R-SYS-001, issue #174) — the first M6 gameplay package, implementing the F0a physics-core decision
# (docs/physics-determinism-decision.md): sim state is integer/fixed-point ONLY (the simmath Q16
# core), broad-phase via context_spatial (a conservative prune — the exact fixed-point narrow phase
# decides sim state), sim components folded into the L-54 hierarchical state hash by stable name
# through the M6-F0b sim_components() seam. It is a PACKAGE, not kernel core (R-KERNEL-001 / L-60):
# the kernel never links back. Registers the determinism-physics3d-scene ctest, joining the
# determinism-* family the blocking CI Determinism gate runs across the 3-OS matrix. Wired here,
# immediately after runtime/session (whose context_session provides the sim-component registration
# seam it links) and deliberately apart from the packages/spatial+simmath cluster above, so
# concurrent sibling tasks inserting into this file merge cleanly (consumers name the
# `context_physics3d` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(packages/physics3d)

# M6 P2: the deterministic fixed-point rigid-body 2D physics package (src/packages/physics2d/,
# R-2D-002 / L-55, issue #176) — the 2D sibling of physics3d, a Box2D-class 2D rigid-body package under
# the same R-KERNEL-004 contract, reusing the F0a fixed-point (Q16) determinism pattern P1 proved: sim
# state is integer/fixed-point ONLY (the simmath core), broad-phase via context_spatial (a conservative
# degenerate-Z prune — the exact fixed-point narrow phase decides sim state), sim components folded into
# the L-54 hierarchical state hash by stable name through the M6-F0b sim_components() seam. It is a
# PACKAGE, not kernel core (R-KERNEL-001 / L-60): the kernel never links back. Registers the
# determinism-physics2d-scene ctest, joining the determinism-* family the blocking CI Determinism gate
# runs across the 3-OS matrix. Wired here immediately after packages/physics3d (its sibling) and
# deliberately apart from the editor/runtime clusters and the file tail, so concurrent sibling tasks
# inserting into this file merge cleanly (consumers name the `context_physics2d` target, resolved at
# generate time regardless of add_subdirectory order).
add_subdirectory(packages/physics2d)

# M6 P4: the particle-system package (src/packages/particles/, R-SYS-003, issue #178) — authored +
# simulated effects with the determinism split (docs/physics-determinism-decision.md + R-SIM-001):
# DETERMINISTIC SIM-PATH particles are integer/fixed-point ONLY (the simmath Q16 core) and fold into
# the L-54 hierarchical state hash by stable name through the M6-F0b sim_components() seam; FREE-RUNNING
# COSMETIC particles are a presentation observer that reads sim state read-only, holds its own float
# state, and folds into NO hash (the R-SIM-001 downstream-observer rule). It is a PACKAGE, not kernel
# core (R-KERNEL-001 / L-60): the kernel never links back. Particles do not collide, so — unlike the
# physics packages — it has NO context_spatial dependency. Registers the determinism-particle-scene
# ctest, joining the determinism-* family the blocking CI Determinism gate runs across the 3-OS matrix.
# Wired here immediately after packages/physics2d (its M6 WAVE sibling) and deliberately apart from the
# editor/runtime clusters and the file tail, so concurrent sibling tasks inserting into this file merge
# cleanly (consumers name the `context_particles` target, resolved at generate time regardless of
# add_subdirectory order).
add_subdirectory(packages/particles)

# M6 P3: the animation package (src/packages/animation/, R-SYS-002 / R-SYS-008, issue #180) — skeletal
# animation with blending + a NEW authored ctx:anim-graph content kind + a deterministic root-motion sim
# path, with the determinism split (docs/physics-determinism-decision.md + R-SIM-001): the DETERMINISTIC
# SIM PATH (the anim-graph playback cursor + transition blend drives accumulated ROOT MOTION) is
# integer/fixed-point ONLY (the simmath Q16 core) and folds into the L-54 hierarchical state hash by
# stable name through the M6-F0b sim_components() seam, with deterministic fixed-point pose SAMPLE/BLEND;
# the FULL per-bone POSE is a presentation observer that reads sim state read-only, holds its own float
# pose, and folds into NO hash (the R-SIM-001 downstream-observer rule). Clips are DCC-import-only
# (R-ASSET-001). It is a PACKAGE, not kernel core (R-KERNEL-001 / L-60): the kernel never links back;
# animation needs no broad-phase, so — unlike the physics packages — it has NO context_spatial
# dependency. Registers the determinism-animation-scene ctest, joining the determinism-* family the
# blocking CI Determinism gate runs across the 3-OS matrix. Wired here immediately after
# packages/particles (its M6 WAVE sibling) and deliberately apart from the editor/runtime clusters and
# the file tail, so concurrent sibling tasks inserting into this file merge cleanly (consumers name the
# `context_animation` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(packages/animation)

# M6 P5: the spline package (src/packages/spline/, R-SYS-004, issue #182) — deterministic fixed-point
# paths/curves for movement, geometry, and tooling (the LAST M6 WAVE3 package), with the determinism
# split (docs/physics-determinism-decision.md + R-SIM-001): the DETERMINISTIC SIM PATH (a PathFollower
# advancing by arc length along a de Casteljau / Catmull-Rom fixed cubic, heading via deterministic
# fixed_atan2) is integer/fixed-point ONLY (the simmath Q16 core) and folds into the L-54 hierarchical
# state hash by stable name through the M6-F0b sim_components() seam; the tooling/geometry curve DISPLAY
# is a presentation observer that tessellates float polylines, reads sim state read-only, holds its own
# float display geometry, and folds into NO hash (the R-SIM-001 downstream-observer rule). It is a
# PACKAGE, not kernel core (R-KERNEL-001 / L-60): the kernel never links back; splines do not collide,
# so — unlike the physics packages — it has NO context_spatial dependency. Registers the
# determinism-spline-scene ctest, joining the determinism-* family the blocking CI Determinism gate runs
# across the 3-OS matrix. Wired here immediately after packages/animation (its M6 WAVE sibling) and
# deliberately apart from the editor/runtime clusters and the file tail, so concurrent sibling tasks
# inserting into this file merge cleanly (consumers name the `context_spline` target, resolved at
# generate time regardless of add_subdirectory order).
add_subdirectory(packages/spline)

# M6 P6: the audio package (src/packages/audio/, R-SYS-006 / L-46, issue #184) — the FIRST fully-observer
# package (no sim-path half). Audio is ENTIRELY a PRESENTATION OBSERVER (R-SIM-001), OFF the deterministic
# sim path: it READS sim state (positions for spatialization, event triggers) but NEVER writes sim state,
# registers NO sim component, and does NOT participate in the deterministic build or the L-54 state hash —
# its miniaudio audio thread + float mixing live entirely off the sim path (the m6-exit-5 seam invariant,
# proved by tests/test_observer_hash.cpp). Because it has no sim state there is NO determinism-*-scene
# ctest for audio and it is NOT on the `deterministic` job --target list in ci.yml. miniaudio (the L-46
# backbone) is vendored + compiled null-backend-only as its own warnings-suppressed sub-lib. It is a
# PACKAGE, not kernel core (R-KERNEL-001 / L-60): the kernel never links back. Wired here immediately
# after packages/spline (its M6 WAVE sibling) and deliberately apart from the editor/runtime clusters and
# the file tail, so concurrent sibling tasks inserting into this file merge cleanly (consumers name the
# `context_audio` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(packages/audio)

# M6 P7: the input package (src/packages/input/, R-SYS-007 / L-45, issue #186) — the LAST M6 WAVE4
# gameplay package. The authoring/mapping/routing FRONT-END: it maps raw device events (keyboard /
# mouse / gamepad / touch / VR-controller) through stackable action maps + a layered UI-capture stack
# (L-45) into the mapped action layer, with deterministic UI-vs-gameplay focus arbitration (R-SYS-007)
# + runtime rebinding. THE SEAM: it FEEDS the EXISTING sim-facing InputState sink (src/runtime/session/)
# — route() emits session::ActionActivation / TickInputs, the existing types — so there is NO forked
# sim-path input representation; it registers NO sim component of its own (tests/test_feed.cpp). Its
# device->action->InputState pipeline IS on the deterministic path, so it ships a determinism-input-scene
# ctest (on the `deterministic` job --target list in ci.yml). It is a PACKAGE, not kernel core (L-60):
# the kernel never links back. Wired here immediately after packages/audio (its M6 WAVE4 sibling) and
# apart from the editor/runtime clusters and the file tail, so concurrent sibling tasks inserting into
# this file merge cleanly (consumers name the `context_input` target, resolved at generate time
# regardless of add_subdirectory order).
add_subdirectory(packages/input)

# M7 T1: the runtime UI package (src/packages/ui/, R-UI-002/005/006, issue #223) — the HEADLESS
# foundation of the pluggable runtime UI system. A retained UiTree/UiNode (closed role vocabulary +
# CSS-like style props + visibility/opacity/transform), a pointer/focus/key/custom event + handler
# dispatch model, dirty/damage tracking computed IN the tree (damage coalescing), the backend-agnostic
# UiProvider contract + Capabilities struct (R-UI-005), and a null/headless provider. LOCKS D1/D2/D3/D6:
# it is a NEW runtime tree, borrowing the editor context_gui_uitree DESIGN (closed role vocab so
# R-A11Y-002 can hook it post-core) with ZERO link-level sharing; UI is presentation (D6) so it lives
# OUTSIDE the sim World and registers NO hashed sim component — at this foundation tier the package is
# PURE STDLIB and does not link the sim at all (the GPU backend in src/render/ui/ and the L-45 input glue
# arrive in later M7 tasks). It is a PACKAGE, not kernel core (L-60): the kernel never links back. Wired
# here after the packages cluster and apart from the runtime/editor clusters + the file tail, so
# concurrent sibling tasks inserting into this file merge cleanly (consumers name the `context_ui`
# target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(packages/ui)

# M3 task 2a: the in-process V8 JavaScript host (src/runtime/js/, issue #76 / L-61 /
# R-LANG-002/008 / R-SEC-009) — the foundation of the M3 TypeScript scripting tier. A hybrid host
# over the SHA-pinned rusty_v8 prebuilt channel (real v8:: API for the STL-free majority + rusty_v8
# extern-"C" shims for the two STL-crossing ops): Isolate+Context + trivial eval + host<->JS both
# ways, the shape-B VM-allocated (sandbox-interior) backing-store seam (task 3 builds zero-copy
# views on it), and a stubbed CDP inspector seam (R-OBS-005). V8 is CI-only for its dependency path
# (MSVC/Clang-ABI prebuilt — the local Strawberry-GCC dev gate builds a stub); the 3-OS CI build
# legs are the authoritative gate. OWN stanza — apart from the editor cluster + the exit-gate block
# below — so it never collides at the file tail (consumers name the `context_js` target, resolved at
# generate time regardless of add_subdirectory order).
add_subdirectory(runtime/js)

# M3-exit A / #71 (PR 1 of 4): the sandboxed WASM package-migration runner tier's INERT wasmtime
# C-API supply-chain leg (src/runtime/wasm/, L-37 / L-62 / R-SEC-009). Acquires + exposes the
# SHA-pinned wasmtime C-API prebuilt as the context_wasm target (tools/wasmtime-prebuilt.json +
# tools/fetch_wasmtime.py — TLS + SHA-256 pin + verify-before-use, fail-closed; the V8/rusty_v8 +
# wgpu-native + CEF third-party-prebuilt precedent). A CI-only dependency path GATED behind
# CONTEXT_BUILD_WASM (default OFF): the local Strawberry-GCC dev gate + the default 3-OS build matrix
# never fetch it (the subdir early-returns), so this add is inert for the routine gate. It links into
# NOTHING and runs NO wasm — the deterministic WasmRunner (which links context_wasm + flips the option
# ON) lands in PR 3; the src/editor/migrate/ MigrationRunner seam lands in PR 2 (untouched here). OWN
# stanza next to its runtime/js sibling — consumers name the `context_wasm` target, resolved at
# generate time regardless of add_subdirectory order.
add_subdirectory(runtime/wasm)

# M3 task 5: the R-SEC-005 engine-driven install path (src/editor/pkg/, issue #100) — the security
# envelope for the TS scripting tier's npm installs: pinned-version + lockfile-integrity enforcement
# (incl. transitive), scripts-requiring -> native-tier L-49 consent gate, and extract-without-scripts
# (--ignore-scripts, structural). Stdlib-only crypto (no license-gate dependency). OWN stanza — apart
# from the editor cluster + the exit-gate block below — so it never collides at the file tail
# (consumers name the `context_editor_pkg` target, resolved at generate time regardless of
# add_subdirectory order). Placed BEFORE runtime/ts so context_editor_pkg exists for the ts tier's
# install-resolves-in-the-host integration test link.
add_subdirectory(editor/pkg)

# M3 task 2b-i: the TypeScript toolchain (src/runtime/ts/, issue #83 / L-61 / R-LANG-002/004) —
# the build half of the M3 TS scripting tier. Transpiles + bundles authored .ts into a JS module
# the runtime/js V8 host evaluates, over the SHA-pinned esbuild prebuilt channel
# (tools/ts-toolchain.json + tools/fetch_esbuild.py; the same third-party-prebuilt precedent as
# rusty_v8). esbuild is a build-TIME subprocess (never linked), so context_ts builds on EVERY
# toolchain and its transpile ctest is a LOCAL gate; the in-V8 integration ctest links context_js
# and is CI-only for its V8 path. OWN stanza — apart from the editor cluster + the exit-gate block
# below — so it never collides at the file tail (consumers name the `context_ts` target, resolved
# at generate time regardless of add_subdirectory order). MUST follow add_subdirectory(runtime/js)
# so context_js exists for the integration test's link.
add_subdirectory(runtime/ts)

# M6 X1: the L-47 profiler substrate (src/runtime/profile/, R-SIM-008 / R-OBS-002) — the FIRST
# profiler channel in-tree: the GC-pause channel that makes JS-tier GC pauses observable and
# attributable against the R-LANG-012 frame budget (fed from the JsEngine GC pull seam; the
# R-SIM-008 inter-tick window is scheduled by runtime/session's inter-tick hook). The channel is
# backend-agnostic, so its unit ctest is a LOCAL gate on every toolchain. OWN stanza — apart from
# the editor cluster + the exit-gate block below — so it never collides at the file tail (consumers
# name the `context_profile` target, resolved at generate time regardless of add_subdirectory
# order). MUST follow add_subdirectory(runtime/js) so context_js exists for its PUBLIC link.
add_subdirectory(runtime/profile)

# M3 task 3b: the (query, executor) system model over the kernel World + derived TS accessor codegen
# (src/editor/system/, issue #90 / R-LANG-009 / R-LANG-010 item 1 / R-LANG-002; Part of #88) — the M3
# keystone that lets gameplay authored in TS mutate the shared World. Builds on the landed runSystemView
# zero-copy view-lifetime gate (runtime/js, #89) + declarative component storage (editor/component, #87):
# a system registers as a (query, executor) pair whose executor runs ONCE over zero-copy views of the
# queried declared components, with typed derived TS accessors over those views. The library builds on
# EVERY toolchain; only the keystone in-V8 ctest links context_js + context_ts (CI-only for its V8 path,
# like runtime/ts's in-V8 test). OWN stanza — apart from the editor cluster + the exit-gate block below —
# so it never collides at the file tail (consumers name the `context_system` target, resolved at generate
# time regardless of add_subdirectory order). MUST follow add_subdirectory(runtime/js) + runtime/ts so
# context_js / context_ts exist for the keystone test's link and the esbuild staging is available.
add_subdirectory(editor/system)

# M3 task 4a: the safe parallel scheduler (src/editor/schedule/, issue #92 / R-SIM-006 / R-LANG-011 /
# L-38; Part of #88) — from the systems' STATIC declared component read/write sets it derives a
# parallel schedule DAG ONCE and CACHES it (invalidated only on a composition change, never per frame):
# non-conflicting native/engine systems run in parallel batches, conflicting writes serialize, and TS
# systems are EXCLUDED from the DAG entirely (the single JS-VM lane, R-LANG-011). The core library
# names only ComponentId + std::thread, so its DAG/batching/parallel gates build + run on EVERY
# toolchain (and are the real subject of the ASan/UBSan + TSan CI legs); only the C++-and-TS-both-
# mutate-World keystone links context_js/context_ts/context_system and is CI-only for its V8 path. OWN
# stanza — apart from the editor cluster + the exit-gate block below — so it never collides at the file
# tail (consumers name the `context_schedule` target, resolved at generate time regardless of order).
# MUST follow add_subdirectory(editor/system) so context_system exists for the keystone test's link.
add_subdirectory(editor/schedule)

# M4 foundation: the render subsystem (src/render/, R-REND-001/002 / R-HEAD-002 / L-11/L-39/L-56,
# issue #114) — the tiered RHI abstraction (WebGPU T1 semantics) + the L-39 sim->render extract +
# double buffer + the detachable renderer (context_render, built + tested everywhere), plus the
# opt-in wgpu-native T1 backend + offscreen pixel-readback proof (context_render_wgpu, behind
# CONTEXT_BUILD_RENDER_WGPU — a CI-gated dependency path like V8). Rendering composes ON TOP of the
# kernel (R-KERNEL-002): render depends on context_kernel, never the reverse, so a headless build
# never pulls a GPU backend (R-HEAD-002). OWN stanza — deliberately apart from the editor/runtime
# cluster above and from the exit-gate block below — so concurrent sibling tasks inserting into this
# file merge cleanly (consumers name the `context_render` target, resolved at generate time
# regardless of add_subdirectory order).
add_subdirectory(render)

# M5-F0a foundation: the CEF editor-GUI BUILD SUBSTRATE (src/editor/cef/, L-15 / L-41 / R-UI-007 /
# R-SEC-009, issue #150) — fetch + SHA-verify the pinned CEF prebuilt (tools/fetch_cef.py) + boot it
# headless (a CEF browser subprocess boots + exits 0). Behind CONTEXT_BUILD_GUI_CEF (default OFF): a
# CI-only dependency path like V8 (src/runtime/js/) + wgpu-native (src/render/) — the MSVC/Clang-ABI
# CEF prebuilt cannot link under the local Strawberry-GCC dev gate, so the default matrix + local dev
# gate never enter it (the subdir early-returns when OFF). NO editor host / panels / RPC (that is
# M5-F0b / src/editor/gui/, which links this substrate). OWN stanza — deliberately apart from the
# editor/runtime cluster above and the exit-gate block below — so concurrent sibling tasks inserting
# into this file merge cleanly (consumers name targets resolved at generate time regardless of order).
add_subdirectory(editor/cef)

# M5-F0b foundation: the CEF editor HOST + R-EDIT-001 extension contract + headless UI-logic tree +
# minimal L-41 compositing seam + a11y hook (src/editor/gui/, issue #152; L-15 / L-41 / R-EDIT-001 /
# R-UI-007 / R-A11Y-001 / R-SEC-007). The three headless libs (uitree / contract / compositor) build +
# unit-test on the DEFAULT matrix; the CEF host is a CI-only dependency path behind CONTEXT_BUILD_GUI_CEF
# (the editor-cef-smoke job), linking the F0a CEF substrate above. Added AFTER editor/cef so the shared
# CEF acquisition (fetch + libcef_dll_wrapper) runs there first and the host reuses it. OWN stanza —
# deliberately apart from the editor/runtime cluster + the exit-gate block below — so concurrent sibling
# tasks inserting into this file merge cleanly (consumers name targets resolved at generate time
# regardless of add_subdirectory order).
add_subdirectory(editor/gui)

# M6 X2: the replication state-sync harness (src/runtime/netsync/, R-NET-001 / L-48, issue X2) — the
# layer that DRIVES the F0b in-storage L-48 replication metadata hooks (the kernel World's per-entity
# network identity == the L-37 composed id, authority, and dirty/delta versioning; F0b shipped the
# hooks and left the harness to X2). Provides the identity guard + dirty-scanner (delta culling) +
# snapshot capture + fail-closed atomic apply that make a REPLICA world CONVERGE to a SOURCE world
# over the L-48 dirty/delta — validated by an in-process two-session moving-body harness (dynamic
# context_physics3d / physics2d bodies replicate source -> replica and converge, R-NET-001 v1; NO
# network transport). It is a RUNTIME module composing on the kernel World (like runtime/session) and
# links ONLY context_kernel — physics-agnostic (the moving-body sim it replicates is a TEST-only dep;
# L-60: the kernel never links back). Fills the F0a-reserved net.* contract-catalog block. This is a
# CONVERGENCE/netcode harness, NOT a strict-FP determinism scene: it registers NO determinism-* ctest
# and adds nothing to ci.yml's `deterministic` --target list. OWN stanza — deliberately apart from the
# editor/runtime clusters above and the exit-gate block below — so concurrent sibling tasks inserting
# into this file merge cleanly (consumers name the `context_netsync` target, resolved at generate time
# regardless of add_subdirectory order).
add_subdirectory(runtime/netsync)

# M8 a06: the shipped RuntimeKernel HOST binary (src/runtime/host/, R-BUILD-009 / R-HEAD-001/002 / L-5,
# issue #259) — the FIRST standalone runtime executable. It boots a packed v1 artifact headlessly (the
# runtime content seam → a Session → step N ticks) in two DCE flavors: desktop (render subsystem present)
# and server/headless (render absent). MUST follow add_subdirectory(render) above (the desktop flavor
# links context_render) as well as runtime/session + runtime/content (both above). OWN stanza —
# deliberately apart from the editor/runtime clusters and the exit-gate block below — so concurrent
# sibling tasks inserting into this file merge cleanly (consumers name targets resolved at generate time
# regardless of add_subdirectory order).
add_subdirectory(runtime/host)

# The L-50 multi-client concurrency VALIDATION suite (issue #284, a16-l50): two deterministic
# integration tests red-teaming the shipped M1 concurrency + M2 structural-merge machinery, registered
# under the non-gate `concurrency-` ctest prefix (auto-run in the build job's general step on all 3 OS
# legs; no ci.yml change). Added here — before tests/integration — because it composes the inspector
# (L-30 engine), testing (fault harness), merge, cli, and session targets (all defined above), AND the
# M8.5 exit gate (tests/integration) registers ctest ALIASES of its two executables (m85-exit-1a/-1b),
# so those targets must exist when tests/integration is processed.
add_subdirectory(tests/concurrency)

# ---------------------------------------------------------------------------
# The M1 EXIT GATE (ROADMAP §1 M1 Exit / issue #36): five integration tests, one per exit
# criterion, registered as the `m1-exit-*` ctest family and CI-gated per the §6 tiering (blocking
# per-PR on the 3-OS build matrix — see the dedicated ci.yml step). Added LAST: the gate composes
# every M1 component above — kernel, filesync, derivation, contract, bridge, editorkernel, cli,
# testing — and spawns the real `context` binary, so all of their targets must already exist. This
# directory also hosts the M2/M4/M5/M6/M7/M8 and (a23) M8.5 exit gates; the M8.5 gates ALIAS the
# a16 concurrency executables (added just above) + the a20 contextual-help executable (editor/gui).
# ---------------------------------------------------------------------------
add_subdirectory(tests/integration)

# ---------------------------------------------------------------------------
# M9 e03 — the HEADLESS INVARIANT gate (design 03 §2; R-HEAD-002/004).
#
# "Nothing in the daemon/runtime links the present path; only the Shell does." Asserted here, on the
# REAL target graph, transitively — a violation fails the configure on every OS leg before anything
# compiles. Must be LAST: every audited target has to exist before its link closure can be walked.
#
# context_render (the GPU-free RHI abstraction + extract) is deliberately NOT forbidden: the desktop
# runtime flavor links it on purpose. What headless targets must never reach is the PRESENTATION
# path — windows, swapchains and OS blits (context_render_present) or a GPU backend
# (context_render_wgpu).
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextPresentIsolation.cmake)

set(_context_headless_targets
    context_runtime_server   # the server/headless export flavor — render subsystem absent entirely
    context_runtime_desktop  # the desktop flavor: links context_render, but NEVER presentation
    context                  # the CLI: a pure projection of the contract registry
    context_client)          # the installed/exported client SDK (M9 e02)
set(_context_present_isolation_report "${CMAKE_BINARY_DIR}/present-isolation-report.txt")

context_assert_no_present_linkage(
    REPORT "${_context_present_isolation_report}"
    FORBIDDEN context_render_present context_render_wgpu
    TARGETS ${_context_headless_targets})

# The ctest's expectation is spelled out LITERALLY below rather than reusing
# ${_context_headless_targets}. That independence is the whole point: an expectation derived from
# the same variable that drives the audit shrinks with it, so dropping a target would remove it from
# BOTH the audit and the check and the gate would stay green over a smaller and smaller set.

# The companion ctest. It cannot observe a violation (the configure above already failed on one) —
# it exists to catch the gate silently covering NOTHING, e.g. after a target rename, on EITHER side:
# an audited target that vanished, or a forbidden name that is no longer a target (whose closure
# checks then match nothing while still reporting "isolated"). Registered in the non-gate
# `render-present-` family, so the build job's general ctest step runs it on all three OS legs with
# no ci.yml --target bookkeeping.
#
# EXPECT_FORBIDDEN names only context_render_present: it is built unconditionally, whereas
# context_render_wgpu is legitimately absent unless CONTEXT_BUILD_RENDER_WGPU is ON.
add_test(NAME render-present-headless-isolation
    COMMAND ${CMAKE_COMMAND}
        -DREPORT=${_context_present_isolation_report}
        "-DEXPECT_AUDITED=context_runtime_server;context_runtime_desktop;context;context_client"
        -DEXPECT_FORBIDDEN=context_render_present
        -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/present_isolation_check.cmake)

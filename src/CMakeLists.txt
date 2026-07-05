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
option(CONTEXT_BUILD_SPIKES "Build the M0 de-risking spike targets under spikes/ (opt-in; the default CI matrix stays spike-free; some spikes require the vcpkg toolchain with VCPKG_MANIFEST_FEATURES=spikes)" OFF)
option(CONTEXT_BUILD_SPIKE_WASM "Build ONLY spikes/wasm (subset of CONTEXT_BUILD_SPIKES; acquires its own runtimes — needs no vcpkg toolchain), so CI can exercise the WASM spike without configuring the vcpkg-dependent spikes" OFF)
option(CONTEXT_BUILD_SPIKE_CEF "Build the CEF accelerated-OSR compositing spike (L-41) — downloads the pinned ~162 MB CEF binary distribution; Windows/MSVC-only, local runs only, NEVER enabled in CI" OFF)

# spikes/wasm builds WAMR (a C library): the C language must be enabled HERE — at top-level
# scope, BEFORE any add_subdirectory — not inside the spike's directory. Enabling C mid-tree
# under a chainloaded vcpkg toolchain leaves earlier-processed directories with a stale
# no-C-compiler snapshot and generation fails with "No known features for C compiler".
if(CONTEXT_BUILD_SPIKES OR CONTEXT_BUILD_SPIKE_WASM)
    enable_language(C)
endif()

# Warnings-as-errors baseline: the `context_warnings` interface target (../cmake/ContextWarnings.cmake).
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/ContextWarnings.cmake)

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

# M2 wave 2: the engine schema vocabulary + units law + per-kind versioned schemas (R-DATA-006,
# src/editor/schema/, issue #47) — x-ctx-type/x-ctx-storage/x-ctx-ref, the ONE tagged-union
# convention, SI+radians x-ctx-units, the compiled engine kind schemas (scene, project), and the
# document validator the derivation validate node runs. Wired here — deliberately apart from the
# serializer/derivation block above — so concurrent sibling tasks touching src/editor/ and this
# file merge cleanly (its consumers name the `context_schema` target, which CMake resolves at
# generate time regardless of add_subdirectory order).
add_subdirectory(editor/schema)

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
install(TARGETS context-hello context_kernel
    RUNTIME DESTINATION "${CONTEXT_INSTALL_SUBDIR}/bin"
    LIBRARY DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib"
    ARCHIVE DESTINATION "${CONTEXT_INSTALL_SUBDIR}/lib")
install(FILES "${_context_version_marker}" DESTINATION "${CONTEXT_INSTALL_SUBDIR}")

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

# M2 wave 3: the scene-composition READ path (src/editor/compose/, L-35/R-DATA-002, issue #51) —
# id-path overrides with innermost-out precedence, stable intra-file ids (L-33), the scene-root
# entity, the R-FILE-011(e) depth-cap/fan-in/cycle diagnostics, deterministic composed identity
# (L-37), and the R-CLI-006 provenance chains. The derivation graph's compose node links this
# target. Wired here — deliberately apart from the serializer/derivation and schema blocks above —
# so concurrent sibling tasks touching src/editor/ and this file merge cleanly (consumers name the
# `context_compose` target, which CMake resolves at generate time regardless of order).
add_subdirectory(editor/compose)

# M2 wave 4: player save-game groundwork (src/runtime/save/, R-DATA-005 / L-37, issue #66) — the
# FIRST src/runtime/ (RuntimeKernel) target. RuntimeKernel's OWN serialization (R-FILE-009 — never
# authored files): a per-component schemaVersion save header + composed-identity-addressed entity
# records (L-37) + a minimal save-migration runner that REUSES the editor's per-payload migration
# mechanism (context_migrate::migrate_payload) so a shipped build loads older saves through exactly
# the parse-time mechanism. OWN stanza inserted here — deliberately apart from the editor cluster
# above and before the M1 exit-gate block — so it never collides at the file tail (consumers name
# the `context_save` target, resolved at generate time regardless of add_subdirectory order).
add_subdirectory(runtime/save)

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

# ---------------------------------------------------------------------------
# The M1 EXIT GATE (ROADMAP §1 M1 Exit / issue #36): five integration tests, one per exit
# criterion, registered as the `m1-exit-*` ctest family and CI-gated per the §6 tiering (blocking
# per-PR on the 3-OS build matrix — see the dedicated ci.yml step). Added LAST: the gate composes
# every M1 component above — kernel, filesync, derivation, contract, bridge, editorkernel, cli,
# testing — and spawns the real `context` binary, so all of their targets must already exist.
# ---------------------------------------------------------------------------
add_subdirectory(tests/integration)

# src/editor/build/ — the M8 build orchestration core (R-BUILD-002 / R-BUILD-007, task a05). The pure,
# deterministic spine of the per-agent build pipeline: run_build drives verify → toolchain → aot →
# transcode → pack → link → adapter over in-memory inputs, producing a deterministic target pack (the
# real derive→transcode→pack chain over context_compose / context_import / context_pack) plus the
# R-KERNEL-003 generated-registration TU. The native AOT compile, the machine-code link, and the
# platform adapter are honestly stubbed until a06 (reported, never faked). The CLI (src/cli/
# build_command.cpp) wraps this with the disk IO. R-PKG-002 per-target toolchain manifest consumption
# lives here too (toolchain_manifest.*, mirroring cmake/toolchain-versions.json).
add_library(context_build STATIC
    src/build_orchestrator.cpp
    src/adapter.cpp
    src/signing.cpp
    src/doctor.cpp
    src/toolchain_manifest.cpp)

target_include_directories(context_build PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_compose / context_pack / context_import: the public build_orchestrator.h header exposes
# their types (compose::SceneResolver + ComposeLimits, pack::kDefaultEngineVersion, import::DerivedArtifact),
# so their include paths must propagate to consumers. context_compose links context_serializer PUBLIC,
# so the canonical JSON + JsonValue tree come through transitively. This is the ONE build-side target
# that binds compose + import + pack together — exactly the integration test_pack_transcode proves in a
# test; here it is the shipped orchestrator.
target_link_libraries(context_build PUBLIC context_compose context_pack context_import)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_build PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The R-QA-011 malformed/failure corpus for the build spine: the happy path (a real deterministic pack
# from a composed project + reproducibility), each of the five build.* codes reached from a real
# request, the a03 transcode.* wrapping, and the R-PKG-002 toolchain-manifest drift guard against the
# L-42 source of truth (cmake/toolchain-versions.json).
add_executable(context_build_test_build_orchestrator tests/test_build_orchestrator.cpp)
target_link_libraries(context_build_test_build_orchestrator
    PRIVATE context_build context_warnings)
target_include_directories(context_build_test_build_orchestrator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
# The drift guard reads the real L-42 toolchain manifest at test time (the R-PKG-002 consumption proof).
target_compile_definitions(context_build_test_build_orchestrator PRIVATE
    CONTEXT_TOOLCHAIN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/toolchain-versions.json")
# Self-contained GCC runtime: the test walks the manifest with std::ifstream; on this repo's Windows/GCC
# host such an exe must static-link the GCC runtime or a stale PATH libstdc++-6.dll aborts it at launch
# (targets/context-engine/setup.md). Gated to GNU so the MSVC/Clang CI legs are untouched.
target_link_options(context_build_test_build_orchestrator PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)
add_test(NAME build-test_build_orchestrator COMMAND context_build_test_build_orchestrator)

# a06/a10 export-adapter (adapter.*): the pure plan + launcher + manifest generators — the Linux + the
# a10 Windows flavors (desktop/server), the honest stub for uncovered targets, and the deterministic
# launcher (posix launch.sh / windows launch.cmd) + manifest text. No filesystem, so no static-runtime
# link needed (unlike the orchestrator drift guard).
add_executable(context_build_test_adapter tests/test_adapter.cpp)
target_link_libraries(context_build_test_adapter PRIVATE context_build context_warnings)
target_include_directories(context_build_test_adapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME build-test_adapter COMMAND context_build_test_adapter)

# a10 Authenticode signing hook (signing.*): the pure signing plan + report + PE-signature-presence
# parser. Covers the windows required plan, the not-required plan for other targets, the signed /
# unsigned (never-silent WARNING) states, and pe_has_authenticode_signature over synthetic PE fixtures
# (a signed vs unsigned Certificate Table, and malformed/non-PE inputs). No filesystem/subprocess.
add_executable(context_build_test_signing tests/test_signing.cpp)
target_link_libraries(context_build_test_signing PRIVATE context_build context_warnings)
target_include_directories(context_build_test_signing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME build-test_signing COMMAND context_build_test_signing)

# a09 environment doctor (doctor.*): the R-QA-011 corpus for the PURE diagnosis core — a healthy
# reference environment passes; deliberately-broken fixtures (a missing/wrong-version toolchain
# component, a starved file-sync watch budget, an absent signing prereq) produce the documented
# machine-readable findings + doctor.* codes. No filesystem/subprocess (the core is IO-free — the CLI
# wrapper owns the real host probing), so no static-runtime link is needed.
add_executable(context_build_test_doctor tests/test_doctor.cpp)
target_link_libraries(context_build_test_doctor PRIVATE context_build context_warnings)
target_include_directories(context_build_test_doctor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME build-test_doctor COMMAND context_build_test_doctor)

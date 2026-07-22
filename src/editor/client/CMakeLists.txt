# src/editor/client/ — context_client, the FIRST installed/exported library (M9 e02, D10/A-F1).
#
# The client half of the wire, single-sourced: endpoint discovery, the attach handshake (token +
# scopes + protocol negotiate), typed request helpers, and the R-CLI-015 subscription consumer. The
# `context` CLI rides it, and so does any out-of-tree consumer — the editor shell (e04) and e05's JS
# client host — which is what makes the D10 boundary real: the editor is an ORDINARY client that
# links the PUBLISHED surface, never kernel internals.
#
# Link surface is deliberately narrow (02 §3): context_contract (envelope/json/handshake/registry)
# publicly, context_bridge (transport) privately. NOTHING from editorkernel, filesync, derivation,
# compose, … — the include-graph gate in the `editor-boundary` CI job enforces exactly this.

add_library(context_client STATIC
    src/instance.cpp
    src/arbitration.cpp
    src/wire.cpp
    src/client.cpp
    src/subscription.cpp
    src/schema.cpp)

target_include_directories(context_client PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# context_contract is PUBLIC: its types appear in the SDK's public headers (client.h names
# contract::Json and contract::kProtocolMajor). context_bridge is PRIVATE: the transport channel is
# built over bridge::TransportClient, but ONLY inside client.cpp — no installed header names a bridge
# type, so bridge is link payload, not surface (see the install() rationale in src/CMakeLists.txt).
# For a STATIC library CMake records a PRIVATE dep as $<LINK_ONLY:...>, so consumers still link the
# archive transitively while getting none of its include paths — which is exactly the boundary.
# Every Context target links the warnings-as-errors baseline.
target_link_libraries(context_client
    PUBLIC context_contract
    PRIVATE context_bridge context_warnings)

# Self-contained GCC runtime for the client test/tool executables — they use std::filesystem +
# stream IO, whose libstdc++ symbols live in a NEWER libstdc++-6.dll; on a MinGW host a stale
# libstdc++-6.dll earlier on PATH can shadow the toolchain's and be loaded first, yielding a
# STATUS_ENTRYPOINT_NOT_FOUND (or a launch-time SegFault) even though build + link were clean. Gated
# to GNU so the macOS/Clang and Windows/MSVC CI legs are untouched (mirrors src/cli/CMakeLists.txt).
set(_context_client_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- the GENERATED client schema (R-CLI-009 spirit) ---------------------------------------------
# Hand-written client typings are prohibited: the registry is the single source of truth, so the
# client-facing schema is EMITTED BY THE BUILD from `describe`. e05a's JS client generator
# (tools/gen_client_typings.py) consumes the BUILD-generated artifact below — published as
# CONTEXT_CLIENT_SCHEMA_GENERATED and projected into the editor-core TypeScript typings by
# src/editor/webui — not the installed copy. The committed copy under schema/ is what the drift gate
# compares against.
add_executable(context_client_schema_gen tools/schema_gen_main.cpp)
target_link_libraries(context_client_schema_gen PRIVATE context_client context_warnings)
target_link_options(context_client_schema_gen PRIVATE ${_context_client_static_runtime})

# ⚠ Cleared at the top of src/CMakeLists.txt before the first add_subdirectory (a cache entry
# survives a reconfigure, which would defeat a consumer's ordering guard in a warm build dir) —
# adding another cross-directory CACHE INTERNAL handoff means adding it to that clear list too.
set(CONTEXT_CLIENT_SCHEMA_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/context-client-schema.json"
    CACHE INTERNAL "The build-generated client schema artifact")
set(_context_client_schema_committed "${CMAKE_CURRENT_SOURCE_DIR}/schema/context-client-schema.json")

add_custom_command(
    OUTPUT "${CONTEXT_CLIENT_SCHEMA_GENERATED}"
    COMMAND context_client_schema_gen "${CONTEXT_CLIENT_SCHEMA_GENERATED}"
    DEPENDS context_client_schema_gen
    COMMENT "context_client: generating the client schema from the contract registry (describe)"
    VERBATIM)
# ALL so a plain `cmake --build` produces the artifact — it is a build output, never a hand edit.
add_custom_target(context_client_schema ALL DEPENDS "${CONTEXT_CLIENT_SCHEMA_GENERATED}")

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/cli/ pattern);
# tests/client_test.h is the tiny CHECK() harness, tests/mock_channel.h the scripted wire.
set(_context_client_tests
    test_client
    test_arbitration
    test_subscription
    test_spawn_handshake
    test_schema)

foreach(_t IN LISTS _context_client_tests)
    add_executable(context_client_${_t} tests/${_t}.cpp)
    target_link_libraries(context_client_${_t} PRIVATE context_client context_warnings)
    target_link_options(context_client_${_t} PRIVATE ${_context_client_static_runtime})
    target_include_directories(context_client_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME client-${_t} COMMAND context_client_${_t})
endforeach()

# The drift gate needs the COMMITTED artifact's path (it compares it byte-for-byte against what the
# registry projects right now).
target_compile_definitions(context_client_test_schema
    PRIVATE CONTEXT_CLIENT_SCHEMA_COMMITTED="${_context_client_schema_committed}")

# --- cross-process e2e (R-QA-013): the LIVE half of T1 -------------------------------------------
# Spawns a REAL `context daemon` and drives it through context_client: subscribe -> snapshot -> a
# real edit's events -> acks, plus the D20 assertion that a TOKENLESS attach is denied while the
# discovered token attaches cleanly. Needs the built `context` binary's path as a compile-time define
# ($<TARGET_FILE:context> resolves per-config) and must build AFTER it — which is why this directory
# is added after add_subdirectory(cli) in src/CMakeLists.txt. Reuses the CLI's header-only
# spawn/wait/kill helper rather than growing a second copy.
add_executable(context_client_test_client_e2e tests/test_client_e2e.cpp)
target_link_libraries(context_client_test_client_e2e PRIVATE context_client context_warnings)
target_link_options(context_client_test_client_e2e PRIVATE ${_context_client_static_runtime})
target_include_directories(context_client_test_client_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../cli/tests)
target_compile_definitions(context_client_test_client_e2e
    PRIVATE CONTEXT_BINARY="$<TARGET_FILE:context>")
add_dependencies(context_client_test_client_e2e context)
add_test(NAME client-test_client_e2e COMMAND context_client_test_client_e2e)
# The daemon child can block if something goes wrong; cap the cross-process test so CI never hangs.
set_tests_properties(client-test_client_e2e PROPERTIES TIMEOUT 120)

# Sanitize preset only: the spawned `context` binary links context_js on the V8-capable legs, baking
# the uninstrumented rusty_v8 archive (and a second, hidden-visibility copy of the C++ runtime's
# typeinfo) into the child. UBSan's vptr check then false-positives on valid iostream code and
# PRINTING those diagnostics drives the demangler, whose scratch allocations LSan reports as leaks —
# failing the test with no Context defect. Same narrow suppressions the CLI e2e tests wire (see
# docs/sanitizer-v8-false-positives.md, issue #201); inert on the stub (non-V8) build.
if(CONTEXT_SANITIZE)
    set_tests_properties(client-test_client_e2e PROPERTIES ENVIRONMENT
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/ts/lsan-suppressions.txt")
endif()

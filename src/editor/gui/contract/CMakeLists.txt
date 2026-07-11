# src/editor/gui/contract/ — M5-F0b: the R-EDIT-001 editor-UI extension contract + the
# capability-scoped bridge shim (context_gui_contract, R-EDIT-001 / R-UI-007 / R-SEC-007, issue #152).
# The versioned panel/inspector/gizmo/asset-kind-editor registration surface (deny-by-default on the
# contract invariants) + the concrete CEF extension sandbox policy + the ExtensionBridge shim that
# clamps a panel to a scoped session over the ONE bridge dispatcher. Pure C++ over context_bridge — no
# CEF — so it builds + tests on the DEFAULT 3-OS matrix and the local dev gate.

add_library(context_gui_contract STATIC
    src/sandbox.cpp
    src/extension.cpp
    src/registry.cpp
    src/shim.cpp)

target_include_directories(context_gui_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# context_bridge is PUBLIC: sandbox.h/shim.h expose bridge::ScopeSet / bridge::Dispatcher / bridge::
# Session in the public headers (the capability-scoped bridge reuses the R-SEC-007 scope vocabulary +
# the ONE dispatcher — CLI ≡ RPC ≡ MCP ≡ extension by construction). context_bridge transitively
# brings context_contract (Envelope/Json/handshake). context_warnings is PRIVATE (no flag leak to the
# CEF host consumer — conventions.md). NO new third-party dependency (no vcpkg / license-allowlist
# change).
target_link_libraries(context_gui_contract
    PUBLIC context_bridge
    PRIVATE context_warnings)

# --- tests (R-QA-013) ---------------------------------------------------------------------------
# context_bridge's daemon/lock pull std::filesystem, so static-link the GCC runtime (gated to GNU) to
# keep the local ctest gate deterministic against a PATH-shadowed libstdc++-6.dll (setup.md).
set(_context_gui_contract_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_gui_contract_tests
    test_sandbox
    test_registry
    test_shim)

foreach(_t IN LISTS _context_gui_contract_tests)
    add_executable(context_gui_contract_${_t} tests/${_t}.cpp)
    target_link_libraries(context_gui_contract_${_t} PRIVATE context_gui_contract context_warnings)
    target_link_options(context_gui_contract_${_t} PRIVATE ${_context_gui_contract_static_runtime})
    target_include_directories(context_gui_contract_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME gui-contract-${_t} COMMAND context_gui_contract_${_t})
endforeach()

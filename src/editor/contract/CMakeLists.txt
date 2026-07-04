# src/editor/contract/ — the public-contract cluster (M1, R-CLI-007/008/009/013): the single
# registry that generates CLI ≡ RPC ≡ MCP ≡ introspection, the uniform result envelope + versioned
# error-code catalog, the whole-contract `describe` self-description, and the capability-negotiation
# handshake. The `context` CLI (src/cli/) is the first consumer; the bridge daemon (a later M1 task)
# consumes the same registry so the surfaces cannot drift.

add_library(context_contract STATIC
    src/json.cpp
    src/error_catalog.cpp
    src/envelope.cpp
    src/handshake.cpp
    src/registry.cpp
    src/resource_handle.cpp)

target_include_directories(context_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_contract PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo carries no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest (see the sibling
# src/kernel/ pattern). tests/contract_test.h is the tiny zero-dependency CHECK() harness.
set(_context_contract_tests
    test_json
    test_error_catalog
    test_envelope
    test_handshake
    test_registry_parity
    test_resource_handle)

foreach(_t IN LISTS _context_contract_tests)
    add_executable(context_contract_${_t} tests/${_t}.cpp)
    target_link_libraries(context_contract_${_t} PRIVATE context_contract context_warnings)
    target_include_directories(context_contract_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME contract-${_t} COMMAND context_contract_${_t})
endforeach()

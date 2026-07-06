# src/editor/component/ — declarative component-type authoring (M3 task 2b-ii: R-LANG-010, issue
# #86). Schema-first component definitions compiled into a runtime storage layout + a published,
# versioned schema, and registered into the kernel World's data-driven archetype/SoA storage with NO
# native rebuild (L-60). ONE declarative definition derives the ECS layout (offsets/size/align from
# the M2 x-ctx-storage vocabulary), the canonical-JSON payload encoding, the R-CLI-005 published
# schema `context describe` introspects, and a stable layout hash. Depends on the schema vocabulary
# (x-ctx-* law), the canonical serializer (parse + publish), and the kernel (raw World storage);
# deliberately contract-free — the contract registry consumes THIS module, never the reverse.

add_library(context_component STATIC
    src/component_type.cpp
    src/component_registry.cpp)

target_include_directories(context_component PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC: the public headers expose the serializer's JsonValue and the kernel's World/ComponentId.
target_link_libraries(context_component PUBLIC context_serializer context_kernel)
# PRIVATE: only the .cpp consumes the schema vocabulary (x-ctx-* law checks) — no schema type appears
# in the public headers.
target_link_libraries(context_component PRIVATE context_schema)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_component PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo carries no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest (the sibling module
# pattern; tests/component_test.h is the tiny CHECK() harness). Coverage spans the happy path (compile
# a definition, derive the layout, publish the schema, register + round-trip a payload through real
# World storage), edge cases (multi-lane vectors, partial-patch payloads, layout-hash sensitivity,
# archetype migration), and failure paths (every dialect + vocabulary-law rejection, out-of-range
# payload values, unknown fields).
set(_context_component_tests
    test_component_type
    test_component_registry)

foreach(_t IN LISTS _context_component_tests)
    add_executable(context_component_${_t} tests/${_t}.cpp)
    target_link_libraries(context_component_${_t} PRIVATE context_component context_warnings)
    target_include_directories(context_component_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME component-${_t} COMMAND context_component_${_t})
endforeach()

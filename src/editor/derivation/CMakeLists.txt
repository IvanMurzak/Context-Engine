# src/editor/derivation/ — the M1 incremental derivation graph (L-19/L-22, R-FILE-013, R-CLI-006):
# file changes (from the file-sync layer's ReconcileChange) → the canonical-parse node → the
# derived context::kernel::World, with content-hash memoized dirty-subgraph recompute, a monotonic
# derived-world generation counter, derivation-side backpressure (coalesced batched passes + a
# queue-depth / bounded-lag signal + load-shed to the visible/queried subgraph), and the
# read-your-writes query barrier. The canonical-parse node delegates to the REAL canonical-JSON
# serializer (src/editor/serializer/, M2 issue #42) behind the same stable seam the M1 placeholder
# occupied; the M2 wave-2 validate node (src/editor/schema/, R-DATA-006, issue #47) schema-checks
# bound payloads on the same parse, retaining last-good derived state on failure (R-FILE-003); the
# M2 wave-3 compose node (src/editor/compose/, L-35, issue #51) flattens scene composition over the
# graph's first cross-file dependency edges, re-flattening dependents when a template changes.

add_library(context_derivation STATIC
    src/canonical_parse.cpp
    src/derivation_graph.cpp
    src/shader_compile_node.cpp)

target_include_directories(context_derivation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Consumes the merged M1 deps read-only via their public headers: context_filesync for the
# ReconcileChange input type, context_kernel for the derived World + the internal EventBus the
# generation / backpressure events are published on. PUBLIC because the derivation graph's public
# headers expose those types, so consumers need their include paths. (context_filesync already links
# context_kernel PUBLIC; context_kernel is named explicitly here because this target uses World and
# EventBus directly.) context_serializer is PUBLIC too: canonical_parse.h exposes the serializer's
# Diagnostic type in CanonicalForm, so consumers need its include path. context_schema (M2 wave 2)
# powers the validate node: derivation_graph.h exposes SchemaSet/ValidationReport in its ctor and
# NodeValidation surface, so it is PUBLIC as well. context_compose (M2 wave 3) powers the compose
# node: derivation_graph.h exposes ComposeLimits/SceneDoc/ComposedScene in DerivationConfig and
# the ComposedView surface, so it is PUBLIC too.
target_link_libraries(context_derivation
    PUBLIC context_filesync context_kernel context_serializer context_schema context_compose)

# The R-FILE-010 shader-compile node (issue #126, src/shader_compile_node.cpp) re-homes the T3a
# content-addressed shader-compile cache behind the derivation-graph node contract: it wraps the
# backend-free IShaderCompiler seam from context_render_material. PUBLIC — shader_compile_node.h exposes
# render::material types (CompiledArtifact / ShaderIr / VariantKey / IShaderCompiler) in its API, so
# consumers need that target's include path. context_render_material is itself dependency-free (links
# only context_warnings), so this adds no native/GPU dependency to the derivation graph. CMake resolves
# the target at generate time regardless of add_subdirectory order (render is wired elsewhere in src/).
target_link_libraries(context_derivation PUBLIC context_render_material)

# The L-37 parse-time migration node (M2 wave 3, src/editor/migrate/): PRIVATE — the public header
# only forward-declares migrate::MigrationSet (an opaque seam pointer); the .cpp calls
# migrate_document on ingest. CMake resolves the target at generate time regardless of
# add_subdirectory order.
target_link_libraries(context_derivation PRIVATE context_migrate)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_derivation PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist entry);
# each test is a plain assertion-bearing executable registered with ctest. See tests/derivation_test.h.
# Coverage spans the happy path (change -> derived World + generation), edge cases (memoization,
# coalescing, idempotent removal, empty pass), and failure paths (barrier timeout, load-shed deferral).
set(_context_derivation_tests
    test_canonical_parse
    test_derivation_graph
    test_incremental
    test_backpressure
    test_query_barrier
    test_validate_node
    test_compose_node
    test_migrate_node
    test_shader_compile_node)

foreach(_t IN LISTS _context_derivation_tests)
    add_executable(context_derivation_${_t} tests/${_t}.cpp)
    target_link_libraries(context_derivation_${_t} PRIVATE context_derivation context_warnings)
    target_include_directories(context_derivation_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME derivation-${_t} COMMAND context_derivation_${_t})
endforeach()

# test_migrate_node registers its own migration steps, so it needs the migrate module's headers
# directly (context_derivation links it PRIVATE — the seam is an opaque pointer in the public API).
target_link_libraries(context_derivation_test_migrate_node PRIVATE context_migrate)

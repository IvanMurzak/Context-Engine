# src/packages/spatial/ — the broad-phase spatial acceleration structure (R-SIM-007; M4 wave-1,
# issue #115). The FIRST real first-party package under src/packages/ (the dir was a README-only
# placeholder). An incrementally-updated dynamic AABB tree (dynamic BVH) over transform-bearing
# entities: insert/update/remove are O(log N) and kept current as bounds change (never rebuilt per
# frame), and radius + AABB queries are O(result + log N). One index is designed to be shared by three
# future consumers wired in later waves — render culling (L-39), spatial queries (R-CLI-006), asset
# streaming (R-ASSET-003).
#
# It is a PACKAGE, not kernel core: R-KERNEL-001 stays minimal. It composes ON the kernel's entity
# identity (context_kernel's Entity) and the kernel gains NO dependency on it (the microkernel
# invariant) — nothing under src/kernel/ links context_spatial.

add_library(context_spatial STATIC
    src/spatial_index.cpp)

target_include_directories(context_spatial PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: the public header (spatial_index.h) exposes context::kernel::Entity as the
# index's handle type, so consumers of context_spatial need the kernel's include dir. This is a
# package composing on the kernel — the dependency direction the microkernel model prescribes; the
# kernel never links back.
target_link_libraries(context_spatial PUBLIC context_kernel)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_spatial PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). Coverage:
#   - test_spatial_index: insert/update/remove correctness + radius/AABB query membership across the
#     happy path, edge cases (empty index, single entity, degenerate/inverted/point bounds, clear),
#     and failure paths (remove/update-absent, invalid entity, negative radius);
#   - test_spatial_index_perf: deterministic sub-linear query-scaling + logarithmic-height sanity
#     (via the last_query_visited_nodes cost metric — no wall-clock timing, so it is CI-stable),
#     including after a bulk incremental update.
set(_context_spatial_tests
    test_spatial_index
    test_spatial_index_perf)

foreach(_t IN LISTS _context_spatial_tests)
    add_executable(context_spatial_${_t} tests/${_t}.cpp)
    target_link_libraries(context_spatial_${_t} PRIVATE context_spatial context_warnings)
    target_include_directories(context_spatial_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME spatial-${_t} COMMAND context_spatial_${_t})
endforeach()

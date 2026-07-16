# src/runtime/content/ — the M8 task a02 RuntimeKernel async streaming content LOADER (R-ASSET-005 /
# R-ASSET-003 / R-FILE-009 / L-24, docs/chunk-pack-format.md): the runtime half of R-ASSET-005 —
# async load / instantiate / unload of packed content units by GUID, a memory-budgeted streaming
# scheduler with proximity-driven decisions (R-SIM-007), and the one loading seam fed BOTH by a v1
# pack (shipped builds) and the live in-editor derived units (play-in-editor). Consumes the frozen v1
# pack format the a01 writer produces; parses it WITHOUT the editor writer's context_compose (L-24 —
# no editor/derivation code in a RuntimeKernel build) by depending only on the frozen byte-layout
# constants (context_pack_format) + the canonical serializer.

add_library(context_runtime_content STATIC
    src/pack_source.cpp
    src/editor_source.cpp
    src/content_loader.cpp
    src/streaming_scheduler.cpp)

target_include_directories(context_runtime_content PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_serializer: the public headers expose the serializer JsonValue tree (a materialized
# UnitEntity carries its composed component payload as a JsonValue). PRIVATE context_pack_format: only
# pack_source.cpp reads the frozen v1 byte-layout constants + LE primitives (no pack type appears in a
# public content header) — so the runtime loader never pulls context_compose (L-24 layering).
target_link_libraries(context_runtime_content PUBLIC context_serializer)
target_link_libraries(context_runtime_content PRIVATE context_pack_format)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_runtime_content PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing ctest executables (the repo carries no C++ test framework; tests/content_test.h
# is the tiny CHECK() harness). The tests build real v1 packs from in-test scenes via the a01 writer
# (context_pack) + the flatten/partition path (context_compose) — a TEST-ONLY link of the editor build
# side, so context_runtime_content itself never depends on it (L-24 preserved). The family is
# `runtime-content-*`: NOT a milestone gate prefix, so it auto-runs in the CI `build` job's general
# ctest step and is auto-built by --preset dev — no ci.yml / --target edit needed.
set(_content_static_runtime $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_content_tests
    test_pack_source        # load/instantiate/unload by GUID from a packed archive + failure paths
    test_content_loader     # residency isolation, unload releases residency, async pump, handles
    test_feed_parity        # editor-fed vs pack-fed: identical derived world, identical state hash
    test_streaming_budget)  # a synthetic over-budget world stays within the configured budget

foreach(_t IN LISTS _content_tests)
    add_executable(context_runtime_content_${_t} tests/${_t}.cpp)
    target_link_libraries(context_runtime_content_${_t}
        PRIVATE context_runtime_content context_pack context_compose context_serializer context_warnings)
    target_link_options(context_runtime_content_${_t} PRIVATE ${_content_static_runtime})
    target_include_directories(context_runtime_content_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME runtime-content-${_t} COMMAND context_runtime_content_${_t})
endforeach()

# The proximity test additionally links context_spatial: it wires the R-SIM-007 spatial index
# (context_spatial) to the ProximityProvider seam through a test-only adapter — the "asset streaming
# depends on the index later" consumer the spatial package README names. A TEST-ONLY link (the
# scheduler library stays free of the package dep — L-60 layering, mirroring runtime/session's
# determinism_gameplay gate).
set(_content_spatial_tests
    test_streaming_proximity)  # proximity-driven load/unload via the real SpatialIndex

foreach(_t IN LISTS _content_spatial_tests)
    add_executable(context_runtime_content_${_t} tests/${_t}.cpp)
    target_link_libraries(context_runtime_content_${_t}
        PRIVATE context_runtime_content context_pack context_compose context_serializer
                context_spatial context_kernel context_warnings)
    target_link_options(context_runtime_content_${_t} PRIVATE ${_content_static_runtime})
    target_include_directories(context_runtime_content_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME runtime-content-${_t} COMMAND context_runtime_content_${_t})
endforeach()

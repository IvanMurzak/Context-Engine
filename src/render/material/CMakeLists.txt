# src/render/material/ — the backend-free material/shader AUTHORING layer (R-REND-005, first slice;
# issue #121, Part of #119). Two GPU-/toolchain-free pieces, so they build + ctest under EVERY
# toolchain including the local Ninja+Strawberry-GCC Windows dev gate (no native shader toolchain yet):
#   * the authoring IR + shader-variant (keyword permutation) enumeration + canonical (de)serialize;
#   * the IShaderCompiler seam + a deterministic fake/reference backend — the real glslang/SPIRV-Cross
#     backend lands BEHIND this seam in a later sub-task (sub-task C), never touching callers.
# The R-FILE-010 content-addressed shader-compile cache was RE-HOMED (issue #126, Part of #119) behind
# a first-class derivation-graph node — context::editor::derivation::ShaderCompileNode in
# src/editor/derivation/ — so shader compilation is a keyed/cached/invalidated/backpressured derived
# artifact like any other, not a standalone side cache. This layer keeps only the backend-free seam it
# wraps. Deliberately dependency-free (only context_warnings): the authoring IR needs neither the RHI
# abstraction nor a GPU backend, so it never pulls context_render/context_kernel — that keeps the local
# gate green with no native shader toolchain (which arrives in sub-tasks B/C behind the seam).

add_library(context_render_material STATIC
    src/material_ir.cpp
    src/shader_compiler.cpp)

target_include_directories(context_render_material PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_render_material PRIVATE context_warnings)

# --- tests (R-QA-013: behaviour ships WITH its tests in the same PR) -----------------------------
# Plain assertion executables registered with ctest (the sibling module pattern; tests/material_test.h
# is the CHECK() harness — the repo carries no C++ test framework). Coverage: IR round-trip over the
# real authored corpus, variant enumeration (incl. the no-keyword + multi-value edges + a bad-parse
# failure family), compile-via-seam determinism, and R-FILE-010 cache-hit behaviour. All four build +
# run under the local GCC dev gate. (The R-FILE-010 cache-hit coverage moved WITH the cache to the
# derivation node — derivation-test_shader_compile_node — when it was re-homed, issue #126.)
# Self-contained GCC runtime for the ctest executables: test_material_ir reads the corpus via
# std::ifstream/std::ostringstream, which pull newer libstdc++ symbols; on this MinGW host a stale
# libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be loaded first, crashing the exe
# at run time even though the build + link were clean (setup.md § static-link note). Static libgcc +
# libstdc++ make the test binaries self-contained. Gated to GNU so the macOS/Clang + Windows/MSVC CI
# legs are untouched (mirrors src/editor/filesync, kinds, bridge, editorkernel, cli).
set(_context_render_material_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

set(_context_render_material_tests
    test_material_ir
    test_variants
    test_compile)
foreach(_t IN LISTS _context_render_material_tests)
    add_executable(context_render_material_${_t} tests/${_t}.cpp)
    target_link_libraries(context_render_material_${_t}
        PRIVATE context_render_material context_warnings)
    target_link_options(context_render_material_${_t}
        PRIVATE ${_context_render_material_static_runtime})
    target_include_directories(context_render_material_${_t}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME render-material-${_t} COMMAND context_render_material_${_t})
endforeach()

# The IR round-trip test parses the REAL authored corpus (corpus/*.shader). Hand it the corpus dir as
# an ABSOLUTE compile definition so the test never depends on the ctest working directory.
target_compile_definitions(context_render_material_test_material_ir
    PRIVATE CONTEXT_MATERIAL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/corpus")

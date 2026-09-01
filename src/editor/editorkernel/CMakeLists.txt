# src/editor/editorkernel/ — the M1 EditorKernel composition. Wires the merged file-authoritative
# libraries into ONE runnable headless attach path (R-ARCH-005 / R-BRIDGE-008): file -> filesync
# watch-hash-reconcile -> incremental derivation -> derived context::kernel::World (+ generation) ->
# bridge daemon (single-instance lock + client event stream + R-SEC-007 scope dispatch). It COMPOSES
# the merged components via their public headers; it does not re-implement them.

add_library(context_editorkernel STATIC
    src/editor_kernel.cpp
    src/editor_session_state.cpp
    src/kernel_server.cpp)

target_include_directories(context_editorkernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# All five composed libraries are PUBLIC: the EditorKernel's public header exposes their types
# (bridge Daemon/StartOutcome/ScopeSet, derivation DerivationGraph/WriteTicket/BarrierResult, filesync
# Reconciler/ReconcileChange, contract Envelope, kernel World), so a consumer needs their include
# paths. context_derivation already links context_filesync + context_kernel, and context_bridge links
# context_contract + context_kernel, so several are transitively available; they are named explicitly
# because this target uses each directly. context_schema is PRIVATE: editor_kernel.cpp names
# schema::engine_schemas() directly (previously reached only transitively via context_derivation)
# while the public header exposes no schema type. Every Context target links the warnings-as-errors
# baseline.
target_link_libraries(context_editorkernel
    PUBLIC context_bridge context_derivation context_filesync context_contract context_kernel
    PRIVATE context_schema context_warnings)

# The L-37 migration set + tool-save stamping (M2 wave 3, src/editor/migrate/): PUBLIC — the
# config header exposes migrate::MigrationSet (as an opaque seam pointer; only forward-declared),
# and the .cpp calls migrate_document/hash_combine on the save paths.
target_link_libraries(context_editorkernel PUBLIC context_migrate)

# M9 e05d3 — the `editor.scene-tree` / `editor.inspect` composed reads (D17/D18): kernel_server.cpp
# builds the boundary-clean panel models DAEMON-SIDE (context_gui_panel_builders, over the same
# disk-backed compose path `context set` plans against — context_compose_project) and answers them
# as wire data, so the Shell's panels hydrate with NO kernel module on the editor's own closure.
# PRIVATE: nothing in the public headers exposes these types — this is serving machinery.
target_link_libraries(context_editorkernel
    PRIVATE context_gui_panel_builders context_compose_project)

# M9 e1 — the `editor.files` project-file-tree read (D10 read half): kernel_server.cpp scans the
# project via a fresh assetdb::AssetDatabase (over a NativeFileStore, the same "always current
# on-disk truth" discipline the scene-tree/inspect reads follow) and hands the result to
# gui::panels::builders::build_files_model. context_assetdb was ALREADY transitively reachable
# through context_gui_panel_builders' PUBLIC interface; named EXPLICITLY per this file's standing
# rule, because this target now uses assetdb::AssetDatabase/RandomGuidGenerator directly. PRIVATE:
# no public header here exposes an assetdb:: type.
target_link_libraries(context_editorkernel PRIVATE context_assetdb)

# M9 e09b-1 — the pointer/value (composed) `edit` mode: kernel_server.cpp now also drives the WRITE
# half of composition (compose::plan_write + the canonical serializer) over that same disk-backed
# resolver, so the editor can issue the design 05 §8 field-addressed write it structurally cannot
# compose itself (context_compose is D10-FORBIDDEN to the Shell). Both were already reachable
# transitively — through context_derivation's PUBLIC interface, the same provenance the
# context_schema note above cites (context_compose_project is linked PRIVATE, so its interface does
# NOT propagate) — and are named EXPLICITLY per this file's standing rule, because this target now
# uses each directly. The visibility split follows the same rule the PUBLIC block states:
# context_serializer is PUBLIC because editor_kernel.h exposes serializer::Diagnostic
# (EditOutcome::encoding_diagnostics), while no public header names a compose:: type.
target_link_libraries(context_editorkernel PUBLIC context_serializer)
target_link_libraries(context_editorkernel PRIVATE context_compose)

# Self-contained GCC runtime for the editorkernel ctest executables. The composition pulls the bridge
# daemon's single-instance lock (std::filesystem) and thus newer libstdc++ symbols; on a MinGW host a
# stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's and be loaded first, yielding
# STATUS_ENTRYPOINT_NOT_FOUND at launch. Static libgcc + libstdc++ make the test binaries
# self-contained and the local ctest gate deterministic. Gated to GNU so the macOS/Clang and
# Windows/MSVC CI legs (libc++ / MSVC runtime) are untouched (mirrors src/editor/bridge, src/cli).
set(_context_editorkernel_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# Plain assertion-bearing executables registered with ctest (the sibling src/editor/bridge pattern);
# tests/editorkernel_test.h is the tiny CHECK() harness. The composed-loop integration smoke lives in
# test_editor_kernel (attach handshake + CLI-verb edit + raw edit + read-your-writes barrier +
# scope-denial exit-class 6).
set(_context_editorkernel_tests
    test_editor_kernel
    test_editor_kernel_native
    test_editor_session_state
    test_kernel_server)

foreach(_t IN LISTS _context_editorkernel_tests)
    add_executable(context_editorkernel_${_t} tests/${_t}.cpp)
    # context_assetdb: test_kernel_server.cpp writes real `<asset>.meta.json` sidecars via
    # assetdb::serialize_meta for its `editor.files` fixture (M9 e1) rather than hand-rolling the
    # schema — linked uniformly, the sibling context_gui_panel_builders-in-every-test pattern.
    target_link_libraries(context_editorkernel_${_t}
        PRIVATE context_editorkernel context_assetdb context_warnings)
    target_link_options(context_editorkernel_${_t} PRIVATE ${_context_editorkernel_static_runtime})
    target_include_directories(context_editorkernel_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME editorkernel-${_t} COMMAND context_editorkernel_${_t})
endforeach()

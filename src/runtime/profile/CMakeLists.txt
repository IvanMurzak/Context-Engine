# src/runtime/profile/ — the L-47 profiler substrate (M6 X1, R-SIM-008 / R-OBS-002). L-47 was
# greenfield until X1: this module hosts the FIRST profiler channel — the GC-pause channel that
# makes JS-tier GC pauses observable/attributable against the R-LANG-012 frame budget. It consumes
# the JsEngine GC pull seam (drainGcPauses / gcPausesDropped), which is a pure virtual interface:
# linking context_js supplies the backend (the real V8 engine on the CI legs, the stub locally —
# the channel itself is backend-agnostic and its unit test runs on EVERY toolchain). L-47's fuller
# shape (per-system spans, Tracy/RenderDoc export, the HUD, `context profile` growth) accretes here.

add_library(context_profile STATIC
    src/gc_channel.cpp)

target_include_directories(context_profile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_js: gc_channel.h names js::JsEngine / js::GcPause in its public API (the drain
# seam), so consumers need the js include path; on non-V8 toolchains context_js is the tiny stub.
# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_profile PUBLIC context_js)
target_link_libraries(context_profile PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -----------------------------
# The channel's unit test is engine-free (samples are recorded directly), so it is a LOCAL gate on
# every toolchain — the engine-fed path is covered by js-test_gc_discipline / js-test_gc_state_hash.
add_executable(context_profile_test_gc_channel tests/test_gc_channel.cpp)
target_link_libraries(context_profile_test_gc_channel PRIVATE context_profile context_warnings)
target_include_directories(context_profile_test_gc_channel
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME profile-test_gc_channel COMMAND context_profile_test_gc_channel)

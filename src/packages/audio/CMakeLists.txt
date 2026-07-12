# src/packages/audio/ — the audio system package (M6 P6, R-SYS-006 / L-46, issue #184), the FIRST
# fully-observer package: audio is ENTIRELY a PRESENTATION OBSERVER (R-SIM-001), OFF the deterministic
# sim path. It READS sim state (entity positions, event triggers) but NEVER writes sim state, registers
# NO sim component, and does NOT participate in the deterministic build or the L-54 hierarchical state
# hash — its miniaudio audio thread + float mixing (audio_engine.cpp) live entirely off the sim path
# (the m6-exit-5 seam invariant, proved by tests/test_observer_hash.cpp). Because it has no sim state,
# there is NO determinism-*-scene ctest for audio and it is NOT on the `deterministic` job --target list.
#
# It is a PACKAGE, not kernel core (L-60 microkernel invariant): it composes on the kernel World as a
# read-only observer, and the kernel gains NO dependency on it — nothing under src/kernel/ links
# context_audio.

# --- vendored miniaudio: its OWN warnings-suppressed static lib (third-party single-header C) ------
# miniaudio (third_party/miniaudio/miniaudio.h, pinned v0.11.22, MIT-0 — recorded in
# tools/license-allowlist.json) is compiled here as an isolated target that does NOT link the
# context_warnings baseline and disables ALL warnings, so its ~90k-line single header builds clean
# under our -Werror / /WX gate without editing upstream. Only the NULL device backend is enabled: CI
# runners have no audio hardware and the tests never open a real device (R-SYS-006 CI = null backend).
add_library(context_audio_miniaudio STATIC src/miniaudio_impl.cpp)

# SYSTEM PUBLIC: the vendored header is a system include (suppresses its warnings) that context_audio's
# audio_engine.cpp also needs; PUBLIC so that TU sees the same include when it includes the declarations.
target_include_directories(context_audio_miniaudio SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)

# Null backend only + no high-level engine / resource-manager / decoders / encoders / generators (this
# package does its own low-level float mixing). PUBLIC so the declarations audio_engine.cpp includes
# match this implementation TU exactly (identical backend/feature configuration).
target_compile_definitions(context_audio_miniaudio PUBLIC
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS
    MA_ENABLE_NULL
    MA_NO_ENGINE
    MA_NO_RESOURCE_MANAGER
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_GENERATION)

# Third-party code: suppress ALL warnings on its impl TU (it deliberately does NOT link
# context_warnings) so it compiles clean under the repo's warnings-as-errors baseline.
if(MSVC)
    target_compile_options(context_audio_miniaudio PRIVATE /w)
else()
    target_compile_options(context_audio_miniaudio PRIVATE -w)
endif()

# The null device backend runs miniaudio's own audio thread and does math; link the platform
# threading / dynamic-loader / math libs it needs. No hardware audio libraries are linked because only
# the null backend is enabled.
find_package(Threads REQUIRED)
target_link_libraries(context_audio_miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(context_audio_miniaudio PUBLIC m)
endif()

# --- the audio package ----------------------------------------------------------------------------
add_library(context_audio STATIC
    src/audio_engine.cpp
    src/observer.cpp)

target_include_directories(context_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC context_kernel: the public headers (observer.h) expose the kernel World/Entity the observer
# reads read-only. The dependency direction is the package composing on the kernel; the kernel never
# links back (L-60).
target_link_libraries(context_audio PUBLIC context_kernel)

# PRIVATE context_audio_miniaudio: the vendored device backbone, used ONLY inside audio_engine.cpp
# (PIMPL) — miniaudio never leaks into the package's public headers, so it stays private (its symbols
# and platform link libs still reach the final link transitively as a static-lib dependency).
target_link_libraries(context_audio PRIVATE context_audio_miniaudio)

# PRIVATE context_session: the observer reads the session built-in Position sim component READ-ONLY to
# drive spatialization; only observer.cpp names it. Audio registers NO sim component and writes NO sim
# state (R-SIM-001) — this dependency is strictly a read of the const World.
target_link_libraries(context_audio PRIVATE context_session)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_audio PRIVATE context_warnings)

# The test executables link context_session (hash_world / sim_components / Position in assertions); on
# this MinGW host a stale libstdc++-6.dll elsewhere on PATH can shadow the toolchain's, so they
# static-link the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC untouched; the same
# self-contained-binary rationale as runtime/session and the sibling packages).
set(_context_audio_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) --------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo
# carries no C++ test framework). tests/audio_test.h is the tiny CHECK() harness. Coverage:
#   - test_engine: the miniaudio null-backend device lifecycle (init/start/stop) + the float mixer
#     (per-bus resolved gain, spatial distance attenuation + equal-power pan, frame-rate independence
#     R-SYS-006 across block sizes) + the device-thread liveness (frames_mixed advances) + the
#     fail-closed invalid-bus / invalid-event refusals;
#   - test_observer: the observer reads sim positions read-only, drives the engine (listener + trigger),
#     the audio.invalid_entity refusal on a dead entity, and the world left byte-identical;
#   - test_observer_hash: the R-SIM-001 / m6-exit-5 PRESENTATION-OBSERVER invariant — arbitrary audio
#     activity (INCLUDING the running audio thread) leaves the sim state hash byte-identical, never
#     writes sim state, and registers no sim component, while doing real (non-vacuous) work;
#   - test_errors: the fail-closed audio.* refusals, pinned to the contract catalog's audio.* block.
set(_context_audio_tests
    test_engine
    test_observer
    test_observer_hash
    test_errors)

foreach(_t IN LISTS _context_audio_tests)
    add_executable(context_audio_${_t} tests/${_t}.cpp)
    target_link_libraries(context_audio_${_t}
        PRIVATE context_audio context_session context_warnings)
    target_link_options(context_audio_${_t} PRIVATE ${_context_audio_static_runtime})
    target_include_directories(context_audio_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME audio-${_t} COMMAND context_audio_${_t})
endforeach()

# The device-thread liveness tests spin briefly waiting on the null backend's audio thread; give them
# generous headroom over the default so a slow CI runner (sanitizer/TSan legs) never times out.
set_tests_properties(audio-test_engine audio-test_observer_hash PROPERTIES TIMEOUT 120)

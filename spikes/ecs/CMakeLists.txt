# spikes/ecs — ECS evaluation spike: EnTT vs flecs vs a thin custom archetype/SoA sketch.
#
# OPT-IN ONLY: this directory is added solely when CONTEXT_BUILD_SPIKES=ON (OFF by default —
# the default CI matrix never builds it). Requires the vcpkg toolchain with the `spikes`
# manifest feature so entt/flecs are installed:
#
#   cmake -S src --preset spikes                  # from the repo root; needs VCPKG_ROOT in the environment
#   cmake --build src/build/spikes --config Release
#   ctest --test-dir src/build/spikes -C Release  # runs the zero-copy proof
#
# The three benchmark executables (ecs-spike-entt / -flecs / -custom) are run manually;
# results and the scoring matrix live in FINDINGS.md.

find_package(EnTT CONFIG REQUIRED)
find_package(flecs CONFIG REQUIRED)

set(spike_ecs_flecs_target $<IF:$<TARGET_EXISTS:flecs::flecs>,flecs::flecs,flecs::flecs_static>)

add_executable(ecs-spike-entt entt/main.cpp)
target_link_libraries(ecs-spike-entt PRIVATE context_warnings EnTT::EnTT)

add_executable(ecs-spike-flecs flecs/main.cpp)
target_link_libraries(ecs-spike-flecs PRIVATE context_warnings ${spike_ecs_flecs_target})

add_executable(ecs-spike-custom custom/main.cpp)
target_link_libraries(ecs-spike-custom PRIVATE context_warnings)

add_executable(ecs-spike-zerocopy zerocopy/main.cpp)
target_link_libraries(ecs-spike-zerocopy PRIVATE context_warnings ${spike_ecs_flecs_target})

foreach(t ecs-spike-entt ecs-spike-flecs ecs-spike-custom ecs-spike-zerocopy)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# MSVC C4702 (unreachable code) fires INSIDE flecs' C++ headers at optimization time;
# back-end warnings are not silenced by /external:W0, so suppress that single code on the
# flecs-consuming targets only (keeps /W4 /WX for our own diagnostics).
if(MSVC)
    target_compile_options(ecs-spike-flecs PRIVATE /wd4702)
    target_compile_options(ecs-spike-zerocopy PRIVATE /wd4702)
endif()

# Fast functional gate (sub-second): the zero-copy aliasing proof must hold.
add_test(NAME ecs-spike-zerocopy-runs COMMAND ecs-spike-zerocopy)

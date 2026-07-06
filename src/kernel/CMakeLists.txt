# src/kernel/ — the microkernel core (M1, R-KERNEL-001): World/ECS, fixed-timestep Scheduler,
# module registry, event bus, resource-handle registry, and the platform seam. The ~6 stable
# interfaces every other engine component composes onto as packages (R-KERNEL-002).

add_library(context_kernel STATIC
    src/world.cpp
    src/scheduler.cpp
    src/module.cpp
    src/event_bus.cpp
    src/platform.cpp
    src/kernel.cpp)

target_include_directories(context_kernel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Every Context target links the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_kernel PRIVATE context_warnings)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) ----------------------------
# The repo has no C++ test framework (adding one would pull a vcpkg dep + a license-allowlist
# entry); each test is a plain assertion-bearing executable registered with ctest. See
# tests/kernel_test.h. Each interface gets its own ctest entry (happy path + edge + failure paths).
set(_context_kernel_tests
    test_world
    test_world_raw
    test_scheduler
    test_module_registry
    test_event_bus
    test_resource
    test_platform
    test_kernel)

foreach(_t IN LISTS _context_kernel_tests)
    add_executable(context_kernel_${_t} tests/${_t}.cpp)
    target_link_libraries(context_kernel_${_t} PRIVATE context_kernel context_warnings)
    target_include_directories(context_kernel_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME kernel-${_t} COMMAND context_kernel_${_t})
endforeach()

# --- R-KERNEL-003 generated-registration DCE proof (registration/) -----------------------------
# The build-time generated registration TU + uniform LTO + linker GC, with a measured
# referenced-vs-unreferenced footprint delta proving an unreferenced package leaves zero footprint.
add_subdirectory(registration)

# aot_pin_guard.cmake — R-QA-013 deterministic regression guard for the Context-Engine#24 fix.
#
# WHY THIS EXISTS: the WAMR-AOT CI SIGILL flake (#24) was caused by host-CPU-dependent wamrc
# codegen (default = LLVMGetHostCPUName() + empty explicit features → LLVM model-default ISA,
# which can overshoot what a hypervisor-masked CI VM executes). The fix pins the AOT target to
# a fleet-safe baseline in CMakeLists.txt. This script guards that pin: it re-compiles the
# guest with its OWN hardcoded copy of the pinned flags and byte-compares the result against
# the image the build rule produced. If the build rule's flags ever drift from the contract
# below (pin dropped/renamed → host-dependent codegen again), the two images differ on any
# host whose CPU defaults are not exactly the pinned baseline — so the regression fails HERE,
# deterministically, instead of resurfacing as an intermittent SIGILL on the CI fleet.
#
# Invoked as a ctest (context-spike-wasm-aot-pin-guard) via:
#   cmake -DWAMRC=<wamrc-exe> -DMODULE_WASM=<guest/module.wasm> -DMODULE_AOT=<built image>
#         -DREFERENCE_AOT=<scratch output path> -P aot_pin_guard.cmake

foreach(_var WAMRC MODULE_WASM MODULE_AOT REFERENCE_AOT)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "aot_pin_guard: required variable ${_var} is not set")
    endif()
endforeach()

if(NOT EXISTS "${MODULE_AOT}")
    message(FATAL_ERROR "aot_pin_guard: built AOT image not found at ${MODULE_AOT} "
                        "(did the context-spike-wasm-aot target run?)")
endif()

# THE PINNED TARGET CONTRACT — keep in lockstep with _ctx_wamrc_flags in CMakeLists.txt.
# x86-64-v2 = SSE4.2/POPCNT-level baseline, executable on every x86_64 GitHub runner
# (Intel Nehalem+ / all AMD EPYC). Intentionally duplicated here, NOT shared through a
# variable: sharing would make the guard tautological (a drifted build rule would drift the
# guard with it).
set(_pinned_flags --opt-level=3 --target=x86_64 --cpu=x86-64-v2)

execute_process(
    COMMAND "${WAMRC}" ${_pinned_flags} -o "${REFERENCE_AOT}" "${MODULE_WASM}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "aot_pin_guard: pinned wamrc invocation failed (exit ${_rc}).\n"
                        "A wamrc upgrade may have dropped/renamed the pinned flags — the pin "
                        "must be re-established, not removed.\nstdout: ${_out}\nstderr: ${_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${REFERENCE_AOT}" "${MODULE_AOT}"
    RESULT_VARIABLE _cmp)
if(NOT _cmp EQUAL 0)
    message(FATAL_ERROR "aot_pin_guard: the built module.aot does NOT byte-match the "
                        "pinned-target reference image.\nThe build rule's wamrc flags have "
                        "drifted from the pinned contract (${_pinned_flags}) — codegen may be "
                        "host-CPU-dependent again, which is exactly the #24 SIGILL flake. "
                        "Restore the pin in spikes/wasm/CMakeLists.txt (_ctx_wamrc_flags).")
endif()

message(STATUS "aot_pin_guard: OK — module.aot matches the pinned x86-64-v2 target contract")

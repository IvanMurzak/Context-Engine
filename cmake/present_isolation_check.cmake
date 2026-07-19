# The ctest half of the headless-invariant gate (M9 e03) — run via `cmake -P`.
#
# The configure-time walk in ContextPresentIsolation.cmake already FATAL_ERRORs on a real violation,
# so this test can never see one. Its job is the failure mode a FATAL_ERROR structurally cannot
# catch: the check NOT RUNNING. Rename `context_runtime_server`, or drop it from the audited list,
# and the configure-time gate happily reports "isolated" over an empty set — a gate that checks
# nothing reads exactly like a gate that passed.
#
# So this asserts the report exists, reached a verdict, and actually AUDITED every target it was told
# to expect.
#
# Inputs: -DREPORT=<path> -DEXPECT_AUDITED=<semicolon-separated target names>

if(NOT DEFINED REPORT)
    message(FATAL_ERROR "present_isolation_check: -DREPORT=<path> is required")
endif()

if(NOT EXISTS "${REPORT}")
    message(FATAL_ERROR
        "present_isolation_check: no audit report at ${REPORT} — the headless-invariant gate did "
        "not run at configure time")
endif()

file(READ "${REPORT}" _report)

if(NOT _report MATCHES "VERDICT: isolated")
    message(FATAL_ERROR "present_isolation_check: the audit did not reach an 'isolated' verdict:\n${_report}")
endif()

foreach(_target IN LISTS EXPECT_AUDITED)
    if(NOT _report MATCHES "(CLEAN|VIOLATION|SKIPPED) ${_target}[ \n]")
        message(FATAL_ERROR
            "present_isolation_check: '${_target}' was never audited by the headless-invariant "
            "gate. Either it was renamed (update the TARGETS list in src/CMakeLists.txt) or the "
            "gate silently stopped covering it.\n${_report}")
    endif()
    if(_report MATCHES "SKIPPED ${_target}[ \n]")
        message(FATAL_ERROR
            "present_isolation_check: '${_target}' exists in the expected set but was SKIPPED as "
            "'not a target in this configuration' — the invariant is unproven for it.\n${_report}")
    endif()
endforeach()

message(STATUS "headless invariant holds: no daemon/runtime target links the presentation path")

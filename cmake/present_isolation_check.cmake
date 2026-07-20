# The ctest half of the headless-invariant gate (M9 e03) — run via `cmake -P`.
#
# The configure-time walk in ContextPresentIsolation.cmake already FATAL_ERRORs on a real violation,
# so this test can never see one. Its job is the failure mode a FATAL_ERROR structurally cannot
# catch: the check NOT RUNNING. Rename `context_runtime_server`, or drop it from the audited list,
# and the configure-time gate happily reports "isolated" over an empty set — a gate that checks
# nothing reads exactly like a gate that passed.
#
# So this asserts the report exists, reached a verdict, actually AUDITED every target it was told to
# expect, and that every target it was told to FORBID is a real target. That last half matters for
# the same reason: renaming context_render_present makes every closure check match nothing, and the
# audit then reports "isolated" while forbidding a name that no longer exists.
#
# Shared by BOTH link-closure gates (the e03 headless invariant and the e04 D10 shell boundary) —
# the report format and this failure mode are identical, only the wording differs, so -DGATE names
# which gate is reporting rather than the message hardcoding one of them.
#
# Inputs: -DREPORT=<path> -DEXPECT_AUDITED=<names> [-DEXPECT_FORBIDDEN=<names>] [-DGATE=<name>]
#         [-DSOURCE_HINT=<where the TARGETS/FORBIDDEN lists live>]
# EXPECT_FORBIDDEN lists only the forbidden targets that exist UNCONDITIONALLY — a target behind an
# off-by-default option (context_render_wgpu) is legitimately absent from a default configure.

if(NOT DEFINED REPORT)
    message(FATAL_ERROR "link-boundary check: -DREPORT=<path> is required")
endif()
if(NOT DEFINED GATE)
    set(GATE "headless-invariant gate")
endif()
if(NOT DEFINED SOURCE_HINT)
    set(SOURCE_HINT "src/CMakeLists.txt")
endif()

if(NOT EXISTS "${REPORT}")
    message(FATAL_ERROR
        "${GATE}: no audit report at ${REPORT} — the gate did not run at configure time")
endif()

file(READ "${REPORT}" _report)

if(NOT _report MATCHES "VERDICT: isolated")
    message(FATAL_ERROR "${GATE}: the audit did not reach an 'isolated' verdict:\n${_report}")
endif()

foreach(_target IN LISTS EXPECT_AUDITED)
    if(NOT _report MATCHES "(CLEAN|VIOLATION|SKIPPED) ${_target}[ \n]")
        message(FATAL_ERROR
            "${GATE}: '${_target}' was never audited. Either it was renamed (update the TARGETS "
            "list in ${SOURCE_HINT}) or the gate silently stopped covering it.\n${_report}")
    endif()
    if(_report MATCHES "SKIPPED ${_target}[ \n]")
        message(FATAL_ERROR
            "${GATE}: '${_target}' exists in the expected set but was SKIPPED as 'not a target in "
            "this configuration' — the invariant is unproven for it.\n${_report}")
    endif()
endforeach()

foreach(_forbidden IN LISTS EXPECT_FORBIDDEN)
    if(NOT _report MATCHES "FORBIDDEN-PRESENT ${_forbidden}[ \n]")
        message(FATAL_ERROR
            "${GATE}: '${_forbidden}' is on the forbidden list but is not a target in this "
            "configuration, so the gate forbade nothing. Either it was renamed (update the "
            "FORBIDDEN list in ${SOURCE_HINT}) or it stopped being built unconditionally."
            "\n${_report}")
    endif()
endforeach()

message(STATUS "${GATE}: holds — no audited target reaches a forbidden one")

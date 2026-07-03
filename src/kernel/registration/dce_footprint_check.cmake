# Measured referenced-vs-unreferenced footprint check (R-KERNEL-003 zero-footprint proof). Run as a
# ctest via `cmake -P`. Asserts that demo package "alpha"'s unique marker is PRESENT in the binary
# that references alpha and ABSENT from the binary that does not — i.e. an unreferenced package
# contributes zero symbol footprint — while the control package "beta" (referenced by BOTH variants)
# is present in both. Also reports the size delta. Toolchain-agnostic: greps the linked binaries with
# file(STRINGS), needing no nm/dumpbin/objdump.

foreach(_v REFERENCED_BIN UNREFERENCED_BIN ALPHA_MARKER BETA_MARKER)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "dce_footprint_check: ${_v} is required")
    endif()
endforeach()

foreach(_bin "${REFERENCED_BIN}" "${UNREFERENCED_BIN}")
    if(NOT EXISTS "${_bin}")
        message(FATAL_ERROR "dce_footprint_check: binary not found: ${_bin}")
    endif()
endforeach()

file(SIZE "${REFERENCED_BIN}" _ref_size)
file(SIZE "${UNREFERENCED_BIN}" _unref_size)

file(STRINGS "${REFERENCED_BIN}"   _ref_alpha   REGEX "${ALPHA_MARKER}")
file(STRINGS "${UNREFERENCED_BIN}" _unref_alpha REGEX "${ALPHA_MARKER}")
file(STRINGS "${REFERENCED_BIN}"   _ref_beta    REGEX "${BETA_MARKER}")
file(STRINGS "${UNREFERENCED_BIN}" _unref_beta  REGEX "${BETA_MARKER}")

message(STATUS "DCE proof: referenced binary   = ${REFERENCED_BIN} (${_ref_size} bytes)")
message(STATUS "DCE proof: unreferenced binary = ${UNREFERENCED_BIN} (${_unref_size} bytes)")

set(_errors "")

# THE proof: the unreferenced package leaves zero footprint (its marker is gone).
if(_unref_alpha)
    string(APPEND _errors
        "  - alpha marker PRESENT in the unreferenced binary (expected ABSENT: the unreferenced package left a footprint)\n")
endif()
# Control 1: the mechanism DOES link a package when it is referenced (no false-positive elimination).
if(NOT _ref_alpha)
    string(APPEND _errors
        "  - alpha marker ABSENT in the referenced binary (expected PRESENT: mechanism failed to link a referenced package)\n")
endif()
# Control 2: the mechanism does not simply strip everything — the shared package survives in both.
if(NOT _ref_beta)
    string(APPEND _errors "  - beta (control) marker ABSENT in the referenced binary\n")
endif()
if(NOT _unref_beta)
    string(APPEND _errors "  - beta (control) marker ABSENT in the unreferenced binary\n")
endif()

if(_errors)
    message(FATAL_ERROR "DCE zero-footprint proof FAILED:\n${_errors}")
endif()

math(EXPR _delta "${_ref_size} - ${_unref_size}")
message(STATUS
    "DCE proof PASSED: unreferenced package 'alpha' contributes zero symbol footprint; "
    "referenced-vs-unreferenced size delta = ${_delta} bytes")

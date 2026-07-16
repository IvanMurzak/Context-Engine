# Render-footprint audit (R-HEAD-002 / L-5 DCE'd minimal builds, M8 task a06). Run as a ctest via
# `cmake -P`. Proves the SERVER/headless flavor carries NO render payload while the DESKTOP flavor does:
#   1. the RENDER marker is PRESENT in the desktop binary and ABSENT from the server binary (the
#      symbol audit — the render subsystem's sentinel is linked only when context_render is);
#   2. the CONTROL marker is present in BOTH (proves the audit is not vacuously stripping everything);
#   3. the server binary is SMALLER than the desktop binary (the size audit — render code has weight).
# Toolchain-agnostic: greps the linked binaries with file(STRINGS) + file(SIZE), needing no
# nm/objdump/dumpbin, so it runs identically on the Linux/macOS CI legs and the local Windows GCC gate.
# Mirrors the R-KERNEL-003 dce_footprint_check.cmake pattern (an unreferenced module = zero footprint).

foreach(_v DESKTOP_BIN SERVER_BIN RENDER_MARKER CONTROL_MARKER)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "render_footprint_check: ${_v} is required")
    endif()
endforeach()

foreach(_bin "${DESKTOP_BIN}" "${SERVER_BIN}")
    if(NOT EXISTS "${_bin}")
        message(FATAL_ERROR "render_footprint_check: binary not found: ${_bin}")
    endif()
endforeach()

file(SIZE "${DESKTOP_BIN}" _desktop_size)
file(SIZE "${SERVER_BIN}" _server_size)

file(STRINGS "${DESKTOP_BIN}" _desktop_render REGEX "${RENDER_MARKER}")
file(STRINGS "${SERVER_BIN}"  _server_render  REGEX "${RENDER_MARKER}")
file(STRINGS "${DESKTOP_BIN}" _desktop_ctrl   REGEX "${CONTROL_MARKER}")
file(STRINGS "${SERVER_BIN}"  _server_ctrl    REGEX "${CONTROL_MARKER}")

message(STATUS "render footprint: desktop binary = ${DESKTOP_BIN} (${_desktop_size} bytes)")
message(STATUS "render footprint: server  binary = ${SERVER_BIN} (${_server_size} bytes)")

set(_errors "")

# THE proof: the server/headless flavor carries no render payload (its render marker is gone).
if(_server_render)
    string(APPEND _errors
        "  - render marker PRESENT in the server binary (expected ABSENT: the headless flavor shipped render payload)\n")
endif()
# Control 1: the desktop flavor DOES link the render subsystem (no false-positive elimination).
if(NOT _desktop_render)
    string(APPEND _errors
        "  - render marker ABSENT in the desktop binary (expected PRESENT: the desktop flavor failed to link render)\n")
endif()
# Control 2: the audit is not simply stripping everything — the control marker survives in both.
if(NOT _desktop_ctrl)
    string(APPEND _errors "  - control marker ABSENT in the desktop binary\n")
endif()
if(NOT _server_ctrl)
    string(APPEND _errors "  - control marker ABSENT in the server binary\n")
endif()
# The size audit (DoD 2): render code has weight, so the headless flavor is smaller.
if(NOT _server_size LESS _desktop_size)
    string(APPEND _errors
        "  - server binary (${_server_size} bytes) is NOT smaller than the desktop binary (${_desktop_size} bytes); render adds no measurable footprint\n")
endif()

if(_errors)
    message(FATAL_ERROR "render footprint audit FAILED:\n${_errors}")
endif()

math(EXPR _delta "${_desktop_size} - ${_server_size}")
message(STATUS
    "render footprint audit PASSED: the server/headless flavor carries no render payload; "
    "desktop-vs-server size delta = ${_delta} bytes")

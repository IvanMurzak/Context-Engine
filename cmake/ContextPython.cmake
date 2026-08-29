# ContextPython.cmake — the Python interpreter policy for every `find_package(Python3)` in the tree.
#
# WHY THIS EXISTS. FindPython3 on Windows consults the registry FIRST by default
# (`Python3_FIND_REGISTRY=FIRST`): HKLM\SOFTWARE\Python\PythonCore\<version>\InstallPath, a
# MACHINE-GLOBAL key that any process on the box can rewrite. On the shared self-hosted CI machine
# another tenant's tool provisioning did exactly that — twice (2026-08-27, 2026-08-29): it pointed the
# key at its own ephemeral tool cache, then deleted the directory, and every Windows leg of this repo
# died at configure with `Could NOT find Python3` while a perfectly good interpreter sat on disk.
# Nothing in this repository had changed. See docs/self-hosted-runners.md § 6.
#
# THE RULE. An interpreter handed in through the environment is the build's own decision:
#
#   CONTEXT_PYTHON3_EXECUTABLE=<path to python.exe>
#
# When it is set, that interpreter is pinned as `Python3_EXECUTABLE` and the registry lookup is turned
# OFF for the whole configure, so the machine's registry state can no longer reach this build. CI's
# Windows legs set it from `.github/actions/windows-python`, which locates and RUNS a Python >= 3.12
# through directory scans only (never the registry, never the `py` launcher, which reads it). A
# developer never needs to set it — with the variable absent this module changes nothing.
#
# Fail-closed on a bad pin: a path that does not exist is a configuration error worth a legible
# stop, not a silent fall-through to the very registry lookup the pin exists to bypass.
#
# Included ONCE from src/CMakeLists.txt, before the first add_subdirectory — plain directory-scope
# code (not a function), so `Python3_FIND_REGISTRY` lands in the top-level scope every subdirectory
# inherits. The include(ContextCef) site that also finds Python3 is in that same scope.

if(DEFINED ENV{CONTEXT_PYTHON3_EXECUTABLE} AND NOT "$ENV{CONTEXT_PYTHON3_EXECUTABLE}" STREQUAL "")
    set(_context_python_pin "$ENV{CONTEXT_PYTHON3_EXECUTABLE}")
    file(TO_CMAKE_PATH "${_context_python_pin}" _context_python_pin)
    if(NOT EXISTS "${_context_python_pin}")
        message(FATAL_ERROR
            "CONTEXT_PYTHON3_EXECUTABLE names a file that does not exist: '${_context_python_pin}'. "
            "Unset it to let FindPython3 search, or point it at a real python executable "
            "(docs/self-hosted-runners.md § 6).")
    endif()
    # FORCE: the environment pin is the authority for this configure, including over a value a
    # previous configure of the same tree cached. FindPython3 re-validates a changed executable.
    set(Python3_EXECUTABLE "${_context_python_pin}" CACHE FILEPATH
        "Python interpreter pinned through CONTEXT_PYTHON3_EXECUTABLE (cmake/ContextPython.cmake)"
        FORCE)
    # Belt and braces: with the executable pinned FindPython3 never searches, but should the pin ever
    # be rejected the fallback must still not read the machine's registry.
    set(Python3_FIND_REGISTRY NEVER)
    message(STATUS "Python: pinned to ${_context_python_pin} (CONTEXT_PYTHON3_EXECUTABLE); "
                   "registry lookup off")
    unset(_context_python_pin)
endif()

# context_acquire_cef() — the shared CEF-acquisition prologue for the two CONTEXT_BUILD_GUI_CEF call
# sites (src/editor/cef/ M5-F0a substrate + src/editor/gui/host/ M5-F0b host), Context-Engine#214.
# Resolves the pinned CEF prebuilt for this host triple, fetches + SHA-verifies it
# (tools/fetch_cef.py, R-SEC-009 fail-closed), sets CEF's own idempotent CACHE knobs, and finds the
# CEF package (FindCEF.cmake + libcef_dll_wrapper) — everything both call sites need before building
# their own per-OS boot target. Behavior is byte-identical to the prior per-call-site duplication:
# same CEF pin, same boot-smoke, same substrate outputs, protocolMajor unchanged.
#
# NOT enable_language(C): that guard is directory-position-sensitive (mid-tree enable_language leaves
# earlier-processed directories with a stale no-C-compiler snapshot) and stays where it already was —
# top-level src/CMakeLists.txt, BEFORE any add_subdirectory. It was never duplicated per call site.
#
# Deliberately a MACRO, not a function: FindCEF.cmake / cef_macros.cmake (the CEF distribution's own
# CMake, invoked here via find_package(CEF REQUIRED) + SET_CEF_TARGET_OUT_DIR()) populate a large
# surface of plain (non-CACHE) variables in the INCLUDING scope — CEF_STANDARD_LIBS,
# CEF_LIB_DEBUG/RELEASE, CEF_BINARY_FILES, CEF_RESOURCE_FILES, CEF_BINARY_DIR, CEF_RESOURCE_DIR,
# CEF_HELPER_APP_SUFFIXES, CEF_TARGET_OUT_DIR, etc — which both call sites consume directly after this
# prologue returns. A CMake function() introduces a NEW variable scope, so all of those would be
# silently lost on return unless individually re-exported via PARENT_SCOPE — fragile, and liable to
# break silently on a future CEF version that adds a new variable. A macro() is scope-transparent (it
# behaves as if textually inlined at the call site), which is what makes this hoist byte-identical to
# the prior per-call-site duplication instead of a behavior change.
#
# Params:
#   log_prefix   — message()-prefix identifying which target is fetching CEF (was the literal
#                  "context_cef" in src/editor/cef/, "context_gui_host" in src/editor/gui/host/).
#   ci_job_name  — the CI job name cited in the Windows/non-MSVC FATAL_ERROR (was "cef-substrate" /
#                  "editor-cef-smoke").
macro(context_acquire_cef log_prefix ci_job_name)
    # --- resolve the host triple (must match a tools/cef-prebuilt.json key) ------------------------
    set(_cef_triple "")
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        if(NOT MSVC)
            message(FATAL_ERROR "CONTEXT_BUILD_GUI_CEF on Windows needs the MSVC-ABI CEF prebuilt; build \
with MSVC (the ${ci_job_name} CI job does — VsDevCmd + Ninja + cl on the self-hosted runner).")
        endif()
        set(_cef_triple "x86_64-pc-windows-msvc")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_cef_triple "aarch64-apple-darwin")
    elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8
           AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|X86_64")
        set(_cef_triple "x86_64-unknown-linux-gnu")
    else()
        message(FATAL_ERROR "CONTEXT_BUILD_GUI_CEF: no CEF prebuilt pinned for \
${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} (tools/cef-prebuilt.json has Linux-x64 / macOS-ARM64 / \
Win-x64).")
    endif()

    # --- fetch + verify the pinned CEF prebuilt (verify-before-use, fail-closed) --------------------
    # FindPython3 resolves the real interpreter (incl. the `py` launcher on the self-hosted Windows
    # runner), robust to the python3-vs-py alias difference — same as src/runtime/js/. Idempotent:
    # fetch_cef.py stamps its output, so a second call site (or a re-configure) re-entering this same
    # ${_cef_dest} is a no-op past the stamp check.
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_cef_manifest "${CMAKE_SOURCE_DIR}/../tools/cef-prebuilt.json")
    set(_cef_fetch "${CMAKE_SOURCE_DIR}/../tools/fetch_cef.py")
    set(_cef_dest "${CMAKE_BINARY_DIR}/_cef/${_cef_triple}")
    message(STATUS "${log_prefix}: fetching + verifying pinned CEF prebuilt for ${_cef_triple}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_cef_fetch}"
                --manifest "${_cef_manifest}" --triple "${_cef_triple}" --dest "${_cef_dest}"
        RESULT_VARIABLE _cef_rc OUTPUT_VARIABLE _cef_out ERROR_VARIABLE _cef_err)
    if(NOT _cef_rc EQUAL 0)
        message(FATAL_ERROR
            "${log_prefix}: CEF prebuilt fetch/verify FAILED (rc=${_cef_rc}) — refusing to build against "
            "an unverified CEF (R-SEC-009 fail-closed).\nstdout: ${_cef_out}\nstderr: ${_cef_err}")
    endif()
    string(STRIP "${_cef_out}" _cef_out)
    message(STATUS "${log_prefix}: ${_cef_out}")

    # --- consume the CEF distribution's own CMake (FindCEF + libcef_dll_wrapper) --------------------
    # The pinned distribution ships FindCEF.cmake / cef_variables.cmake / cef_macros.cmake — the sanctioned
    # way to build against it. Knobs go in the cache BEFORE find_package(CEF); CACHE ... FORCE is
    # idempotent so re-running this macro from the second call site is a harmless re-set of the same
    # value:
    #   * USE_SANDBOX OFF — the boot substrate runs no_sandbox (the M138+ Windows sandbox bootstrap.exe
    #     launch model is an F0b packaging concern, L-41 / ROADMAP §1 M5 note); OFF also drops the
    #     CEF_USE_SANDBOX/BOOTSTRAP defines + cef_sandbox link.
    #   * CEF_RUNTIME_LIBRARY_FLAG /MD — match this repo's dynamic MSVC runtime (CEF defaults to /MT, which
    #     LNK2038-clashes), mirroring the spike.
    #   * USE_ATL OFF — the boot substrate needs no ATL; keep the toolchain requirement minimal.
    #   * CEF_DEBUG_INFO_FLAG /Z7 (Windows) — CEF defaults this to /Zi, which writes a separate .pdb through
    #     the ONE shared mspdbsrv.exe server. Under CEF's /MP parallel build on the self-hosted Windows
    #     runner (amplified by sccache) the parallel cl.exe instances contend on that shared PDB server and
    #     intermittently fail the many libcef_dll_wrapper cpptoc .obj compiles with `fatal error C1090: PDB
    #     API call failed, error code '23'` — a transient runner-load FLAKE that passes on re-run (owner-
    #     flagged run 29201369515: attempt 1 RED, attempt 2 GREEN, same commit). /Z7 embeds debug info in
    #     each .obj (no shared .pdb server), so /MP has zero PDB-server contention; this is CEF's own
    #     documented knob for exactly this case (its default comment recommends /Z7 for caching build tools
    #     like clcache). Windows/MSVC-only (the C1090 is MSVC-specific); the linker still emits a .pdb.
    set(USE_SANDBOX OFF CACHE BOOL "" FORCE)
    set(CEF_RUNTIME_LIBRARY_FLAG "/MD" CACHE STRING "" FORCE)
    set(USE_ATL OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(CEF_DEBUG_INFO_FLAG "/Z7" CACHE STRING "" FORCE)
    endif()
    set(CEF_ROOT "${_cef_dest}" CACHE INTERNAL "Pinned CEF distribution root")
    # The CEF binary distribution ships its OWN CMake as a MODULE package (cmake/FindCEF.cmake), NOT a
    # CONFIG package — so CMake can only locate FindCEF.cmake if ${CEF_ROOT}/cmake is on CMAKE_MODULE_PATH
    # BEFORE find_package(CEF). (FindCEF.cmake itself re-appends that dir for its internal cef_variables /
    # cef_macros includes, but that runs only AFTER the module is found.) CMAKE_MODULE_PATH is a
    # directory-scoped variable, so each call site's own directory scope needs its own append — this
    # macro re-appends it every call, matching the prior per-call-site duplication.
    list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
    find_package(CEF REQUIRED)

    # Output binaries into a config subdir (CEF's convention; also where libcef + resources are staged).
    SET_CEF_TARGET_OUT_DIR()

    # Build libcef_dll_wrapper from the pinned distribution (its OWN target with CEF's warning posture).
    # Guarded so processing BOTH call sites in one CONTEXT_BUILD_GUI_CEF configure never double-defines
    # the target — the second call site's invocation is a no-op past this point.
    if(NOT TARGET libcef_dll_wrapper)
        add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll_wrapper")
    endif()
endmacro()

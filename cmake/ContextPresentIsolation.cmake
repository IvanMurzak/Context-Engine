# LINK-CLOSURE BOUNDARY GATES. Two of them now share this module:
#
#   * the HEADLESS INVARIANT (M9 e03; design 03 §2, R-HEAD-002/004) — nothing in the daemon/runtime
#     links the presentation path;
#   * the D10 SHELL BOUNDARY (M9 e04; design 02 §1 / 03 §1) — the editor Shell links the published
#     client SDK, never the EditorKernel's own modules.
#
# They differ only in which targets and what a violation MEANS, so the walk, the report format and
# the vacuous-pass protections are shared (context_assert_no_linkage) and only the wording is
# per-gate. The original e03 gate is described below and its behaviour is unchanged.
#
# The claim: "nothing in the daemon/runtime links the present path; only the Shell does." That claim
# is cheap to state, easy to erode — one convenient target_link_libraries and a headless server binary
# is dragging window/swapchain/GDI code around — and NOTHING would fail. This module makes it
# enforceable the same way tools/check_include_graph.py makes the D10 client boundary enforceable:
# by checking the real graph, transitively, and failing the BUILD when it is violated.
#
# It walks the transitive link closure of each declared headless target and errors if any forbidden
# presentation target is reachable. Transitivity is what makes it strong — presentation cannot sneak
# in via an intermediate library, because every hop is walked.
#
# It runs at CONFIGURE time, so a violation fails the configure on every OS leg of the build matrix
# before a single object is compiled. It also writes an audit report, which the companion ctest
# (`render-present-headless-isolation`) re-reads — that test's job is to catch the OTHER failure
# mode, the one a FATAL_ERROR can never catch: the check silently not running at all because a target
# was renamed and quietly skipped.

# Collect the transitive link closure of `target` into `out_var` (a list of target names).
# Non-target entries — raw library names like `gdi32`, `-framework Metal`, absolute paths, generator
# expressions — are not link EDGES we can follow, so they are ignored by design.
function(context_collect_link_closure target out_var)
    set(_pending "${target}")
    set(_seen "")
    while(_pending)
        list(POP_FRONT _pending _current)
        if(_current IN_LIST _seen)
            continue()
        endif()
        if(NOT TARGET ${_current})
            continue()
        endif()
        list(APPEND _seen "${_current}")

        set(_deps "")
        # LINK_LIBRARIES is unset on an INTERFACE library; INTERFACE_LINK_LIBRARIES is what carries
        # PUBLIC edges. Reading both is what keeps the walk from stopping one hop short.
        get_target_property(_type ${_current} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_link ${_current} LINK_LIBRARIES)
            if(_link)
                list(APPEND _deps ${_link})
            endif()
        endif()
        get_target_property(_iface ${_current} INTERFACE_LINK_LIBRARIES)
        if(_iface)
            list(APPEND _deps ${_iface})
        endif()

        foreach(_dep IN LISTS _deps)
            # Strip the $<LINK_ONLY:...> wrapper CMake puts around PRIVATE deps of a static library
            # when they surface through INTERFACE_LINK_LIBRARIES; the dependency inside it is real.
            if(_dep MATCHES "^\\$<LINK_ONLY:(.+)>$")
                set(_dep "${CMAKE_MATCH_1}")
            endif()
            if(_dep MATCHES "^\\$<")
                continue() # any other generator expression is not a followable edge
            endif()
            if(TARGET ${_dep})
                list(APPEND _pending "${_dep}")
            endif()
        endforeach()
    endwhile()
    set(${out_var} "${_seen}" PARENT_SCOPE)
endfunction()

# context_assert_no_linkage(REPORT <path> TITLE <text> KIND <word> REMEDY <text>
#                           FORBIDDEN <t>... TARGETS <t>...)
#
# The GENERIC gate: FATAL_ERRORs when any FORBIDDEN target is transitively reachable from any TARGETS
# entry, and always writes an audit report to REPORT in the shared format the companion ctest reads.
#
# Parameterized rather than copied because there are now TWO link-closure boundaries in the tree —
# e03's headless invariant (no daemon/runtime target links presentation) and e04's D10 shell boundary
# (the editor Shell reaches the daemon over the published client SDK, never by linking the kernel's
# own modules). They differ only in WHICH targets and WHAT the failure means, so the walk, the report
# format and the vacuous-pass protections are shared and only the wording is per-gate. TITLE/KIND
# appear in the report, REMEDY in the FATAL_ERROR.
function(context_assert_no_linkage)
    cmake_parse_arguments(_arg "" "REPORT;TITLE;KIND;REMEDY" "FORBIDDEN;TARGETS" ${ARGN})
    if(NOT _arg_REPORT)
        message(FATAL_ERROR "context_assert_no_linkage: REPORT is required")
    endif()

    set(_report "# ${_arg_TITLE}\n")
    string(APPEND _report "# Forbidden (${_arg_KIND}) targets: ${_arg_FORBIDDEN}\n")

    # Record whether each FORBIDDEN name is a real target. Without this the gate has a second
    # vacuous-pass mode, symmetric with the audited-side one the companion ctest already catches:
    # rename context_render_present and every closure check below silently matches nothing, so the
    # audit reports "isolated" while forbidding a target that no longer exists.
    foreach(_forbidden IN LISTS _arg_FORBIDDEN)
        if(TARGET ${_forbidden})
            string(APPEND _report "FORBIDDEN-PRESENT ${_forbidden}\n")
        else()
            string(APPEND _report "FORBIDDEN-ABSENT ${_forbidden} (not a target in this configuration)\n")
        endif()
    endforeach()

    set(_violations "")

    foreach(_target IN LISTS _arg_TARGETS)
        if(NOT TARGET ${_target})
            # A checked target that does not exist in THIS configuration (an option is off) is
            # recorded as skipped rather than silently dropped — the companion ctest reads these
            # lines, so a target that quietly vanished is visible instead of looking like a pass.
            string(APPEND _report "SKIPPED ${_target} (not a target in this configuration)\n")
            continue()
        endif()
        context_collect_link_closure(${_target} _closure)
        set(_hits "")
        foreach(_forbidden IN LISTS _arg_FORBIDDEN)
            if(_forbidden IN_LIST _closure)
                list(APPEND _hits "${_forbidden}")
            endif()
        endforeach()
        if(_hits)
            string(APPEND _report "VIOLATION ${_target} links ${_hits}\n")
            list(APPEND _violations "${_target} -> ${_hits}")
        else()
            list(LENGTH _closure _closure_size)
            string(APPEND _report "CLEAN ${_target} (${_closure_size} targets in closure)\n")
        endif()
    endforeach()

    if(_violations)
        string(APPEND _report "VERDICT: violated\n")
        file(WRITE "${_arg_REPORT}" "${_report}")
        message(FATAL_ERROR "${_arg_TITLE} VIOLATED.\n  ${_violations}\n${_arg_REMEDY}")
    endif()

    string(APPEND _report "VERDICT: isolated\n")
    file(WRITE "${_arg_REPORT}" "${_report}")
endfunction()

# context_assert_no_present_linkage(REPORT <path> FORBIDDEN <t>... TARGETS <t>...)
#
# The e03 HEADLESS INVARIANT: nothing in the daemon/runtime/CLI links the presentation path.
function(context_assert_no_present_linkage)
    cmake_parse_arguments(_arg "" "REPORT" "FORBIDDEN;TARGETS" ${ARGN})
    context_assert_no_linkage(
        REPORT "${_arg_REPORT}"
        TITLE "Headless-invariant audit (M9 e03, design 03 §2)"
        KIND "presentation"
        REMEDY
            "Only the Shell may link presentation. If a headless target genuinely needs render \
code, it needs the GPU-free context_render abstraction — not context_render_present \
(windows/swapchain/OS blit) and not context_render_wgpu (the GPU backend)."
        FORBIDDEN ${_arg_FORBIDDEN}
        TARGETS ${_arg_TARGETS})
endfunction()

# context_assert_shell_boundary(REPORT <path> FORBIDDEN <t>... TARGETS <t>...)
#
# The e04 D10 SHELL BOUNDARY: the editor Shell is an ORDINARY CLIENT. It talks the published contract
# through context_client and never links the kernel's own modules (02 §1). Nothing in a monorepo
# enforces that by itself — an in-tree target can link whatever it likes and still build — and the
# `editor-boundary` CI job's include-graph check covers only the INSTALLED client headers, not what
# the Shell links. This is the other half.
function(context_assert_shell_boundary)
    cmake_parse_arguments(_arg "" "REPORT" "FORBIDDEN;TARGETS" ${ARGN})
    context_assert_no_linkage(
        REPORT "${_arg_REPORT}"
        TITLE "D10 shell-boundary audit (M9 e04, design 02 §1 / 03 §1)"
        KIND "kernel-internal"
        REMEDY
            "The editor Shell is an ORDINARY CLIENT: it reaches the daemon over the published \
context_client SDK, never by linking the EditorKernel's own modules. If the Shell needs something \
those modules provide, it belongs on the client contract (a verb, a topic, or an addition to the \
exported SDK) — not on a direct link edge, which would make the editor un-shippable apart from the \
engine's internals and would silently un-test the boundary e02 built."
        FORBIDDEN ${_arg_FORBIDDEN}
        TARGETS ${_arg_TARGETS})
endfunction()

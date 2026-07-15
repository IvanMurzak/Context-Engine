# src/packages/ui/ — the runtime UI package (M7 T1, R-UI-002/005/006, issue #223). The headless
# foundation of the pluggable runtime UI system: a retained UiTree/UiNode (closed role vocabulary +
# CSS-like style props + visibility/opacity/transform), a pointer/focus/key/custom event + handler
# dispatch model, dirty/damage tracking computed IN the tree (damage coalescing), the backend-agnostic
# UiProvider contract + Capabilities struct (R-UI-005), and a null/headless provider.
#
# LOCKS D1/D2/D3/D6: this is a NEW runtime tree, borrowing the editor context_gui_uitree DESIGN (closed
# role vocab so R-A11Y-002 can hook it post-core) with ZERO link-level sharing — the editor tree never
# becomes a runtime dependency. UI is presentation (D6): it lives OUTSIDE the sim World and registers no
# hashed sim component — at this foundation tier the package is PURE STDLIB and does not link the sim at
# all (the L-45 input-routing glue + session composition arrive in a later task). The GPU backend lives
# in src/render/ui/ (a later task) and implements the provider contract header shipped HERE (D3).
#
# It is a PACKAGE, not kernel core (L-60 microkernel invariant): nothing under src/kernel/ links it.

add_library(context_ui STATIC
    src/damage.cpp
    src/layout.cpp
    src/provider.cpp
    src/ui_tree.cpp)

target_include_directories(context_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Pure stdlib — NO context_session / context_kernel link at this tier (unlike context_input, whose public
# API is defined in the session input vocabulary). Only the warnings-as-errors baseline (conventions.md).
target_link_libraries(context_ui PRIVATE context_warnings)

# --- context_ui_script: the TypeScript-authoring binding shim (M7 T4 / a4, R-UI-001/006) ----------
# The doubles-only bridge between an authored `context.ui` TS surface running on the V8 host and a
# headless UiTree: a UiScriptContext over a caller-owned UiTree + numeric StateStore, plus the table of
# HostFunction-compatible primitives (tree construction / CSS-like style props / event handlers /
# read-only data binding) the host binds as globals the TS API wraps. Deliberately JsEngine-FREE (its
# UiHostFunction typedef is byte-identical to js::HostFunction but names no V8/js header) so it stays
# PURE STDLIB and builds + unit-tests on EVERY toolchain with no V8 link — the js glue that binds the
# table + wires the handler callback to JsEngine::callFunction lives with the caller (src/runtime/ts/),
# CI-only for its V8 path (the m6-exit-2 / test_ts_in_v8 split). Links only context_ui + the warnings
# baseline; a separate STATIC lib so the context_ui foundation stays minimal (its README charter).
add_library(context_ui_script STATIC
    src/script_bindings.cpp)
# context_ui PUBLIC — script_bindings.h (in context_ui's public include tree) depends on ui_tree.h /
# events.h / ui_node.h, so the include dir must propagate to context_ui_script consumers. Warnings
# baseline PRIVATE.
target_link_libraries(context_ui_script PUBLIC context_ui)
target_link_libraries(context_ui_script PRIVATE context_warnings)

# The test executables link context_ui; on this MinGW host a stale libstdc++-6.dll elsewhere on PATH can
# shadow the toolchain's, so they static-link the GCC runtime (gated to GNU — Apple Clang/libc++ and MSVC
# untouched; the same self-contained-binary rationale as the sibling packages).
set(_context_ui_static_runtime
    $<$<CXX_COMPILER_ID:GNU>:-static-libgcc;-static-libstdc++>)

# --- tests (R-QA-013: behavior ships WITH its tests in the same PR) -------------------------------
# Plain assertion-bearing executables registered with ctest (the sibling module pattern; the repo carries
# no C++ test framework). tests/ui_test.h is the tiny CHECK() harness. Coverage:
#   - test_tree:          build/mutate — create/parent/reparent/remove, style/text/bounds mutation, the
#                         closed role vocabulary;
#   - test_dispatch:      the event + handler model — target-then-bubble delivery, handled stops
#                         propagation, registration order, per-kind routing, the minimal focus model;
#   - test_damage:        dirty/damage tracking — region coalescing (overlap union, chains, edge-touch),
#                         structural change ⇒ full-surface damage;
#   - test_provider:      the UiProvider capability negotiation/fallback table — no damage support ⇒ full
#                         repaint; damage-capable ⇒ incremental coalesced regions;
#   - test_null_provider: the null/headless provider — all-false capabilities + zero-cost present (no
#                         tree walk, no mutation), contrasted with a recording provider, plus the
#                         provider-less headless-logic guarantee (R-UI-006).
#   - test_layout:        headless layout + hit-testing + focus order (T2) — flow (Row/Column) + absolute
#                         anchoring computed rects, resize/reflow → damage propagation (drives a1's
#                         damage), top-most hit-testing (overlap/nesting/hidden/zero-opacity), and
#                         deterministic document-order focus.
# The `ui-*` ctest family auto-runs in the general CI test step (no CI job-list edits needed — it is not
# in the build job's -E gate-exclusion regex and is built by --preset dev like every target).
set(_context_ui_tests
    test_tree
    test_dispatch
    test_damage
    test_provider
    test_null_provider
    test_layout)

foreach(_t IN LISTS _context_ui_tests)
    add_executable(context_ui_${_t} tests/${_t}.cpp)
    target_link_libraries(context_ui_${_t} PRIVATE context_ui context_warnings)
    target_link_options(context_ui_${_t} PRIVATE ${_context_ui_static_runtime})
    target_include_directories(context_ui_${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ui-${_t} COMMAND context_ui_${_t})
endforeach()

# --- context_ui_script tests (M7 T4 / a4) ---------------------------------------------------------
#   - test_script_bindings: the doubles-only authoring binding shim — the numeric protocol decoders,
#                           the StateStore, tree construction / style props / event handlers /
#                           read-only data binding through the UiScriptContext AND the host-function
#                           table (as the V8 host feeds them), the dispatch->handler invoker bridge
#                           (a C++ invoker stands in for V8 __ui_invoke), plus failure paths. Pure
#                           C++ (no V8): a LOCAL gate on all 3 OS legs; the authored-TS-in-V8 proof is
#                           ts-test_ui_ts_authoring (CI-only for its V8 path). Auto-runs in the general
#                           step (same `ui-*` family, not in the build job's -E gate-exclusion regex).
add_executable(context_ui_test_script_bindings tests/test_script_bindings.cpp)
target_link_libraries(context_ui_test_script_bindings
    PRIVATE context_ui_script context_ui context_warnings)
target_link_options(context_ui_test_script_bindings PRIVATE ${_context_ui_static_runtime})
target_include_directories(context_ui_test_script_bindings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME ui-test_script_bindings COMMAND context_ui_test_script_bindings)

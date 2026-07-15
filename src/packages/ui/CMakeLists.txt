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

# --- context_ui_input: the UI-side input routing glue (M7 T3 / a3, R-SYS-007 / L-45, D5/D6) --------
# The router->session glue that binds a headless UiTree to the EXISTING input-package InputRouter and
# the ONE sim InputState sink (Session::inject_action_at). L-45 CONSUMPTION, not invention: a UI gaining
# focus pushes a capturing `ui` InputContext onto the router (modal); pointer samples hit-test a2's
# computed rects (a hit dispatches a UI event + emits UI-originated gameplay intents; a miss is swallowed
# when modal or falls through to gameplay when an overlay); every gameplay activation is a
# session::ActionActivation fed through the SAME inject path a key-bound action uses (D5). UI is
# presentation (D6): this lib registers NO sim component and the tree lives OUTSIDE the sim World, so
# hash_world is bit-identical with UI present vs absent (tests/test_input_routing.cpp).
#
# UNLIKE the pure-stdlib context_ui foundation, this glue needs the sim seam, so it is a SEPARATE STATIC
# lib (the context_ui_script precedent — keep the foundation minimal). It links context_input PUBLIC
# (whose public API is the session input vocabulary route() returns — that brings context_session +
# context_kernel transitively) and context_ui PUBLIC (the tree/hit-test surface). CMake resolves both
# targets at generate time regardless of add_subdirectory order.
add_library(context_ui_input STATIC
    src/input_routing.cpp)
target_link_libraries(context_ui_input PUBLIC context_ui context_input)
target_link_libraries(context_ui_input PRIVATE context_warnings)

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

# --- context_ui_input tests (M7 T3 / a3) ----------------------------------------------------------
#   - test_input_routing: THE SEAM — the UI-side router->session glue over a real InputRouter + real
#                         Session: capture-mode swallows unbound gameplay input (HUD-with-focus), a
#                         non-capturing overlay passes it through, a UI button press lands in InputState
#                         IDENTICALLY to a key-bound action (single sink, D5), and hash_world is UNCHANGED
#                         by UI presence with NO sim-component fork (first D6 assertion). Links
#                         context_session (Session / InputState / hash_world in assertions) via
#                         context_ui_input's transitive context_input; static-links the GCC runtime like
#                         the sibling packages. Auto-runs in the general CI step (the `ui-*` family is
#                         not in the build job's -E gate-exclusion regex, built by --preset dev).
add_executable(context_ui_test_input_routing tests/test_input_routing.cpp)
target_link_libraries(context_ui_test_input_routing
    PRIVATE context_ui_input context_ui context_input context_session context_warnings)
target_link_options(context_ui_test_input_routing PRIVATE ${_context_ui_static_runtime})
target_include_directories(context_ui_test_input_routing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME ui-test_input_routing COMMAND context_ui_test_input_routing)

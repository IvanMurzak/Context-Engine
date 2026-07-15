// The runtime UI TypeScript-authoring binding shim (M7 T4 / a4, R-UI-001/006). The doubles-only bridge
// between an authored `context.ui` TypeScript surface running on the V8 host (src/runtime/js/) and a
// headless `context_ui` UiTree: a UiScriptContext owns a caller-supplied UiTree + a numeric StateStore
// and exposes a table of HostFunction-compatible primitives (tree construction, CSS-like style props,
// event handlers, read-only data binding) the host binds as globals the `context.ui` TS API wraps.
//
// Only DOUBLES cross the JsEngine::bindHostFunction seam (double(void*, const double*, size_t)), so the
// closed Role / EventType / Positioning / Flow vocabularies + NodeId handles + integer state keys all
// marshal as doubles — the numeric protocol below is mirrored 1:1 by the authored TS shim.
//
// This library is PURE STDLIB and JsEngine-FREE by design (UiHostFunction is byte-identical to
// js::HostFunction but declared here so no V8/js header leaks in): the js glue that binds the table +
// wires the handler callback to JsEngine::callFunction lives with the caller (src/runtime/ts/). So the
// shim's C++ half builds + unit-tests on EVERY toolchain with no V8 link (the m6-exit-2 / test_ts_in_v8
// split): the shim is a LOCAL gate; the authored-TS-in-V8 proof is CI-only for its V8 dependency path.
//
// UI is presentation (D6): the StateStore is NOT sim state and folds into no state hash; UI->state is
// only the action path (write/add), state->UI is read-only (data binding).

#pragma once

#include "context/packages/ui/events.h"
#include "context/packages/ui/ui_node.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace context::packages::ui
{

class UiTree;

// The read-only data-binding source, keyed by an integer state key. A string key cannot cross the
// doubles-only bindHostFunction seam, so authored TS keys state by a small closed set of numeric ids
// (the app defines their meaning). Presentation (D6): never hashed sim state.
class StateStore
{
public:
    void set(std::int32_t key, double value);
    void add(std::int32_t key, double delta); // read-modify-write (the button-action path)
    [[nodiscard]] double get(std::int32_t key) const noexcept; // 0.0 for an unset key
    [[nodiscard]] bool has(std::int32_t key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

private:
    std::unordered_map<std::int32_t, double> values_;
};

// A HostFunction-compatible primitive — BYTE-IDENTICAL to context::runtime::js::HostFunction
// (double(*)(void*, const double*, std::size_t)) but declared HERE so this library never includes a
// V8/js header (keeps context_ui_script js-free; the runtime/ts glue binds the table to the engine, the
// two signatures being the same type by construction).
using UiHostFunction = double (*)(void* user, const double* args, std::size_t nargs);

// One named primitive the host installs as a global that the `context.ui` TS surface calls. `user` is a
// UiScriptContext*.
struct UiHostBinding
{
    const char* name;
    UiHostFunction fn;
};

// The numeric protocol the doubles-only primitives use (mirrored 1:1 by the authored TS shim). Role /
// EventType / Positioning / Flow cross as their enum declaration-order index; a bad index is rejected.
[[nodiscard]] bool decode_role(double raw, Role& out) noexcept;
[[nodiscard]] bool decode_event_type(double raw, EventType& out) noexcept;
[[nodiscard]] bool decode_positioning(double raw, Positioning& out) noexcept;
[[nodiscard]] bool decode_flow(double raw, Flow& out) noexcept;

// The retained-tree authoring context the doubles-only primitives operate on. The caller owns the
// UiTree + StateStore; this binds an authored `context.ui` program to them. Handler dispatch calls back
// into the authored TS via a caller-installed invoker (runtime/ts wires it to
// JsEngine::callFunction("__ui_invoke", ...)); a pure-C++ test installs a local invoker so the
// dispatch->handler bridge is exercised with no V8.
class UiScriptContext
{
public:
    UiScriptContext(UiTree& tree, StateStore& state) noexcept;

    [[nodiscard]] UiTree& tree() noexcept { return tree_; }
    [[nodiscard]] const UiTree& tree() const noexcept { return tree_; }
    [[nodiscard]] StateStore& state() noexcept { return state_; }
    [[nodiscard]] const StateStore& state() const noexcept { return state_; }

    // The dispatch->handler callback: invoked with the authored handler id + the firing event when a
    // node the TS registered a handler on receives a matching event. runtime/ts wires it to the V8
    // host (__ui_invoke); a C++ test installs its own to assert routing without V8.
    void set_invoker(std::function<void(std::uint32_t handler_id, Event& ev)> invoker);

    // --- the doubles-only primitives (also reachable via ui_host_bindings() for the host) ----------
    // Public so the pure-C++ unit test drives them EXACTLY as the authored TS would.

    // Create a `role` node under `parent`; returns kInvalidNode for an invalid parent.
    [[nodiscard]] NodeId create(NodeId parent, Role role);
    bool set_opacity(NodeId id, double opacity);            // clamped [0,1]
    bool set_visible(NodeId id, bool visible);
    bool set_background(NodeId id, double r, double g, double b, double a); // 0..255 channels
    bool set_foreground(NodeId id, double r, double g, double b, double a);
    bool set_padding(NodeId id, double padding);
    // A small CSS-like layout box: Flow/Absolute positioning + size + how this node stacks its own
    // Flow children (None/Row/Column) + inter-child gap. Marks region damage.
    bool set_layout_box(NodeId id, Positioning pos, double w, double h, Flow flow, double gap);
    bool set_bounds(NodeId id, double x, double y, double w, double h);

    // Register the authored handler `handler_id` to fire for `type` on `id`.
    void on(NodeId id, EventType type, std::uint32_t handler_id);

    // Read-only data binding: bind `id`'s displayed value to the numeric state `key`.
    bool bind_value(NodeId id, std::int32_t state_key);
    [[nodiscard]] bool is_bound(NodeId id) const noexcept;
    // The CURRENT value the binding resolves to (state readback through the binding); 0.0 if unbound.
    [[nodiscard]] double bound_value(NodeId id) const noexcept;

    // State access: the UI->state action path (write/add) + the read-only data-binding read source.
    void write_state(std::int32_t key, double value);
    void add_state(std::int32_t key, double delta);
    [[nodiscard]] double read_state(std::int32_t key) const noexcept;

    // Invoked by the tree handler closure `on()` installed — forwards to the caller-installed invoker.
    void invoke_handler(std::uint32_t handler_id, Event& ev);

private:
    UiTree& tree_;
    StateStore& state_;
    std::function<void(std::uint32_t, Event&)> invoker_;
    std::unordered_map<NodeId, std::int32_t> bindings_; // node -> state key (read-only binding)
};

// The table of primitives the host binds as globals (user = &UiScriptContext). Names match the
// `context.ui` TS surface's low-level calls (e.g. "__ui_create"). Stable order; never empty.
[[nodiscard]] const std::vector<UiHostBinding>& ui_host_bindings();

} // namespace context::packages::ui

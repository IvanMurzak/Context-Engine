// The runtime UI TypeScript-authoring binding shim (M7 T4 / a4, R-UI-001/006) — the PURE-C++ half.
// Drives the doubles-only UiScriptContext primitives + the host-function table EXACTLY as the authored
// context.ui TS surface would through JsEngine::bindHostFunction (which passes doubles only), with NO V8
// link — so this is a LOCAL gate on all three OS legs (the m6-exit-2 split; the authored-TS-in-V8
// end-to-end proof is CI-only in ts-test_ui_ts_authoring). Covers happy path, edge cases, AND failures.

#include "context/packages/ui/script_bindings.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace context::packages::ui;

namespace
{

// Look up a host-function-table entry by name (the host binds these; here we call them directly).
UiHostFunction host_fn(const std::string& name)
{
    for (const UiHostBinding& b : ui_host_bindings())
        if (name == b.name)
            return b.fn;
    return nullptr;
}

// Call a table entry as the host would (a doubles vector), returning its double result.
double call_host(void* user, const std::string& name, const std::vector<double>& args)
{
    UiHostFunction fn = host_fn(name);
    if (fn == nullptr)
        return -1.0;
    return fn(user, args.data(), args.size());
}

} // namespace

int main()
{
    // --- the numeric protocol decoders round-trip the closed vocabularies + reject out-of-range ----
    {
        Role role = Role::Group;
        CHECK(decode_role(0.0, role) && role == Role::Root);
        CHECK(decode_role(4.0, role) && role == Role::Button);
        CHECK(decode_role(11.0, role) && role == Role::ListItem);
        CHECK(!decode_role(12.0, role)); // out of range
        CHECK(!decode_role(-1.0, role));

        EventType et = EventType::Custom;
        CHECK(decode_event_type(0.0, et) && et == EventType::PointerDown);
        CHECK(decode_event_type(9.0, et) && et == EventType::Custom);
        CHECK(!decode_event_type(10.0, et));

        Positioning pos = Positioning::Flow;
        CHECK(decode_positioning(1.0, pos) && pos == Positioning::Absolute);
        CHECK(!decode_positioning(2.0, pos));

        Flow flow = Flow::None;
        CHECK(decode_flow(2.0, flow) && flow == Flow::Column);
        CHECK(!decode_flow(3.0, flow));
    }

    // --- StateStore: unset reads as 0, set/add/has ------------------------------------------------
    {
        StateStore s;
        CHECK(!s.has(1));
        CHECK(s.get(1) == 0.0);
        s.set(1, 5.0);
        CHECK(s.has(1));
        CHECK(s.get(1) == 5.0);
        s.add(1, 3.0);
        CHECK(s.get(1) == 8.0);
        s.add(2, 4.0); // add to an unset key initializes from 0
        CHECK(s.get(2) == 4.0);
    }

    // --- happy path: build a HUD-like tree through the CONTEXT primitives, then dispatch an event
    //     whose handler mutates state, and assert the read-only data binding reflects the new state.
    //     This is the C++ mirror of the authored-TS DoD (build tree -> dispatch -> assert readback).
    {
        constexpr std::int32_t kScoreKey = 100;
        UiTree tree;
        StateStore state;
        UiScriptContext ctx(tree, state);

        // The dispatch->handler bridge: a C++ invoker stands in for the V8 __ui_invoke callback. It
        // records which authored handler fired and (like the authored onClick) bumps the score.
        std::vector<std::uint32_t> fired;
        ctx.set_invoker([&](std::uint32_t handler_id, Event& ev) {
            fired.push_back(handler_id);
            ev.handled = true;
            ctx.add_state(kScoreKey, 10.0);
        });

        // Build: a column HUD panel with a score label (bound to the score) + a button.
        const NodeId panel = ctx.create(tree.root(), Role::Panel);
        CHECK(panel != kInvalidNode);
        CHECK(ctx.set_layout_box(panel, Positioning::Flow, 200.0, 80.0, Flow::Column, 4.0));
        CHECK(ctx.set_background(panel, 0.0, 0.0, 0.0, 160.0));

        const NodeId score = ctx.create(panel, Role::Label);
        CHECK(score != kInvalidNode);
        CHECK(ctx.set_foreground(score, 255.0, 255.0, 255.0, 255.0));
        ctx.write_state(kScoreKey, 0.0);
        CHECK(ctx.bind_value(score, kScoreKey));
        CHECK(ctx.is_bound(score));
        CHECK(ctx.bound_value(score) == 0.0); // reflects initial state

        const NodeId button = ctx.create(panel, Role::Button);
        CHECK(button != kInvalidNode);
        ctx.on(button, EventType::PointerDown, 7u); // authored handler id 7

        // Structure was built from the primitives: root + panel + score + button = 4 live nodes.
        CHECK(tree.node_count() == 4);
        CHECK(tree.node(panel)->role == Role::Panel);
        CHECK(tree.node(score)->role == Role::Label);
        CHECK(tree.node(button)->role == Role::Button);
        CHECK(tree.descendant_count(panel) == 2);

        // Dispatch a pointer-down on the button: the handler fires and bumps the score.
        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = button;
        tree.dispatch(ev);

        CHECK(fired.size() == 1);
        CHECK(fired[0] == 7u);
        CHECK(ev.handled);

        // State readback: the action bumped the score, and the read-only data binding reflects it.
        CHECK(ctx.read_state(kScoreKey) == 10.0);
        CHECK(ctx.bound_value(score) == 10.0);

        // A second dispatch accumulates (proves it is the real store, not a one-shot).
        Event ev2;
        ev2.type = EventType::PointerDown;
        ev2.target = button;
        tree.dispatch(ev2);
        CHECK(ctx.read_state(kScoreKey) == 20.0);
        CHECK(ctx.bound_value(score) == 20.0);
    }

    // --- the host-function TABLE decodes doubles the way the V8 host feeds them --------------------
    {
        constexpr std::int32_t kHpKey = 200;
        UiTree tree;
        StateStore state;
        UiScriptContext ctx(tree, state);

        std::vector<std::uint32_t> fired;
        ctx.set_invoker([&](std::uint32_t id, Event&) { fired.push_back(id); });

        void* user = &ctx;
        // __ui_create(root=0, Panel=1) -> a real node id
        const double panel_d = call_host(user, "__ui_create", {0.0, 1.0});
        const NodeId panel = static_cast<NodeId>(static_cast<std::uint32_t>(panel_d));
        CHECK(panel != kInvalidNode);
        CHECK(tree.node(panel) != nullptr);

        // style props via the table
        CHECK(call_host(user, "__ui_set_opacity", {panel_d, 0.5}) == 1.0);
        CHECK(tree.node(panel)->style.opacity == 0.5f);
        CHECK(call_host(user, "__ui_set_background", {panel_d, 10.0, 20.0, 30.0, 40.0}) == 1.0);
        CHECK((tree.node(panel)->style.background == Color{10, 20, 30, 40}));
        CHECK(call_host(user, "__ui_set_layout", {panel_d, 0.0, 100.0, 50.0, 2.0, 3.0}) == 1.0);
        CHECK(tree.node(panel)->layout.flow == Flow::Column);
        CHECK(call_host(user, "__ui_set_visible", {panel_d, 0.0}) == 1.0);
        CHECK(!tree.node(panel)->style.visible);

        // __ui_create(panel, Button=4) then bind + on via the table
        const double btn_d = call_host(user, "__ui_create", {panel_d, 4.0});
        const NodeId button = static_cast<NodeId>(static_cast<std::uint32_t>(btn_d));
        CHECK(button != kInvalidNode);
        CHECK(call_host(user, "__ui_bind_value", {btn_d, static_cast<double>(kHpKey)}) == 1.0);

        // __ui_write_state / __ui_add_state / __ui_read_state
        CHECK(call_host(user, "__ui_write_state", {static_cast<double>(kHpKey), 3.0}) == 3.0);
        CHECK(call_host(user, "__ui_add_state", {static_cast<double>(kHpKey), 2.0}) == 5.0);
        CHECK(call_host(user, "__ui_read_state", {static_cast<double>(kHpKey)}) == 5.0);
        CHECK(ctx.bound_value(button) == 5.0);

        // __ui_on(button, PointerDown=0, handler 42) then a real dispatch fires it
        CHECK(call_host(user, "__ui_on", {btn_d, 0.0, 42.0}) == 1.0);
        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = button;
        tree.dispatch(ev);
        CHECK(fired.size() == 1 && fired[0] == 42u);
    }

    // --- failure paths: bad parent, dead node, bad enum indices ------------------------------------
    {
        UiTree tree;
        StateStore state;
        UiScriptContext ctx(tree, state);
        void* user = &ctx;

        // create under an invalid parent -> kInvalidNode
        CHECK(ctx.create(999999u, Role::Panel) == kInvalidNode);
        // a bad role index -> __ui_create returns kInvalidNode
        CHECK(call_host(user, "__ui_create", {0.0, 99.0}) == static_cast<double>(kInvalidNode));
        // style mutation on a dead node -> false/0
        CHECK(!ctx.set_opacity(999999u, 0.5));
        CHECK(call_host(user, "__ui_set_opacity", {123456.0, 0.5}) == 0.0);
        // a bad event-type index -> __ui_on returns 0 and registers nothing
        const double panel_d = call_host(user, "__ui_create", {0.0, 1.0});
        CHECK(call_host(user, "__ui_on", {panel_d, 99.0, 1.0}) == 0.0);
        // bound_value of an unbound node -> 0
        CHECK(!ctx.is_bound(static_cast<NodeId>(static_cast<std::uint32_t>(panel_d))));
        CHECK(ctx.bound_value(static_cast<NodeId>(static_cast<std::uint32_t>(panel_d))) == 0.0);
        // bind to a dead node -> false
        CHECK(!ctx.bind_value(999999u, 1));
    }

    UI_TEST_MAIN_END();
}

// Handler dispatch (M7 T1, R-UI-006): target-then-bubble delivery, propagation stop on handled, multiple
// handlers in registration order, the pointer/key/custom event kinds, and the minimal focus model.

#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

#include <string>
#include <vector>

using namespace context::packages::ui;

int main()
{
    UiTree tree;
    const NodeId panel = tree.create_node(Role::Panel, tree.root());
    const NodeId button = tree.create_node(Role::Button, panel);

    // --- bubbling: an event on `button` visits button -> panel -> root ----------------------------
    {
        std::vector<NodeId> visited;
        tree.add_handler(button, EventType::PointerDown, [&](Event& e) { visited.push_back(e.current); });
        tree.add_handler(panel, EventType::PointerDown, [&](Event& e) { visited.push_back(e.current); });
        tree.add_handler(tree.root(), EventType::PointerDown,
                         [&](Event& e) { visited.push_back(e.current); });

        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = button;
        ev.pointer_x = 12.0f;
        ev.pointer_y = 8.0f;
        tree.dispatch(ev);

        CHECK(visited.size() == 3);
        CHECK(visited[0] == button);
        CHECK(visited[1] == panel);
        CHECK(visited[2] == tree.root());
        CHECK(ev.current == tree.root()); // last visited
    }

    // --- handled stops propagation ----------------------------------------------------------------
    {
        UiTree t;
        const NodeId p = t.create_node(Role::Panel, t.root());
        const NodeId b = t.create_node(Role::Button, p);
        int panel_hits = 0;
        t.add_handler(b, EventType::PointerUp, [](Event& e) { e.handled = true; });
        t.add_handler(p, EventType::PointerUp, [&](Event&) { ++panel_hits; });

        Event ev;
        ev.type = EventType::PointerUp;
        ev.target = b;
        t.dispatch(ev);
        CHECK(ev.handled);
        CHECK(panel_hits == 0); // panel never reached
    }

    // --- multiple handlers on one node fire in registration order ---------------------------------
    {
        UiTree t;
        const NodeId b = t.create_node(Role::Button, t.root());
        std::string order;
        t.add_handler(b, EventType::Custom, [&](Event&) { order += "a"; });
        t.add_handler(b, EventType::Custom, [&](Event&) { order += "b"; });
        Event ev;
        ev.type = EventType::Custom;
        ev.custom_name = "click";
        ev.target = b;
        t.dispatch(ev);
        CHECK(order == "ab");
    }

    // --- event kind is respected: a KeyDown handler ignores a PointerMove -------------------------
    {
        UiTree t;
        const NodeId b = t.create_node(Role::TextInput, t.root());
        int key_hits = 0;
        std::string got_key;
        t.add_handler(b, EventType::KeyDown, [&](Event& e) { ++key_hits; got_key = e.key; });

        Event move;
        move.type = EventType::PointerMove;
        move.target = b;
        t.dispatch(move);
        CHECK(key_hits == 0);

        Event key;
        key.type = EventType::KeyDown;
        key.target = b;
        key.key = "Enter";
        t.dispatch(key);
        CHECK(key_hits == 1);
        CHECK(got_key == "Enter");
    }

    // --- dispatch to an invalid/dead target is a no-op --------------------------------------------
    {
        UiTree t;
        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = kInvalidNode;
        t.dispatch(ev); // must not crash
        CHECK(!ev.handled);
    }

    // --- focus model: set_focus emits FocusLost(old) then FocusGained(new) ------------------------
    {
        UiTree t;
        const NodeId a = t.create_node(Role::Button, t.root());
        const NodeId b = t.create_node(Role::Button, t.root());
        std::vector<std::string> log;
        t.add_handler(a, EventType::FocusGained, [&](Event&) { log.push_back("a:gained"); });
        t.add_handler(a, EventType::FocusLost, [&](Event&) { log.push_back("a:lost"); });
        t.add_handler(b, EventType::FocusGained, [&](Event&) { log.push_back("b:gained"); });

        CHECK(t.focused() == kInvalidNode);
        CHECK(t.set_focus(a));
        CHECK(t.focused() == a);
        CHECK(t.set_focus(a)); // idempotent, no extra events
        CHECK(t.set_focus(b));
        CHECK(t.focused() == b);

        CHECK(log.size() == 3);
        CHECK(log[0] == "a:gained");
        CHECK(log[1] == "a:lost");
        CHECK(log[2] == "b:gained");

        CHECK(!t.set_focus(999999u));     // a non-existent id is refused
        CHECK(t.focused() == b);           // focus unchanged by the refused call
        CHECK(t.set_focus(kInvalidNode));  // clearing focus is allowed
        CHECK(t.focused() == kInvalidNode);
    }

    UI_TEST_MAIN_END();
}

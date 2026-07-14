// Null-provider zero-cost (M7 T1, R-UI-006): the null/headless provider reports all-false capabilities
// and its present() does NO rendering work — it never walks the tree. Contrasted with a recording
// provider whose work scales with the tree, this pins the "UI logic runs headless with zero render
// cost" guarantee. Also: the full tree logic runs with NO provider attached at all.

#include "context/packages/ui/null_provider.h"
#include "context/packages/ui/provider.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

using namespace context::packages::ui;

namespace
{
// A provider that DOES real per-node work: on present it walks the tree and "draws" every visible node,
// so its draw count scales with the tree. The foil for the null provider's zero-cost.
class RecordingProvider final : public UiProvider
{
public:
    [[nodiscard]] Capabilities capabilities() const override
    {
        Capabilities caps;
        caps.gpu_driver = true;
        caps.damage_repaint = true;
        return caps;
    }

    void present(const UiTree& tree, const RepaintPlan&) override
    {
        draw_calls = 0;
        walk(tree, tree.root());
    }

    std::size_t draw_calls = 0;

private:
    void walk(const UiTree& tree, NodeId id)
    {
        const UiNode* n = tree.node(id);
        if (n == nullptr)
            return;
        if (n->style.visible)
            ++draw_calls;
        for (const NodeId child : n->children)
            walk(tree, child);
    }
};

[[nodiscard]] std::size_t visible_count(const UiTree& tree, NodeId id)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr)
        return 0;
    std::size_t c = n->style.visible ? 1u : 0u;
    for (const NodeId child : n->children)
        c += visible_count(tree, child);
    return c;
}
} // namespace

int main()
{
    // Build a small HUD-ish tree.
    UiTree tree;
    const NodeId hud = tree.create_node(Role::Panel, tree.root());
    const NodeId score = tree.create_node(Role::Label, hud);
    const NodeId health = tree.create_node(Role::ProgressBar, hud);
    tree.set_text(score, "Score: 0");
    (void)health;

    const RepaintPlan full_plan = negotiate_repaint(Capabilities{}, tree.take_damage(), Rect{0, 0, 640, 480});

    // --- the null provider: all-false caps, present is a pure no-op -------------------------------
    NullProvider null;
    const Capabilities caps = null.capabilities();
    CHECK(!caps.gpu_driver && !caps.damage_repaint && !caps.composited_transforms);
    CHECK(!caps.text_shaping && !caps.bidi && !caps.ime);

    const std::size_t nodes_before = tree.node_count();
    for (int i = 0; i < 5; ++i)
        null.present(tree, full_plan);
    CHECK(null.frames_presented() == 5);
    // present did NOT mutate the tree (no rendering side effects, no relayout, no state churn)
    CHECK(tree.node_count() == nodes_before);
    CHECK(tree.node(score)->text == "Score: 0");

    // --- the foil: a recording provider does per-node work on the SAME tree -----------------------
    RecordingProvider rec;
    rec.present(tree, full_plan);
    const std::size_t visible = visible_count(tree, tree.root());
    CHECK(visible == 4); // root + hud + score + health, all visible
    CHECK(rec.draw_calls == visible);
    CHECK(rec.draw_calls > 0); // a real provider renders; the null provider rendered nothing

    // --- R-UI-006: the full logic runs headless with NO provider attached -------------------------
    {
        UiTree t;
        const NodeId b = t.create_node(Role::Button, t.root());
        int clicks = 0;
        t.add_handler(b, EventType::PointerDown, [&](Event&) { ++clicks; });
        Event ev;
        ev.type = EventType::PointerDown;
        ev.target = b;
        t.dispatch(ev);
        CHECK(clicks == 1);
        CHECK(!t.take_damage().empty()); // the create produced damage — logic ran, no renderer needed
    }

    UI_TEST_MAIN_END();
}

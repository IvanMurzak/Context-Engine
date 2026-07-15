// M7 exit criterion 1 — `m7-exit-1-hud-headless` (design 2026-07-13-m7-runtime-ui / a12-m7-exit;
// R-UI-001/005/006, D6): the authored platformer-2d HUD (samples/platformer-2d/ui/hud.ui-hud.json)
// RENDERS + DATA-BINDS with no GPU. Two halves, both headless (the R-UI-006 guarantee):
//
//   * DATA-BINDS — the authored HUD is loaded through the REAL a5 `context ui` load path in-process
//     (cli::run — the same headless layout + numeric data-binding the shipped verbs run): the score
//     label + health bar resolve their read-only bindings to the seeded numeric state, layout computed
//     real rects, and a click on the collect button scores points through the UI->state action path.
//   * RENDERS HEADLESS — the a6 engine-integrated UI backend's read-only EXTRACT step (extract_ui,
//     src/render/ui/) walks a HUD's retained tree into flat draw quads with NO GPU (drives no rhi.h
//     device at all), and the null/headless provider accepts the frame at zero render cost. The
//     pixel-exact HUD is the sibling `render` job's ui-hud SSIM golden (native + web); THIS gate is the
//     headless extract/present logic that feeds it — the health FILL overdraws its bar in painter order.
//
// Runs in the blocking "M7 exit gate" build-job step on all three OS legs. UI is presentation (D6): none
// of this touches the sim World (the m7-exit-4 gate pins hash_world invariance separately).

#include "m7_exit_test.h"

#include "context/cli/app.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/packages/ui/null_provider.h"
#include "context/packages/ui/provider.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"
#include "context/render/ui/snapshot.h"

#include <optional>
#include <string>
#include <vector>

#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace cli = context::cli;
namespace contract = context::editor::contract;
namespace ui = context::packages::ui;
namespace rui = context::render::ui;

using context::tests::m7::report;

namespace
{

const std::string kHud = std::string(CONTEXT_SAMPLES_DIR) + "/platformer-2d/ui/hud.ui-hud.json";

// The dumped node named `name`, or a null Json if absent.
const contract::Json& node_by_name(const contract::Json& nodes, const std::string& name)
{
    static const contract::Json kNull;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes.at(i).contains("name") && nodes.at(i).at("name").as_string() == name)
            return nodes.at(i);
    return kNull;
}

// --- half 1: the authored HUD data-binds headless through the REAL a5 load path -------------------
void test_data_binds_headless()
{
    // ui dump — the whole retained tree, computed rects, resolved bindings, numeric state (headless).
    const contract::Envelope dumped = cli::run({"ui", "dump", kHud});
    CHECK(dumped.ok());
    if (!dumped.ok())
        return;
    const contract::Json& data = dumped.data();
    CHECK(data.at("nodeCount").as_number() == 5.0); // root + panel + label + bar + button

    // Layout ran: the panel resolved a real box (220x96 from the authored flow layout).
    const contract::Json& nodes = data.at("nodes");
    bool panel_laid_out = false;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const contract::Json& n = nodes.at(i);
        if (n.at("role").as_string() == "panel")
        {
            panel_laid_out = n.at("rect").at("w").as_number() == 220.0 &&
                             n.at("rect").at("h").as_number() == 96.0;
        }
    }
    CHECK(panel_laid_out);

    // Read-only data binding: the seeded state reaches each bound node's resolved value.
    CHECK(data.at("state").at("score").as_number() == 0.0);
    CHECK(data.at("state").at("health").as_number() == 100.0);
    const contract::Json& score = node_by_name(nodes, "score-label");
    const contract::Json& health = node_by_name(nodes, "health-bar");
    CHECK(score.contains("boundState") && score.at("boundState").as_string() == "score");
    CHECK(score.contains("boundValue") && score.at("boundValue").as_number() == 0.0);
    CHECK(health.contains("boundState") && health.at("boundState").as_string() == "health");
    CHECK(health.contains("boundValue") && health.at("boundValue").as_number() == 100.0);

    // ui query — the single-node headless read the drive loop uses.
    const contract::Envelope q = cli::run({"ui", "query", kHud, "health-bar"});
    CHECK(q.ok());
    if (q.ok())
        CHECK(q.data().at("boundValue").as_number() == 100.0);

    // ui send click — the UI->state action path (collect: +10 score, -5 health), all headless.
    const contract::Envelope clicked = cli::run({"ui", "send", kHud, "click", "--target", "collect-button"});
    CHECK(clicked.ok());
    if (clicked.ok())
    {
        CHECK(clicked.data().at("state").at("score").as_number() == 10.0);
        CHECK(clicked.data().at("state").at("health").as_number() == 95.0);
    }

    // ui assert — fail-closed: the correct bound value passes, a wrong one fails.
    CHECK(cli::run({"ui", "assert", kHud, "health-bar", "--value", "100"}).ok());
    CHECK(!cli::run({"ui", "assert", kHud, "health-bar", "--value", "999"}).ok());
    CHECK(cli::run({"ui", "assert", kHud, "score-label", "--value", "0"}).ok());
}

// --- half 2: a HUD renders headless — the a6 extract produces the colored draw quads, no GPU --------
void test_renders_headless()
{
    // A HUD render tree mirroring the platformer HUD's colored rects (explicit bounds — the render form;
    // the authored flow HUD's text-sized children carry no fill box, exactly as build_reference_hud does
    // for the ui-hud golden). Panel background, a health bar background with a green FILL child that
    // paints on top, and the collect button.
    ui::UiTree tree;
    const ui::NodeId root = tree.root();

    auto rect_node = [&tree](ui::NodeId parent, ui::Role role, const ui::Rect& r,
                             const ui::Color& bg) -> ui::NodeId
    {
        const ui::NodeId id = tree.create_node(role, parent);
        tree.set_bounds(id, r);
        ui::Style s;
        s.background = bg;
        tree.set_style(id, s);
        return id;
    };

    const ui::NodeId panel = rect_node(root, ui::Role::Panel, ui::Rect{8, 8, 220, 96}, ui::Color{0, 0, 0, 160});
    const ui::NodeId bar = rect_node(panel, ui::Role::ProgressBar, ui::Rect{16, 40, 120, 16},
                                     ui::Color{40, 40, 48, 255});
    const ui::NodeId fill = rect_node(bar, ui::Role::ProgressBar, ui::Rect{16, 40, 90, 16},
                                      ui::Color{0, 200, 0, 255}); // health fill, paints ON the bar
    const ui::NodeId button = rect_node(panel, ui::Role::Button, ui::Rect{16, 64, 120, 20},
                                        ui::Color{40, 40, 60, 255});
    CHECK(panel != ui::kInvalidNode && bar != ui::kInvalidNode && fill != ui::kInvalidNode &&
          button != ui::kInvalidNode);

    // The a6 read-only EXTRACT: walk the tree into flat draw quads, pre-order (painter's algorithm). No
    // GPU device is touched — this is the headless extract the ui-hud golden's provider then rasterizes.
    rui::UiRenderSnapshot snap;
    rui::extract_ui(tree, ui::Rect{0, 0, 240, 135}, snap);
    CHECK(snap.quads.size() == 4); // panel + bar-bg + fill + button (all opaque, non-empty)

    // Painter order (the sort key rises monotonically) and the health FILL overdraws its bar background.
    std::optional<std::uint32_t> bar_order;
    std::optional<std::uint32_t> fill_order;
    bool button_drawn = false;
    std::uint32_t prev = 0;
    bool first = true;
    for (const rui::UiQuad& q : snap.quads)
    {
        if (!first)
            CHECK(q.order >= prev);
        prev = q.order;
        first = false;
        if (q.color == ui::Color{40, 40, 48, 255})
            bar_order = q.order;
        if (q.color == ui::Color{0, 200, 0, 255})
            fill_order = q.order;
        if (q.color == ui::Color{40, 40, 60, 255})
            button_drawn = true;
    }
    CHECK(bar_order.has_value() && fill_order.has_value());
    CHECK(bar_order.has_value() && fill_order.has_value() && *fill_order > *bar_order); // fill on top
    CHECK(button_drawn);

    // The null / headless provider accepts the frame at ZERO render cost (R-UI-006): present() does no
    // draw work, only proves the contract ran. The negotiation falls back to a FULL repaint because the
    // null provider advertises no damage_repaint (the R-UI-005 fallback table).
    ui::NullProvider provider;
    const ui::Capabilities caps = provider.capabilities();
    CHECK(!caps.gpu_driver && !caps.damage_repaint); // headless: renders nothing
    const ui::RepaintPlan plan =
        ui::negotiate_repaint(caps, tree.take_damage(), ui::Rect{0, 0, 240, 135});
    CHECK(plan.full_repaint); // no damage_repaint support => full repaint
    provider.present(tree, plan);
    CHECK(provider.frames_presented() == 1);
}

} // namespace

int main()
{
    test_data_binds_headless();
    test_renders_headless();
    return report("m7-exit-1-hud-headless",
                  "the authored platformer HUD data-binds + renders headless (no GPU)");
}

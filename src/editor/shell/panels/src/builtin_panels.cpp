// The panel composition root (see builtin_panels.h for the D10 layering rationale).

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/client/client.h" // the pump's live daemon reads (complete type HERE only)
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/shell/panels/inspector_feed.h"
#include "context/editor/shell/panels/problems_feed.h" // ProblemsFeed complete type (see builtin_panels.h)
#include "context/editor/shell/panels/scenetree_feed.h"
#include "context/editor/shell/panels/session_feed.h" // SessionFeed complete type (e08b)

#include <cstdio>
#include <string_view>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The roster id of the M5-F0b placeholder panel. A LITERAL because `gui/uitree/builtin.h` declares
// no id constant (builtin_roster.h records the same exception for the same reason) — the a11y
// coverage ctest is what keeps the two spellings honest.
constexpr const char* kPlaceholderPanelId = "placeholder";

// One synchronous daemon read for the pump below: claim-first is the CALLER's job (see the header's
// failure posture). Returns the reply's `result` (the R-CLI-008 envelope) or nullopt, reporting the
// failure to stderr — once per occurrence, because a claimed fetch is not retried until the next
// trigger, so this cannot spam.
[[nodiscard]] std::optional<contract::Json> pump_read(client::Client& client,
                                                      const std::string& method,
                                                      contract::Json params, const char* what)
{
    std::string error;
    std::optional<contract::Json> reply = client.call(method, std::move(params), error);
    if (!reply.has_value())
    {
        std::fprintf(stderr,
                     "context_editor: the %s read (%s) failed (%s); it will retry on the next "
                     "change\n",
                     what, method.c_str(), error.c_str());
    }
    return reply;
}

} // namespace

// Out-of-line so the feed types are complete here (builtin_panels.h forward-declares them to keep
// the header's include chain RTTI/CEF-clean — see that header's comment).
BuiltinPanels::BuiltinPanels() = default;
BuiltinPanels::~BuiltinPanels() = default;
BuiltinPanels::BuiltinPanels(BuiltinPanels&&) noexcept = default;
BuiltinPanels& BuiltinPanels::operator=(BuiltinPanels&&) noexcept = default;

// Same seam, non-member form (see builtin_panels.h): the complete `ProblemsFeed` type is available
// here because this TU includes `problems_feed.h` above.
void apply_problems_snapshot(ProblemsFeed& feed, const contract::Json& snapshot,
                             std::uint64_t generation)
{
    feed.apply_snapshot(snapshot, generation);
}

bool apply_problems_event(ProblemsFeed& feed, const std::string& topic, const contract::Json& payload,
                          std::uint64_t generation)
{
    return feed.apply_event(topic, payload, generation);
}

bool apply_scenetree_event(SceneTreeFeed& feed, const std::string& topic,
                           const contract::Json& payload, std::uint64_t generation)
{
    return feed.apply_event(topic, payload, generation);
}

// The e08b seams. The topic string is declared in BOTH headers for the RTTI/CEF reason builtin_panels.h
// documents; this is the one TU that sees both, so it is where the two spellings are pinned together.
static_assert(std::string_view(kSessionTopic) == std::string_view(kSessionTopicName),
              "the subscriber's topic string and the feed's must be the same string");

bool apply_session_event(SessionFeed& feed, const std::string& topic, const contract::Json& payload)
{
    return feed.apply_event(topic, payload);
}

void bind_session_client(SessionFeed& feed, client::Client* client, std::uint64_t client_id)
{
    feed.bind_client(client, client_id);
}

void pump_panel_feeds(BuiltinPanels& panels, client::Client& client, const std::string& scene_path)
{
    // Scene tree: fetch when due AND a scene is named. The claim precedes the call (header: a
    // failure waits for the next settle rather than hammering).
    if (panels.scenetree != nullptr && !scene_path.empty() && panels.scenetree->fetch_due())
    {
        panels.scenetree->mark_fetched();
        contract::Json params = contract::Json::object();
        params.set("path", contract::Json(scene_path));
        if (const std::optional<contract::Json> reply =
                pump_read(client, "editor.scene-tree", std::move(params), "scene-tree"))
        {
            (void)panels.scenetree->apply_result(*reply);
        }
    }

    // Inspector: fetch the pending selection. `scene_path` addresses the same root scene the tree
    // was built from — a selection can only have come from it.
    if (panels.inspector != nullptr && !scene_path.empty() &&
        panels.inspector->pending().has_value())
    {
        const std::string identity = *panels.inspector->pending();
        panels.inspector->mark_fetched();
        contract::Json params = contract::Json::object();
        params.set("path", contract::Json(scene_path));
        params.set("idPath", contract::Json(identity));
        if (const std::optional<contract::Json> reply =
                pump_read(client, "editor.inspect", std::move(params), "inspector"))
        {
            (void)panels.inspector->apply_result(*reply);
        }
    }
}

const std::vector<std::string>& hostable_panel_ids()
{
    // Built once. Kept in lockstep with the bindings below by `test_builtin_panels.cpp`, which
    // asserts every id here is hosted after install and that nothing else is — so this list cannot
    // drift into a claim the bindings do not honor.
    static const std::vector<std::string> ids = {
        kPlaceholderPanelId,
        gui::panels::problems::ProblemsPanel::kContributionId,
        gui::panels::scenetree::SceneTreePanel::kContributionId,
        gui::panels::inspector::InspectorPanel::kContributionId,
        // e08b: hostable because the playbar stopped being a runtime-session driver. Its library now
        // links nothing but the headless uitree (gui/playbar/CMakeLists.txt explains the split), so
        // binding it here adds no weight to `context_editor`'s D10-audited closure.
        gui::playbar::PlaybarModel::kContributionId,
    };
    return ids;
}

BuiltinPanels install_builtin_panels(PanelHost& host)
{
    BuiltinPanels out;

    // --- the placeholder (M5-F0b, context_gui_uitree) -------------------------------------------
    // A pure function of nothing: a static, a11y-conformant tree. It has no model to feed, no
    // commands to dispatch beyond its own, no gestures and no state — so its provider is `build`
    // ALONE, and `panel.list` reports `gestures:false, persists:false` for it. That minimal shape is
    // useful on its own terms: it proves the host and the hydration runtime work for a panel that
    // supplies nothing optional, which is the floor every future panel clears.
    {
        PanelProvider provider;
        provider.build = [] { return gui::uitree::make_placeholder_panel(); };
        if (host.provide(kPlaceholderPanelId, std::move(provider)))
        {
            ++out.bound;
        }
    }

    // --- Problems (M5-F4, context_gui_panel_problems) -------------------------------------------
    // The panel the e05d1 DoD proves the LIVE read path on. The feed owns the model and is driven
    // from the daemon's `diagnostics` topic by the caller (see problems_feed.h); binding the
    // provider here is what publishes it through `panel.*`.
    {
        auto feed =
            std::make_unique<ProblemsFeed>(host, gui::panels::problems::ProblemsPanel::kContributionId);
        if (host.provide(gui::panels::problems::ProblemsPanel::kContributionId,
                         feed->make_provider()))
        {
            ++out.bound;
            out.problems = std::move(feed);
        }
        // On a refused binding the feed is DROPPED rather than kept: a feed nothing routes to would
        // pump daemon events into a model no renderer can ever see.
    }

    // --- the daemon session feed + the playbar (M9 e08b) -----------------------------------------
    // Constructed BEFORE the Scene tree, because it IS the SelectionGateway that panel writes
    // through: the panel takes the gateway at construction, and a gateway must outlive its panel.
    // Binding the playbar provider here is what makes the L-51 indicator a live, daemon-fed surface
    // rather than a rostered-but-unhosted one.
    {
        auto session =
            std::make_unique<SessionFeed>(host, gui::playbar::PlaybarModel::kContributionId);
        if (host.provide(gui::playbar::PlaybarModel::kContributionId, session->make_provider()))
        {
            ++out.bound;
            out.session = std::move(session);
        }
        // A refused binding DROPS the feed (the ProblemsFeed rule) — and with it the Scene tree's
        // gateway, so the tree below is constructed with none and honestly renders a selection it
        // cannot change, rather than writing through a feed nothing routes to.
    }

    // --- Scene tree + Inspector (M9 e05d3, the D10-split pair) ----------------------------------
    // Hostable because their kernel-typed builders moved daemon-side (gui/panels/builders/): the
    // panel libraries on THIS closure are boundary-clean, and the models arrive as data over
    // `editor.scene-tree` / `editor.inspect` (see the feeds). The Scene tree's selection is what
    // drives the Inspector's fetch (R-HUX-011) — wired HERE, the one place holding both feeds.
    {
        auto scenetree = std::make_unique<SceneTreeFeed>(
            host, gui::panels::scenetree::SceneTreePanel::kContributionId, out.session.get());
        auto inspector = std::make_unique<InspectorFeed>(
            host, gui::panels::inspector::InspectorPanel::kContributionId);

        const bool tree_bound =
            host.provide(gui::panels::scenetree::SceneTreePanel::kContributionId,
                         scenetree->make_provider());
        const bool inspector_bound =
            host.provide(gui::panels::inspector::InspectorPanel::kContributionId,
                         inspector->make_provider());

        if (tree_bound)
        {
            ++out.bound;
        }
        if (inspector_bound)
        {
            ++out.bound;
        }

        // The selection loop, wired only when BOTH ends exist. The raw capture is safe by
        // construction: both feeds live (and die) together in this bag, and no selection fires
        // during destruction (builtin_panels.h documents the member order that keeps it so).
        if (tree_bound && inspector_bound)
        {
            InspectorFeed* inspector_ptr = inspector.get();
            scenetree->panel().add_selection_listener(
                [inspector_ptr](const gui::panels::scenetree::SceneSelection& selection)
                {
                    if (selection.identity.empty())
                    {
                        inspector_ptr->request_clear();
                    }
                    else
                    {
                        inspector_ptr->request(selection.identity);
                    }
                });
        }

        if (tree_bound)
        {
            out.scenetree = std::move(scenetree);
        }
        if (inspector_bound)
        {
            out.inspector = std::move(inspector);
        }
        // A refused binding drops its feed (the ProblemsFeed rule); the selection listener was only
        // wired when both bound, so a dangling capture cannot survive this scope.

        // e08b: point the session feed at the panel whose rendered selection it drives. Done AFTER
        // the move, so the pointer is into the feed that will actually live in the bag.
        if (out.session != nullptr && out.scenetree != nullptr)
        {
            out.session->bind_scene_tree(&out.scenetree->panel(),
                                         gui::panels::scenetree::SceneTreePanel::kContributionId);
        }
    }

    return out;
}

} // namespace context::editor::shell::panels

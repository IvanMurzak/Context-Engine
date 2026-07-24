// The window-management bridge surface for editor-core (M9 e10b, drag surface e10c) — see
// window_bridge.h for the model.

#include "context/editor/shell/window_bridge.h"

#include "context/editor/shell/cross_window_drag.h" // the drag relay served by drag.probe/report-zone

#include <set>
#include <utility>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------- WindowMoveStore

void WindowMoveStore::set_boot_seed(WindowId target, PanelSeed seed)
{
    boot_seeds_[target] = std::move(seed);
}

std::optional<PanelSeed> WindowMoveStore::take_boot_seed(WindowId target)
{
    const auto it = boot_seeds_.find(target);
    if (it == boot_seeds_.end())
    {
        return std::nullopt;
    }
    PanelSeed seed = std::move(it->second);
    boot_seeds_.erase(it);
    return seed;
}

bool WindowMoveStore::has_boot_seed(WindowId target) const
{
    return boot_seeds_.find(target) != boot_seeds_.end();
}

void WindowMoveStore::enqueue_rehome(WindowId target, PanelSeed seed)
{
    rehome_queues_[target].push_back(std::move(seed));
}

std::vector<PanelSeed> WindowMoveStore::take_rehomed(WindowId target)
{
    const auto it = rehome_queues_.find(target);
    if (it == rehome_queues_.end())
    {
        return {};
    }
    std::vector<PanelSeed> pending = std::move(it->second);
    rehome_queues_.erase(it);
    return pending;
}

void WindowMoveStore::forget(WindowId target)
{
    boot_seeds_.erase(target);
    rehome_queues_.erase(target);
}

std::size_t WindowMoveStore::pending_rehomes(WindowId target) const
{
    const auto it = rehome_queues_.find(target);
    return it == rehome_queues_.end() ? 0u : it->second.size();
}

// ------------------------------------------------------------------------------------- WindowBridge

namespace
{

// A panel seed as it rides the wire IN (a `{panelId, state}` object). Total against renderer input:
// nullopt when there is no usable `panelId`, so the caller fails closed rather than seeding a nameless
// panel. `state` is OPAQUE — copied verbatim, never interpreted.
[[nodiscard]] std::optional<PanelSeed> read_seed(const contract::Json& params)
{
    if (!params.is_object())
    {
        return std::nullopt;
    }
    const contract::Json& panel_id = params.at("panelId");
    if (!panel_id.is_string() || panel_id.as_string().empty())
    {
        return std::nullopt;
    }
    PanelSeed seed;
    seed.panel_id = panel_id.as_string();
    // `contains` before copy so a missing `state` stays null rather than an empty object — the
    // "persists no state" case a panel is allowed to be in.
    seed.state = params.contains("state") ? params.at("state") : contract::Json{};
    return seed;
}

// A seed as it rides the wire OUT.
[[nodiscard]] contract::Json seed_json(const PanelSeed& seed)
{
    contract::Json out = contract::Json::object();
    out.set("panelId", contract::Json(seed.panel_id));
    out.set("state", seed.state);
    return out;
}

} // namespace

WindowBridge::WindowBridge(WindowId self_id, WindowMoveStore& store)
    : self_id_(self_id), store_(store)
{
}

void WindowBridge::bind_tear_out(TearOutHandler handler)
{
    tear_out_ = std::move(handler);
}

void WindowBridge::bind_move_to(MoveToHandler handler)
{
    move_to_ = std::move(handler);
}

void WindowBridge::bind_close(CloseHandler handler)
{
    close_ = std::move(handler);
}

void WindowBridge::bind_windows(WindowsProvider provider)
{
    windows_ = std::move(provider);
}

void WindowBridge::bind_drag_store(CrossWindowDragStore* store)
{
    drag_store_ = store;
}

void WindowBridge::bind_ui_mirror_store(UiMirrorStore* store)
{
    mirror_store_ = store;
}

contract::Json WindowBridge::list() const
{
    contract::Json out = contract::Json::object();
    out.set("windowId", contract::Json(static_cast<std::uint64_t>(self_id_)));
    contract::Json ids = contract::Json::array();
    if (windows_)
    {
        for (const WindowId id : windows_())
        {
            ids.push_back(contract::Json(static_cast<std::uint64_t>(id)));
        }
    }
    out.set("windows", std::move(ids));
    return out;
}

contract::Json WindowBridge::tear_out(const contract::Json& params, std::string& error_code)
{
    const std::optional<PanelSeed> seed = read_seed(params);
    if (!seed.has_value())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    ++tear_outs_;

    TearOut request;
    request.source = self_id_;
    request.seed = *seed;
    // A caller-supplied title is a convenience; the default is the app's own window title.
    const contract::Json& title = params.at("title");
    request.title = title.is_string() ? title.as_string() : std::string{};

    // No handler bound (a build with no way to make a window) is itself a LOUD, honest answer, not a
    // crash — editor-core degrades to a floating group exactly as on a factory failure.
    const WindowMoveResult result =
        tear_out_ ? tear_out_(request)
                  : WindowMoveResult{false, kInvalidWindowId,
                                     to_string(WindowCreateOutcome::no_factory),
                                     "no window factory is bound in this build"};

    contract::Json out = contract::Json::object();
    out.set("created", contract::Json(result.ok));
    out.set("windowId", contract::Json(static_cast<std::uint64_t>(result.window_id)));
    out.set("outcome", contract::Json(result.outcome));
    out.set("error", contract::Json(result.error));
    return out;
}

contract::Json WindowBridge::move_to(const contract::Json& params, std::string& error_code)
{
    const std::optional<PanelSeed> seed = read_seed(params);
    if (!seed.has_value())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    const contract::Json& target = params.at("windowId");
    if (!target.is_number())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    ++moves_;

    MoveTo request;
    request.source = self_id_;
    request.target = static_cast<WindowId>(target.as_int());
    request.seed = *seed;

    const WindowMoveResult result =
        move_to_ ? move_to_(request)
                 : WindowMoveResult{false, kInvalidWindowId, std::string{},
                                    "no move handler is bound in this build"};
    if (!result.ok && result.error.empty())
    {
        // A refused move with no reason reads as a shrug; name the unknown target so editor-core's
        // degrade path can say what happened.
        error_code = kErrWindowUnknownTarget;
        return contract::Json{};
    }

    contract::Json out = contract::Json::object();
    out.set("moved", contract::Json(result.ok));
    out.set("windowId", contract::Json(static_cast<std::uint64_t>(result.window_id)));
    out.set("error", contract::Json(result.error));
    return out;
}

contract::Json WindowBridge::seed()
{
    contract::Json out = contract::Json::object();
    const std::optional<PanelSeed> pending = store_.take_boot_seed(self_id_);
    out.set("seeded", contract::Json(pending.has_value()));
    if (pending.has_value())
    {
        ++seeds_served_;
        out.set("panelId", contract::Json(pending->panel_id));
        out.set("state", pending->state);
    }
    return out;
}

contract::Json WindowBridge::rehomed()
{
    contract::Json out = contract::Json::object();
    contract::Json panels = contract::Json::array();
    for (const PanelSeed& seed : store_.take_rehomed(self_id_))
    {
        panels.push_back(seed_json(seed));
    }
    out.set("panels", std::move(panels));
    return out;
}

contract::Json WindowBridge::close()
{
    const WindowMoveResult result =
        close_ ? close_(self_id_)
               : WindowMoveResult{false, self_id_, std::string{},
                                  "no close handler is bound in this build"};
    contract::Json out = contract::Json::object();
    out.set("closed", contract::Json(result.ok));
    out.set("outcome", contract::Json(result.outcome));
    out.set("error", contract::Json(result.error));
    return out;
}

contract::Json WindowBridge::drag_probe()
{
    contract::Json out = contract::Json::object();
    // No store bound (a smoke with no drag session) is the honest inactive answer, never a refusal.
    const DragHover hover =
        drag_store_ != nullptr ? drag_store_->hover_for(self_id_) : DragHover{};
    out.set("active", contract::Json(hover.active));
    if (hover.active)
    {
        ++drag_probes_active_;
        out.set("panelId", contract::Json(hover.panel_id));
        out.set("x", contract::Json(hover.local.x));
        out.set("y", contract::Json(hover.local.y));
        out.set("generation", contract::Json(static_cast<std::uint64_t>(hover.generation)));
    }
    return out;
}

contract::Json WindowBridge::drag_report_zone(const contract::Json& params, std::string& error_code)
{
    if (!params.is_object() || !params.contains("generation") ||
        !params.at("generation").is_number())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    DragZone zone;
    zone.generation = static_cast<std::uint64_t>(params.at("generation").as_int());
    zone.valid = params.at("valid").as_bool();
    const contract::Json& zone_id = params.at("zoneId");
    zone.zone_id = zone_id.is_string() ? zone_id.as_string() : std::string{};
    if (drag_store_ != nullptr)
    {
        // The store drops a stale-generation report itself; a null store (no drag session here) makes
        // the report a well-formed no-op rather than a refusal.
        drag_store_->report_zone(zone);
        ++drag_zones_reported_;
    }
    contract::Json out = contract::Json::object();
    out.set("recorded", contract::Json(true));
    return out;
}

contract::Json WindowBridge::ui_mirror(const contract::Json& params, std::string& error_code)
{
    // A well-formed `editor.ui` envelope carries at least a string `topic` and a string `origin`; the
    // Shell never interprets the payload (D7 tier 2 is opaque to it) but a publish with no topic/origin
    // is a wiring bug it fails CLOSED on rather than broadcasting a nameless fact.
    if (!params.is_object() || !params.at("topic").is_string() ||
        !params.at("origin").is_string())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }

    contract::Json out = contract::Json::object();
    if (mirror_store_ == nullptr)
    {
        // No store bound (a smoke with no mirror session): a well-formed no-op, never a refusal, so
        // the sibling smokes route the method without installing a session.
        out.set("mirrored", contract::Json(false));
        return out;
    }

    // THE BROADCAST (05 §5). Fan the envelope out to EVERY live window, the SENDER included: the
    // sender's own `ui.mirror-poll` will then hand it to `receiveMirrored`, which drops it by
    // `origin` — the branch a unicast relay would never light. `self_id_` is inserted explicitly so
    // the broadcast reaches this window even if a WindowsProvider chose to report peers only.
    std::set<WindowId> targets;
    targets.insert(self_id_);
    if (windows_)
    {
        for (const WindowId id : windows_())
        {
            targets.insert(id);
        }
    }
    for (const WindowId id : targets)
    {
        mirror_store_->enqueue(id, params);
    }
    ++ui_mirrors_published_;

    out.set("mirrored", contract::Json(true));
    out.set("windows", contract::Json(static_cast<std::uint64_t>(targets.size())));
    return out;
}

contract::Json WindowBridge::ui_mirror_poll()
{
    contract::Json out = contract::Json::object();
    contract::Json events = contract::Json::array();
    if (mirror_store_ != nullptr)
    {
        for (contract::Json& envelope : mirror_store_->take(self_id_))
        {
            events.push_back(std::move(envelope));
            ++ui_mirrors_delivered_;
        }
    }
    out.set("events", std::move(events));
    return out;
}

bool WindowBridge::install(BridgeRouter& router)
{
    bool ok = router.register_method(kWindowListMethod,
                                     [this](const BridgeRequest&) -> BridgeResult
                                     { return BridgeResult::ok(list()); });
    ok = router.register_method(
             kWindowTearOutMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = tear_out(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "tear-out request was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    ok = router.register_method(
             kWindowMoveToMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = move_to(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "move-to request was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    ok = router.register_method(kWindowSeedMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(seed()); }) &&
         ok;
    ok = router.register_method(kWindowRehomedMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(rehomed()); }) &&
         ok;
    ok = router.register_method(kWindowCloseMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(close()); }) &&
         ok;
    ok = router.register_method(kDragProbeMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(drag_probe()); }) &&
         ok;
    ok = router.register_method(
             kDragReportZoneMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = drag_report_zone(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "drag report-zone request was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    ok = router.register_method(
             kUiMirrorMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = ui_mirror(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "ui.mirror envelope was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    ok = router.register_method(kUiMirrorPollMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(ui_mirror_poll()); }) &&
         ok;
    return ok;
}

} // namespace context::editor::shell

// The window-management bridge surface for editor-core (M9 e10b, drag surface e10c) — see
// window_bridge.h for the model.

#include "context/editor/shell/window_bridge.h"

#include "context/editor/shell/cross_window_drag.h" // the drag relay served by drag.probe/report-zone
#include "context/editor/shell/menu_model.h" // the d3 menu publish's command-count observable
#include "json_number_read.h" // the shared range-guarded numeric read (float-cast-overflow UB guard)

#include <cstdint>
#include <optional>
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

void WindowBridge::bind_minimize(MinimizeHandler handler)
{
    minimize_ = std::move(handler);
}

void WindowBridge::bind_toggle_maximize(ToggleMaximizeHandler handler)
{
    toggle_maximize_ = std::move(handler);
}

void WindowBridge::bind_focus(FocusHandler handler)
{
    focus_ = std::move(handler);
}

void WindowBridge::bind_chrome_state(ChromeStateProvider provider)
{
    chrome_state_ = std::move(provider);
}

void WindowBridge::bind_appearance(AppearanceHandler handler)
{
    appearance_ = std::move(handler);
}

void WindowBridge::bind_menu(MenuPublishHandler handler)
{
    menu_ = std::move(handler);
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

const char* chrome_mode_token(ChromeMode mode)
{
    switch (mode)
    {
    case ChromeMode::system:
        return kChromeModeSystem;
    case ChromeMode::custom:
        return kChromeModeCustom;
    case ChromeMode::hybrid:
        return kChromeModeHybrid;
    }
    // Unreachable for the closed enum; the -Werror -Wswitch build catches a grown enum first
    // (region_kind_token's rationale).
    return "";
}

contract::Json WindowBridge::minimize()
{
    // Counted on the CALL, bound or not: the ten-smoke rule's claim is the ROUTING, and a smoke
    // with no window behind this bridge must still be able to assert the verb arrived.
    ++minimizes_;
    const bool accepted = minimize_ ? minimize_() : false;
    contract::Json out = contract::Json::object();
    out.set("accepted", contract::Json(accepted));
    return out;
}

contract::Json WindowBridge::toggle_maximize()
{
    ++maximize_toggles_;
    const std::optional<bool> next = toggle_maximize_ ? toggle_maximize_() : std::nullopt;
    contract::Json out = contract::Json::object();
    out.set("accepted", contract::Json(next.has_value()));
    // Unconditional (write_notice_envelope's rationale: "missing" and "meaningless" never differ
    // here); the value is meaningful only when accepted, which the TS parser documents.
    out.set("maximized", contract::Json(next.value_or(false)));
    return out;
}

contract::Json WindowBridge::focus(const contract::Json& params, std::string& error_code)
{
    // d3: the OPTIONAL target (kWindowFocusMethod). Absent = this window (the a1 behaviour, and
    // what every existing caller sends); present must be a number — a present-but-unreadable id is
    // a wiring bug refused loudly, never silently retargeted at self. The range guard runs on the
    // DOUBLE before any integral cast (json_number_read.h), the ui_mirror_report discipline.
    WindowId target = self_id_;
    if (params.is_object() && params.contains("windowId"))
    {
        const std::optional<double> id =
            detail::number_in_range(params, "windowId", 0.0, 4294967295.0);
        // A fractional id (2.5) must not silently TRUNCATE onto a window nobody named — refused
        // exactly like a non-numeric one. The comparison is exact and defined: the guard above
        // proved the double in-range, and an integral in-range double round-trips the cast
        // losslessly.
        if (!id.has_value() || *id != static_cast<double>(static_cast<WindowId>(*id)))
        {
            error_code = kErrWindowBadParams;
            return contract::Json{};
        }
        target = static_cast<WindowId>(*id);
    }
    ++focus_requests_;
    const bool accepted = focus_ ? focus_(target) : false;
    contract::Json out = contract::Json::object();
    out.set("accepted", contract::Json(accepted));
    return out;
}

contract::Json WindowBridge::menu_publish(const contract::Json& params, std::string& error_code)
{
    // The OUTER fail-closed shape (window_bridge.h § menu_publish): editor-core only ever publishes
    // `{menus: [...]}`, so anything else is a wiring bug surfacing loudly — the same posture
    // `set_appearance` takes on its token.
    if (!params.is_object() || !params.at("menus").is_array())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    // Counted on every well-formed publish, bound or not — the ten-smoke routing discipline — and
    // the command tally beside it, so the live boot smoke can assert the model was non-trivial.
    ++menu_publishes_;
    const std::optional<MenuModel> model = parse_menu_model(params);
    last_menu_commands_ = model.has_value() ? model->command_count() : 0;
    const bool accepted = menu_ ? menu_(params) : false;
    contract::Json out = contract::Json::object();
    out.set("accepted", contract::Json(accepted));
    return out;
}

contract::Json WindowBridge::chrome_state()
{
    ++chrome_reads_;
    // The defaulted ChromeState IS the honest unbound answer (window_bridge.h § the chrome state):
    // `system` chrome, zero inset, not maximized, not focused — what every backend truthfully
    // reports in a1 anyway (interim honesty).
    const ChromeState state = chrome_state_ ? chrome_state_() : ChromeState{};
    contract::Json inset = contract::Json::object();
    inset.set("left", contract::Json(static_cast<std::uint64_t>(state.controls_inset_left)));
    inset.set("right", contract::Json(static_cast<std::uint64_t>(state.controls_inset_right)));
    contract::Json out = contract::Json::object();
    out.set("mode", contract::Json(chrome_mode_token(state.mode)));
    out.set("controlsInset", std::move(inset));
    out.set("maximized", contract::Json(state.maximized));
    out.set("focused", contract::Json(state.focused));
    // Derived from this bridge's own identity, never from the provider: the provider describes the
    // WINDOW's chrome, but which strip set to render (02 §9) is a fact about the window's ROLE, and
    // `self_id_` is the same identity `window.list` already reports.
    out.set("window", contract::Json(self_id_ == kPrimaryWindowId ? kChromeWindowPrimary
                                                                  : kChromeWindowSecondary));
    return out;
}

contract::Json WindowBridge::set_appearance(const contract::Json& params, std::string& error_code)
{
    // Fail CLOSED on a token that is neither pinned value (window_bridge.h § set_appearance): a
    // drifted token silently defaulted would tint the frame wrong with both builds green.
    if (!params.is_object() || !params.contains("appearance") ||
        !params.at("appearance").is_string())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    const std::string& token = params.at("appearance").as_string();
    if (token != kWindowAppearanceDark && token != kWindowAppearanceLight)
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }
    const bool dark = token == kWindowAppearanceDark;
    // Counted on every well-formed report, bound or not — the same ten-smoke routing discipline
    // the control verbs follow.
    ++appearance_reports_;
    last_appearance_dark_ = dark;
    const bool accepted = appearance_ ? appearance_(dark) : false;
    contract::Json out = contract::Json::object();
    out.set("accepted", contract::Json(accepted));
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

contract::Json WindowBridge::ui_mirror_report(const contract::Json& params, std::string& error_code)
{
    // A convergence report carries two non-negative running totals: how many mirrored facts the
    // receiving bus APPLIED and how many own-origin echoes it DROPPED. A report missing either, or
    // carrying a negative / non-numeric one, is a wiring bug it fails CLOSED on rather than recording
    // a meaningless count the smoke would then assert against.
    // Both counts are untrusted renderer-wire numbers, routed through the shared range-guarded read
    // (json_number_read.h): the [0, u32-max] check runs on the DOUBLE before any integral cast, so an
    // out-of-int64 double fails CLOSED here rather than triggering the `float-cast-overflow` UB the
    // blocking `sanitize (ASan+UBSan)` leg reports. Absent / non-number / NaN / negative all read the
    // same "no usable number" way — the wiring bug the smoke must never assert a meaningless count off.
    const std::optional<double> applied = detail::number_in_range(params, "applied", 0.0, 4294967295.0);
    const std::optional<double> suppressed =
        detail::number_in_range(params, "suppressed", 0.0, 4294967295.0);
    if (!applied.has_value() || !suppressed.has_value())
    {
        error_code = kErrWindowBadParams;
        return contract::Json{};
    }

    // Last-write-wins: the renderer sends CUMULATIVE totals (they only grow), so the latest report
    // holds the current convergence — the smoke waits for `applied` / `suppressed` to reach the value
    // that proves the drill and they never regress.
    ui_mirror_reported_applied_ = static_cast<std::size_t>(*applied);
    ui_mirror_reported_suppressed_ = static_cast<std::size_t>(*suppressed);
    ++ui_mirror_reports_;

    contract::Json out = contract::Json::object();
    out.set("recorded", contract::Json(true));
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
    // a1 — the window-control verbs + the chrome read. None takes params and none can refuse
    // (unbound handlers degrade to `accepted:false` / the defaulted chrome state), so all four are
    // plain result bindings like `window.close` above.
    ok = router.register_method(kWindowMinimizeMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(minimize()); }) &&
         ok;
    ok = router.register_method(kWindowToggleMaximizeMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(toggle_maximize()); }) &&
         ok;
    // d3: `window.focus` is params-taking now (the OPTIONAL windowId — see kWindowFocusMethod),
    // and so refusable on a malformed one, like tear-out/move-to.
    ok = router.register_method(
             kWindowFocusMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = focus(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "focus request was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    ok = router.register_method(kChromeStateMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(chrome_state()); }) &&
         ok;
    // d3: the menu publish — params-taking (and so refusable on a malformed model), but never
    // refused for being unbound (the non-macOS builds' honest `accepted:false`).
    ok = router.register_method(
             kMenuPublishMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = menu_publish(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "menu publish was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    // b1 — the appearance report: params-taking (and so refusable on malformed params, like
    // tear-out/move-to), but never refused for being unbound.
    ok = router.register_method(
             kWindowSetAppearanceMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = set_appearance(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "appearance report was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
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
    ok = router.register_method(
             kUiMirrorReportMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 contract::Json value = ui_mirror_report(request.params, error_code);
                 if (!error_code.empty())
                 {
                     return BridgeResult::error(error_code, "ui.mirror-report was malformed");
                 }
                 return BridgeResult::ok(std::move(value));
             }) &&
         ok;
    return ok;
}

} // namespace context::editor::shell

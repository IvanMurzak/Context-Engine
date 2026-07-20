// The Shell-side panel host implementation (see panel_host.h for the design and the D10 rationale).
//
// EVERY HANDLER IS TOTAL OVER RENDERER-CONTROLLED INPUT. The params reaching these methods came off
// the privileged bridge, which means they came from a renderer process — untrusted by the same
// reasoning ipc_bridge.h spells out. A missing member, a number where a string belongs, an id that
// is 4 MiB of nul bytes: each is a REFUSAL with a grep-stable code, never a throw and never a
// dereference. The bridge contains a throwing handler, but relying on that would make every one of
// these paths a `handler_threw` with no classification the caller can branch on.

#include "context/editor/shell/panel_host.h"

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/panel_state.h"

#include <utility>

namespace context::editor::shell
{

namespace
{

namespace gc = gui::contract;
namespace ut = gui::uitree;

// Read a required string member off a params object. False (leaving `out` untouched) when the member
// is absent or not a string — the caller answers kErrPanelBadParams.
[[nodiscard]] bool read_string(const contract::Json& params, const std::string& key,
                               std::string& out)
{
    if (!params.is_object() || !params.contains(key))
    {
        return false;
    }
    const contract::Json& value = params.at(key);
    if (!value.is_string())
    {
        return false;
    }
    out = value.as_string();
    return true;
}

// The manifest-v2 projection of one roster entry (04 §3). `hosted`, `gestures` and `state` are HOST
// facts rather than manifest ones: they say what THIS build can actually do with the panel, which is
// what lets the editor list its whole roster honestly while e05d3 is still in flight.
[[nodiscard]] contract::Json project_dock(const gc::DockDefaults& dock)
{
    contract::Json out = contract::Json::object();
    out.set("zone", contract::Json(gc::dock_zone_token(dock.default_zone)));
    out.set("singleton", contract::Json(dock.singleton));
    out.set("minWidth", contract::Json(dock.min_width));
    out.set("minHeight", contract::Json(dock.min_height));
    return out;
}

[[nodiscard]] contract::Json project_capabilities(const std::vector<std::string>& capabilities)
{
    contract::Json out = contract::Json::array();
    for (const std::string& capability : capabilities)
    {
        out.push_back(contract::Json(capability));
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------------------- gesture verbs

const char* gesture_verb_token(GestureVerb verb)
{
    switch (verb)
    {
    case GestureVerb::begin:
        return "begin";
    case GestureVerb::extend:
        return "extend";
    case GestureVerb::commit:
        return "commit";
    case GestureVerb::cancel:
        return "cancel";
    }
    return "cancel";
}

std::optional<GestureVerb> parse_gesture_verb(std::string_view token)
{
    if (token == "begin")
    {
        return GestureVerb::begin;
    }
    if (token == "extend")
    {
        return GestureVerb::extend;
    }
    if (token == "commit")
    {
        return GestureVerb::commit;
    }
    if (token == "cancel")
    {
        return GestureVerb::cancel;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------ PanelHost

PanelHost::PanelHost() : PanelHost(gc::builtin_contributions()) {}

PanelHost::PanelHost(std::vector<gc::Contribution> roster)
{
    roster_.reserve(roster.size());
    for (gc::Contribution& contribution : roster)
    {
        Entry entry;
        entry.manifest = std::move(contribution);
        roster_.push_back(std::move(entry));
    }
}

PanelHost::Entry* PanelHost::find(const std::string& panel_id)
{
    for (Entry& entry : roster_)
    {
        if (entry.manifest.id == panel_id)
        {
            return &entry;
        }
    }
    return nullptr;
}

const PanelHost::Entry* PanelHost::find(const std::string& panel_id) const
{
    for (const Entry& entry : roster_)
    {
        if (entry.manifest.id == panel_id)
        {
            return &entry;
        }
    }
    return nullptr;
}

PanelHost::Entry* PanelHost::resolve_hosted(const std::string& panel_id, std::string& error_code)
{
    // const_cast over the const overload rather than a second copy of the same three branches: the
    // logic is identical and duplicating it is how the two drift apart on the next edit.
    return const_cast<Entry*>(
        static_cast<const PanelHost*>(this)->resolve_hosted(panel_id, error_code));
}

const PanelHost::Entry* PanelHost::resolve_hosted(const std::string& panel_id,
                                                  std::string& error_code) const
{
    const Entry* entry = find(panel_id);
    if (entry == nullptr)
    {
        error_code = kErrPanelUnknown;
        return nullptr;
    }
    if (!entry->hosted)
    {
        // NOT an "unknown panel": the distinction is the whole point of listing unhosted panels. A
        // renderer that asks for the Inspector today gets "this build cannot render it", which is
        // true and actionable, rather than "no such panel", which is false.
        error_code = kErrPanelNotHosted;
        return nullptr;
    }
    return entry;
}

bool PanelHost::provide(const std::string& panel_id, PanelProvider provider)
{
    if (provider.build == nullptr)
    {
        return false;
    }
    Entry* entry = find(panel_id);
    if (entry == nullptr || entry->hosted)
    {
        return false;
    }
    entry->provider = std::move(provider);
    entry->hosted = true;
    return true;
}

bool PanelHost::knows(const std::string& panel_id) const { return find(panel_id) != nullptr; }

bool PanelHost::hosts(const std::string& panel_id) const
{
    const Entry* entry = find(panel_id);
    return entry != nullptr && entry->hosted;
}

std::size_t PanelHost::hosted_count() const
{
    std::size_t count = 0;
    for (const Entry& entry : roster_)
    {
        if (entry.hosted)
        {
            ++count;
        }
    }
    return count;
}

void PanelHost::touch(const std::string& panel_id)
{
    Entry* entry = find(panel_id);
    if (entry != nullptr)
    {
        ++entry->revision;
    }
}

std::uint64_t PanelHost::revision(const std::string& panel_id) const
{
    const Entry* entry = find(panel_id);
    return entry == nullptr ? 0u : entry->revision;
}

contract::Json PanelHost::list() const
{
    contract::Json panels = contract::Json::array();
    for (const Entry& entry : roster_)
    {
        const gc::Contribution& m = entry.manifest;
        contract::Json panel = contract::Json::object();
        panel.set("id", contract::Json(m.id));
        panel.set("kind", contract::Json(gc::contribution_kind_token(m.kind)));
        panel.set("title", contract::Json(m.title));
        panel.set("icon", contract::Json(m.icon));
        panel.set("contractVersion", contract::Json(static_cast<std::uint64_t>(m.contract_version)));
        panel.set("dock", project_dock(m.dock));

        contract::Json content = contract::Json::object();
        content.set("type", contract::Json(gc::content_type_token(m.content.type)));
        content.set("entry", contract::Json(m.content.entry));
        panel.set("content", content);

        contract::Json state = contract::Json::object();
        state.set(gc::kStateSchemaVersionKey,
                  contract::Json(static_cast<std::uint64_t>(m.state.schema_version)));
        panel.set("state", state);

        panel.set("capabilities", project_capabilities(m.capabilities));

        // The HOST facts. `hosted` gates everything else the runtime may attempt; `gestures` and
        // `persists` tell it which optional verbs exist, so it never sends one that can only be
        // refused. An unhosted panel reports false for both — it has no provider to ask.
        panel.set("hosted", contract::Json(entry.hosted));
        panel.set("gestures", contract::Json(entry.hosted && entry.provider.gesture != nullptr));
        panel.set("persists", contract::Json(entry.hosted && entry.provider.get_state != nullptr &&
                                             entry.provider.restore_state != nullptr));
        panel.set("revision", contract::Json(entry.revision));
        panels.push_back(std::move(panel));
    }

    contract::Json out = contract::Json::object();
    out.set("contractMajor", contract::Json(static_cast<std::uint64_t>(gc::kContractMajor)));
    out.set("panels", std::move(panels));
    return out;
}

std::optional<PanelRender> PanelHost::render(const std::string& panel_id,
                                             std::string& error_code) const
{
    const Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return std::nullopt;
    }

    const ut::Panel panel = entry->provider.build();

    PanelRender out;
    out.panel_id = panel_id;
    out.revision = entry->revision;
    // A panel with no root renders as an EMPTY body rather than as a failure: `has_root()` false is
    // a legitimate "nothing to show yet" state (a model built before its first data arrived), and
    // uitree::render_html on a default-constructed root would emit a bogus id-less <section>.
    out.html = panel.has_root() ? ut::render_html(panel) : std::string();
    out.focus_order = panel.has_root() ? ut::focus_order(panel) : std::vector<std::string>();
    out.commands = panel.commands();
    return out;
}

bool PanelHost::invoke(const std::string& panel_id, const std::string& command_id,
                       const contract::Json& params, bool& dispatched, std::string& error_code)
{
    dispatched = false;
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    if (entry->provider.invoke == nullptr)
    {
        // A panel that renders but binds no invoker. Reported as "no such command" rather than as a
        // provider defect, because from the caller's side that is exactly what it is: nothing this
        // panel exposes can be invoked.
        error_code = kErrPanelUnknownCommand;
        return false;
    }
    // REACHABILITY CHECK, at the seam. The panel model is the authority on which commands exist
    // (uitree::audit_a11y already refuses a node bound to a command the panel does not expose), so a
    // `data-command` that no longer resolves means the renderer is acting on a STALE mounted tree.
    // Forwarding it would hand the model a command it never declared.
    const ut::Panel panel = entry->provider.build();
    if (!panel.has_command(command_id))
    {
        error_code = kErrPanelUnknownCommand;
        return false;
    }

    dispatched = entry->provider.invoke(command_id, params);
    ++commands_dispatched_;
    if (dispatched)
    {
        // Only a command the panel ACTED on advances the revision. Bumping on a declined command
        // would make every dead click look like a model change and force a pointless re-render.
        ++entry->revision;
    }
    return true;
}

bool PanelHost::gesture(const std::string& panel_id, GestureVerb verb, const contract::Json& params,
                        bool& dispatched, std::string& error_code)
{
    dispatched = false;
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    if (entry->provider.gesture == nullptr)
    {
        error_code = kErrPanelBadGesture;
        return false;
    }
    dispatched = entry->provider.gesture(verb, params);
    if (dispatched)
    {
        ++entry->revision;
    }
    return true;
}

std::optional<contract::Json> PanelHost::get_state(const std::string& panel_id,
                                                   std::string& error_code) const
{
    const Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    if (entry->provider.get_state == nullptr)
    {
        error_code = kErrPanelNoState;
        return std::nullopt;
    }
    // The blob shape is `{schemaVersion, data}` and the VERSION comes from the manifest, not from
    // the provider: the roster is where a panel declares the version it writes today (04 §3), so a
    // provider cannot stamp a version the manifest disagrees with.
    gc::PanelState state;
    state.schema_version = entry->manifest.state.schema_version;
    state.data = entry->provider.get_state();
    return gc::persist_panel_state(state);
}

bool PanelHost::restore_state(const std::string& panel_id, const contract::Json& persisted,
                              bool& restored, std::string& code, std::string& diagnostic,
                              std::string& error_code)
{
    restored = false;
    code.clear();
    diagnostic.clear();

    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    if (entry->provider.restore_state == nullptr)
    {
        error_code = kErrPanelNoState;
        return false;
    }

    // THE D6 DEGRADE PATH. A blob whose schemaVersion does not match what the panel declares today
    // is NOT migrated and NOT partially applied: the panel keeps its defaults and the caller gets a
    // diagnostic. That is a successful CALL with `restored: false` — never an error — which is what
    // lets e05d2 restore the rest of a layout when one panel's blob is stale.
    const gc::StateRestore outcome =
        gc::restore_panel_state(entry->manifest.state.schema_version, persisted);
    if (!outcome.ok)
    {
        code = outcome.code;
        diagnostic = outcome.diagnostic;
        return true;
    }

    restored = entry->provider.restore_state(outcome.state->data);
    if (!restored)
    {
        // The blob was well-formed and correctly versioned, but the panel refused its CONTENT. Same
        // degrade shape, different cause — reported with the malformed code so a caller sees one
        // classification for "you get default state" rather than having to special-case a third.
        code = gc::kErrStateMalformed;
        diagnostic = "the panel refused the persisted payload; it restored its defaults";
        return true;
    }
    ++entry->revision;
    return true;
}

// -------------------------------------------------------------------------------- bridge binding

bool PanelHost::install(BridgeRouter& router)
{
    bool ok = true;

    ok = router.register_method(kPanelListMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                {
                                    ++lists_served_;
                                    return BridgeResult::ok(list());
                                }) &&
         ok;

    ok = router.register_method(
             kPanelRenderMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 if (!read_string(request.params, "panelId", panel_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.render requires a string 'panelId'");
                 }
                 std::string error_code;
                 const std::optional<PanelRender> rendered = render(panel_id, error_code);
                 if (!rendered.has_value())
                 {
                     return BridgeResult::error(error_code, "panel '" + panel_id +
                                                                "' cannot be rendered by this build");
                 }
                 ++renders_served_;

                 contract::Json focus = contract::Json::array();
                 for (const std::string& node_id : rendered->focus_order)
                 {
                     focus.push_back(contract::Json(node_id));
                 }
                 contract::Json commands = contract::Json::array();
                 for (const ut::Command& command : rendered->commands)
                 {
                     contract::Json entry = contract::Json::object();
                     entry.set("id", contract::Json(command.id));
                     entry.set("title", contract::Json(command.title));
                     commands.push_back(std::move(entry));
                 }

                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(rendered->panel_id));
                 out.set("revision", contract::Json(rendered->revision));
                 out.set("html", contract::Json(rendered->html));
                 out.set("focusOrder", std::move(focus));
                 out.set("commands", std::move(commands));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelCommandMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 std::string command_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_string(request.params, "commandId", command_id))
                 {
                     return BridgeResult::error(
                         kErrPanelBadParams,
                         "panel.command requires string 'panelId' and 'commandId'");
                 }
                 bool dispatched = false;
                 std::string error_code;
                 if (!invoke(panel_id, command_id, request.params, dispatched, error_code))
                 {
                     return BridgeResult::error(error_code, "panel '" + panel_id +
                                                                "' cannot dispatch '" + command_id +
                                                                "'");
                 }
                 contract::Json out = contract::Json::object();
                 out.set("dispatched", contract::Json(dispatched));
                 out.set("revision", contract::Json(revision(panel_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelGestureMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 std::string verb_token;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_string(request.params, "verb", verb_token))
                 {
                     return BridgeResult::error(
                         kErrPanelBadParams, "panel.gesture requires string 'panelId' and 'verb'");
                 }
                 const std::optional<GestureVerb> verb = parse_gesture_verb(verb_token);
                 if (!verb.has_value())
                 {
                     return BridgeResult::error(kErrPanelBadGesture,
                                                "'" + verb_token +
                                                    "' is not one of begin/extend/commit/cancel");
                 }
                 bool dispatched = false;
                 std::string error_code;
                 if (!gesture(panel_id, *verb, request.params, dispatched, error_code))
                 {
                     return BridgeResult::error(error_code, "panel '" + panel_id +
                                                                "' does not accept gestures");
                 }
                 contract::Json out = contract::Json::object();
                 out.set("dispatched", contract::Json(dispatched));
                 out.set("revision", contract::Json(revision(panel_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelStateGetMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 if (!read_string(request.params, "panelId", panel_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.state.get requires a string 'panelId'");
                 }
                 std::string error_code;
                 const std::optional<contract::Json> state = get_state(panel_id, error_code);
                 if (!state.has_value())
                 {
                     return BridgeResult::error(error_code,
                                                "panel '" + panel_id + "' persists no state");
                 }
                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(panel_id));
                 out.set("state", *state);
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelStateSetMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 if (!read_string(request.params, "panelId", panel_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.state.set requires a string 'panelId'");
                 }
                 // A MISSING `state` member is the explicit "restore from nothing" call, not a
                 // malformed one: it lands on restore_panel_state's malformed branch and degrades to
                 // defaults + a diagnostic, exactly like a stale blob. Refusing it here would make
                 // the caller special-case a case D6 already answers.
                 const contract::Json& persisted = request.params.at("state");
                 bool restored = false;
                 std::string code;
                 std::string diagnostic;
                 std::string error_code;
                 if (!restore_state(panel_id, persisted, restored, code, diagnostic, error_code))
                 {
                     return BridgeResult::error(error_code,
                                                "panel '" + panel_id + "' cannot restore state");
                 }
                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(panel_id));
                 out.set("restored", contract::Json(restored));
                 out.set("code", contract::Json(code));
                 out.set("diagnostic", contract::Json(diagnostic));
                 out.set("revision", contract::Json(revision(panel_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    return ok;
}

} // namespace context::editor::shell

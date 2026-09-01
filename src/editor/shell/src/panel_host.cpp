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

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

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

// Read an OPTIONAL string member. `true` (leaving `out` empty) when the member is absent; `false`
// when it is present but not a string, which the caller answers kErrPanelBadParams.
//
// ⚠ ABSENT AND WRONG-TYPED ARE DIFFERENT ANSWERS, and collapsing them is the whole reason this is a
// separate helper rather than "read_string, ignore the failure". `instanceId` is optional on every
// panel method (c3), so its absence is the ordinary single-instance call; a NUMBER there, though, is
// a caller that thinks it is addressing an instance and is not -- silently defaulting it to the
// first live copy would route one instance's command into another's model.
[[nodiscard]] bool read_optional_string(const contract::Json& params, const std::string& key,
                                        std::string& out)
{
    if (!params.is_object() || !params.contains(key))
    {
        return true;
    }
    // PRESENT, so the only remaining question is "is it a string" -- which is exactly what
    // `read_string` answers once the member is known to exist. Delegating keeps ONE definition of
    // "a string member": a tolerance added there (reading a `null` as absent, say) can never leave
    // the required and optional readers disagreeing about the same wire payload.
    return read_string(params, key, out);
}

// The manifest-v3 projection of one roster entry (04 §3 + 04 §2). `hosted`, `gestures` and `state`
// are HOST facts rather than manifest ones: they say what THIS build can actually do with the panel,
// which is what lets the editor list its whole roster honestly while e05d3 is still in flight.
[[nodiscard]] contract::Json project_dock(const gc::DockDefaults& dock)
{
    contract::Json out = contract::Json::object();
    out.set("zone", contract::Json(gc::dock_zone_token(dock.default_zone)));
    // ⚠ `singleton` LIVED HERE UNTIL MANIFEST v3 and is gone — it is now `instances.mode`, a sibling
    // of `dock` rather than a member of it, because "how many copies may exist" was never a DOCKING
    // fact. The wire moved with the C++ struct in the same change, and `panels.ts`'s `readDock` moved
    // with both: a projection that kept emitting a member the struct no longer holds could only ever
    // synthesise it, which is how a manifest starts lying about what it declared.
    out.set("minWidth", contract::Json(dock.min_width));
    out.set("minHeight", contract::Json(dock.min_height));
    return out;
}

// The manifest-v3 `instances` block (04 §2). `max` is emitted unconditionally — 0 on the two modes
// that may not state one — so the reader never has to distinguish "absent" from "zero".
[[nodiscard]] contract::Json project_instances(const gc::InstanceSpec& instances)
{
    contract::Json out = contract::Json::object();
    out.set("mode", contract::Json(gc::instance_mode_token(instances.mode)));
    out.set("max", contract::Json(instances.max));
    return out;
}

[[nodiscard]] contract::Json project_string_array(const std::vector<std::string>& values)
{
    contract::Json out = contract::Json::array();
    for (const std::string& value : values)
    {
        out.push_back(contract::Json(value));
    }
    return out;
}

// The manifest-declared commands (04 §3 `commands`), projected for the editor-core command registry
// (M9 e07b, its source (c)). Built-in uitree panels declare their commands on the C++ Panel model
// instead, so their manifest `commands` are empty and this yields `[]`; iframe contributions — which
// have no C++ model to read commands from — carry theirs here. `when` is the optional context clause
// (empty = always), emitted verbatim so the JS `when`-evaluator sees exactly what the manifest states.
[[nodiscard]] contract::Json project_commands(const std::vector<gc::CommandContribution>& commands)
{
    contract::Json out = contract::Json::array();
    for (const gc::CommandContribution& command : commands)
    {
        contract::Json entry = contract::Json::object();
        entry.set("id", contract::Json(command.id));
        entry.set("title", contract::Json(command.title));
        entry.set("when", contract::Json(command.when));
        out.push_back(std::move(entry));
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

// ------------------------------------------------------------------------- instance identity (c3)

std::string make_panel_instance_id(const std::string& panel_id, std::uint64_t ordinal)
{
    return panel_id + kPanelInstanceSeparator + std::to_string(ordinal);
}

std::string panel_id_of_instance(std::string_view instance_id)
{
    const std::size_t at = instance_id.rfind(kPanelInstanceSeparator);
    // No separator => the whole string IS the kind. That is the honest reading of an id minted
    // before instances existed (a persisted v1 arrangement names bare panel ids), and it is what
    // makes such a layout restore onto the kind rather than onto nothing.
    return at == std::string_view::npos ? std::string(instance_id)
                                        : std::string(instance_id.substr(0, at));
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

bool PanelHost::bind(const std::string& panel_id, PanelProvider provider,
                     PanelProviderFactory factory)
{
    Entry* entry = find(panel_id);
    if (entry == nullptr || entry->hosted)
    {
        return false;
    }
    // The capability shape is read from whichever binding this is -- the shared provider, or the
    // factory's PROBE, which the caller has already taken (see provide_factory).
    entry->offers_gestures = provider.gesture != nullptr;
    entry->offers_state = provider.get_state != nullptr && provider.restore_state != nullptr;
    entry->provider = std::move(provider);
    entry->factory = std::move(factory);
    entry->hosted = true;
    return true;
}

bool PanelHost::provide(const std::string& panel_id, PanelProvider provider)
{
    if (provider.build == nullptr)
    {
        return false;
    }
    return bind(panel_id, std::move(provider), nullptr);
}

bool PanelHost::provide_factory(const std::string& panel_id, PanelProviderFactory factory)
{
    if (factory == nullptr)
    {
        return false;
    }
    // THE PROBE (panel_host.h states why it is taken here rather than deferred). Its provider is
    // used for the capability read inside `bind`, which then RETAINS it as `Entry::provider` for the
    // entry's lifetime -- so a throwaway model it may have built is kept alive too. No instance ever
    // renders from it (`create_instance` calls the factory whenever one is bound), but do not read
    // this as "the probe is discarded": anything added that reads `Entry::provider` without checking
    // `factory` first would silently serve every copy of this kind the probe's model.
    PanelProvider probe = factory(std::string());
    if (probe.build == nullptr)
    {
        return false;
    }
    return bind(panel_id, std::move(probe), std::move(factory));
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

// --------------------------------------------------------------------------- the instance table

namespace
{

// Keep a kind's mint counter AHEAD of an id that arrived from OUTSIDE (the renderer minted it, or a
// persisted arrangement named it). Both sides mint by the same `<panelId>#<n>` rule, so without this
// the host's next mint could collide with a live copy -- which would hand a NEW instance the model of
// an existing one, silently, and make two Dockview slots share one state blob. A non-numeric or
// unparseable tail is simply ignored: an id the host did not mint is still a perfectly good KEY, it
// just says nothing about the counter.
void advance_ordinal_past(const std::string& panel_id, const std::string& instance_id,
                          std::uint64_t& next_ordinal)
{
    const std::string prefix = panel_id + kPanelInstanceSeparator;
    if (instance_id.size() <= prefix.size() || instance_id.compare(0, prefix.size(), prefix) != 0)
    {
        return;
    }
    const std::string tail = instance_id.substr(prefix.size());
    if (tail.empty() || tail.find_first_not_of("0123456789") != std::string::npos)
    {
        return;
    }
    std::uint64_t parsed = 0;
    for (const char digit : tail)
    {
        // Hand-folded rather than strtoull: a 128-character run of digits overflows, and a saturating
        // fold keeps the counter monotonic without depending on errno. Saturation is harmless -- the
        // resource ceiling refuses such a kind long before the counter matters.
        if (parsed > (UINT64_MAX - 9) / 10)
        {
            parsed = UINT64_MAX - 1;
            break;
        }
        parsed = parsed * 10 + static_cast<std::uint64_t>(digit - '0');
    }
    if (parsed + 1 > next_ordinal)
    {
        next_ordinal = parsed + 1;
    }
}

} // namespace

PanelHost::Instance* PanelHost::find_instance(Entry& entry, const std::string& instance_id)
{
    for (Instance& instance : entry.instances)
    {
        if (instance.id == instance_id)
        {
            return &instance;
        }
    }
    return nullptr;
}

bool PanelHost::may_open(const Entry& entry, std::string& diagnostic) const
{
    const std::size_t live = entry.instances.size();
    // THE RESOURCE CEILING FIRST, and above every mode including `unlimited`: it exists to bound an
    // allocation a renderer drives, so a mode that opted out of the declarative limit must not also
    // opt out of this one.
    if (live >= kMaxPanelInstances)
    {
        diagnostic = "panel '" + entry.manifest.id + "' already holds " + std::to_string(live) +
                     " live instances, the host ceiling of " + std::to_string(kMaxPanelInstances);
        return false;
    }
    switch (entry.manifest.instances.mode)
    {
    case gc::InstanceMode::singleton:
        if (live >= 1)
        {
            diagnostic = "panel '" + entry.manifest.id +
                         "' declares instances.mode \"singleton\": at most one live copy";
            return false;
        }
        return true;
    case gc::InstanceMode::limited:
    {
        // A ceiling below one is not a ceiling (extension.h): the registry refuses such a manifest,
        // so reaching this with `max <= 0` means a roster built in code rather than parsed. Refusing
        // is the honest answer -- inventing a floor of 1 would host a panel its own manifest says
        // may not exist.
        const std::size_t max = entry.manifest.instances.max > 0
                                    ? static_cast<std::size_t>(entry.manifest.instances.max)
                                    : 0u;
        if (live >= max)
        {
            diagnostic = "panel '" + entry.manifest.id +
                         "' declares instances.mode \"limited\" with max " + std::to_string(max) +
                         "; " + std::to_string(live) + " are already open";
            return false;
        }
        return true;
    }
    case gc::InstanceMode::unlimited:
        return true;
    }
    // Unreachable for a well-formed enumerator; deny-by-default, matching instance_mode_token's own
    // fallback -- an unknown mode must read as the MOST restrictive answer, never as `unlimited`.
    diagnostic = "panel '" + entry.manifest.id + "' declares an instance mode this build cannot read";
    return false;
}

std::string PanelHost::refusal_message(const std::string& panel_id, const std::string& error_code,
                                       std::string fallback) const
{
    if (error_code != kErrPanelInstanceLimit)
    {
        return fallback;
    }
    const Entry* entry = find(panel_id);
    if (entry == nullptr)
    {
        return fallback;
    }
    std::string diagnostic;
    if (may_open(*entry, diagnostic) || diagnostic.empty())
    {
        // Defensive only: the code says a ceiling refused, so the predicate must still refuse.
        return fallback;
    }
    return diagnostic;
}

PanelHost::Instance* PanelHost::create_instance(Entry& entry, const std::string& instance_id,
                                                std::string& code, std::string& diagnostic)
{
    if (instance_id.empty() || instance_id.size() > kMaxPanelInstanceIdLength)
    {
        code = kErrPanelBadParams;
        diagnostic = "an instance id must be 1.." + std::to_string(kMaxPanelInstanceIdLength) +
                     " characters";
        return nullptr;
    }
    if (!may_open(entry, diagnostic))
    {
        code = kErrPanelInstanceLimit;
        return nullptr;
    }
    Instance instance;
    instance.id = instance_id;
    // A FACTORY IS CALLED ONCE PER INSTANCE; a shared-model binding is COPIED, which copies the
    // std::functions and therefore keeps pointing at the one model (panel_host.h states the split).
    instance.provider = entry.factory != nullptr ? entry.factory(instance_id) : entry.provider;
    if (instance.provider.build == nullptr)
    {
        // A factory that produced a provider which cannot render. Reported as `not_hosted` rather
        // than as a limit: from the caller's side this build genuinely cannot draw the panel, and
        // that is the code every other unrenderable path already answers.
        code = kErrPanelNotHosted;
        diagnostic = "panel '" + entry.manifest.id +
                     "' bound a factory that produced no renderable provider";
        return nullptr;
    }
    advance_ordinal_past(entry.manifest.id, instance_id, entry.next_ordinal);
    entry.instances.push_back(std::move(instance));
    return &entry.instances.back();
}

PanelHost::Instance* PanelHost::resolve_instance(Entry& entry, const std::string& instance_id,
                                                 std::string& error_code)
{
    if (instance_id.empty())
    {
        // THE DEFAULT INSTANCE -- what every pre-c3 caller means. The FIRST live copy, or a freshly
        // materialised one when the kind has none yet, which is what makes a single-instance panel
        // behave exactly as it did before instances existed.
        if (!entry.instances.empty())
        {
            return &entry.instances.front();
        }
        std::string diagnostic;
        return create_instance(entry, make_panel_instance_id(entry.manifest.id, entry.next_ordinal),
                               error_code, diagnostic);
    }
    if (Instance* existing = find_instance(entry, instance_id); existing != nullptr)
    {
        return existing;
    }
    // MATERIALISE, do not refuse. The renderer owns panel lifecycle: it minted this id and docked a
    // slot for it, so the first call naming it is when the model is needed (panel_host.h states the
    // rule and why). The ceiling inside `create_instance` is what keeps that honest.
    std::string diagnostic;
    return create_instance(entry, instance_id, error_code, diagnostic);
}

InstanceOpen PanelHost::open_instance(const std::string& panel_id,
                                      const std::string& requested_instance_id)
{
    InstanceOpen out;
    std::string error_code;
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        out.code = error_code;
        out.diagnostic = "panel '" + panel_id + "' cannot be hosted by this build";
        return out;
    }
    if (!requested_instance_id.empty())
    {
        if (Instance* existing = find_instance(*entry, requested_instance_id); existing != nullptr)
        {
            // ALREADY OPEN, AND THAT IS AN ANSWER, NOT A FAILURE (design 04 section 3). A restore
            // naming an id it already restored gets that copy back rather than a refusal it would
            // have to tell apart from a real one.
            out.outcome = InstanceOutcome::focused;
            out.instance_id = existing->id;
            return out;
        }
    }
    else if (entry->manifest.instances.mode == gc::InstanceMode::singleton &&
             !entry->instances.empty())
    {
        // THE SINGLETON RULE, and the one behaviour `dock.singleton` never had: a second open
        // FOCUSES the live copy. `panelhost.ts`'s `open` refused every second open of every panel,
        // which is why the flag read as decorative -- there was no path on which it could differ.
        out.outcome = InstanceOutcome::focused;
        out.instance_id = entry->instances.front().id;
        return out;
    }
    const std::string id = requested_instance_id.empty()
                               ? make_panel_instance_id(panel_id, entry->next_ordinal)
                               : requested_instance_id;
    Instance* created = create_instance(*entry, id, out.code, out.diagnostic);
    if (created == nullptr)
    {
        return out;
    }
    out.outcome = InstanceOutcome::opened;
    out.instance_id = created->id;
    out.code.clear();
    out.diagnostic.clear();
    return out;
}

bool PanelHost::close_instance(const std::string& panel_id, const std::string& instance_id)
{
    Entry* entry = find(panel_id);
    if (entry == nullptr)
    {
        return false;
    }
    // ONE spelling of "which live copy does this id name" (the predicate `find_instance` applies).
    // Erasing at most one is not an assumption: every creation path checks `find_instance` first, so
    // ids are unique within an entry. Hand-rolling the scan is what forced the iterator arithmetic.
    return std::erase_if(entry->instances,
                         [&](const Instance& in) { return in.id == instance_id; }) > 0;
}

std::vector<std::string> PanelHost::instances(const std::string& panel_id) const
{
    std::vector<std::string> ids;
    const Entry* entry = find(panel_id);
    if (entry == nullptr)
    {
        return ids;
    }
    ids.reserve(entry->instances.size());
    for (const Instance& instance : entry->instances)
    {
        ids.push_back(instance.id);
    }
    return ids;
}

std::string PanelHost::addressed_instance(const std::string& panel_id,
                                          const std::string& instance_id) const
{
    if (!instance_id.empty())
    {
        return instance_id;
    }
    const Entry* entry = find(panel_id);
    return entry == nullptr || entry->instances.empty() ? std::string() : entry->instances.front().id;
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
        // --- manifest v3 (04 §2) ------------------------------------------------------------------
        // DECLARATIVE ONLY, and inert on this side of the wire until their consumers land: `instances`
        // is read by c3's instance runtime, `path` by d1's Window menu, `selection`/`events` by d2's
        // package fact bus. They are projected NOW, with the contract that introduced them, because
        // `read_package_manifest` is the C++ INVERSE of this projection (package_store.h) — a member
        // parsed on one side and not emitted on the other is exactly the drift that invariant exists
        // to forbid.
        panel.set("instances", project_instances(m.instances));
        panel.set("path", contract::Json(m.path));

        contract::Json selection = contract::Json::object();
        selection.set("subjects", project_string_array(m.selection.subjects));
        panel.set("selection", std::move(selection));

        contract::Json events = contract::Json::object();
        events.set("publishes", project_string_array(m.events.publishes));
        events.set("subscribes", project_string_array(m.events.subscribes));
        panel.set("events", std::move(events));

        contract::Json content = contract::Json::object();
        content.set("type", contract::Json(gc::content_type_token(m.content.type)));
        content.set("entry", contract::Json(m.content.entry));
        panel.set("content", content);

        contract::Json state = contract::Json::object();
        state.set(gc::kStateSchemaVersionKey,
                  contract::Json(static_cast<std::uint64_t>(m.state.schema_version)));
        panel.set("state", state);

        panel.set("capabilities", project_string_array(m.capabilities));
        // The manifest-declared commands (04 §3), the editor-core command registry's source (c)
        // (M9 e07b). Empty for every built-in uitree panel (they declare on the C++ model); an
        // iframe contribution carries its own here.
        panel.set("commands", project_commands(m.commands));

        // The HOST facts. `hosted` gates everything else the runtime may attempt; `gestures` and
        // `persists` tell it which optional verbs exist, so it never sends one that can only be
        // refused. An unhosted panel reports false for both — it has no provider to ask.
        panel.set("hosted", contract::Json(entry.hosted));
        // ⚠ THE BINDING'S capability shape, not a LIVE INSTANCE's (c3). The roster is read at boot,
        // before any panel is opened, so an answer derived from the open set would report "no
        // gestures" for every multi-instance panel and the renderer would never send one; it would
        // also FLIP mid-session as copies open and close. `bind` decides it once -- from the shared
        // provider, or from the factory's probe -- and this reports that decision.
        panel.set("gestures", contract::Json(entry.hosted && entry.offers_gestures));
        panel.set("persists", contract::Json(entry.hosted && entry.offers_state));
        panel.set("revision", contract::Json(entry.revision));
        panels.push_back(std::move(panel));
    }

    contract::Json out = contract::Json::object();
    out.set("contractMajor", contract::Json(static_cast<std::uint64_t>(gc::kContractMajor)));
    out.set("panels", std::move(panels));
    return out;
}

std::optional<PanelRender> PanelHost::render(const std::string& panel_id, std::string& error_code,
                                             const std::string& instance_id)
{
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    const Instance* instance = resolve_instance(*entry, instance_id, error_code);
    if (instance == nullptr)
    {
        return std::nullopt;
    }

    const ut::Panel panel = instance->provider.build();

    PanelRender out;
    out.panel_id = panel_id;
    out.instance_id = instance->id;
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
                       const contract::Json& params, bool& dispatched, std::string& error_code,
                       const std::string& instance_id)
{
    dispatched = false;
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    Instance* instance = resolve_instance(*entry, instance_id, error_code);
    if (instance == nullptr)
    {
        return false;
    }
    if (instance->provider.invoke == nullptr)
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
    const ut::Panel panel = instance->provider.build();
    if (!panel.has_command(command_id))
    {
        error_code = kErrPanelUnknownCommand;
        return false;
    }

    dispatched = instance->provider.invoke(command_id, params);
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
                        bool& dispatched, std::string& error_code, const std::string& instance_id)
{
    dispatched = false;
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    Instance* instance = resolve_instance(*entry, instance_id, error_code);
    if (instance == nullptr)
    {
        return false;
    }
    if (instance->provider.gesture == nullptr)
    {
        error_code = kErrPanelBadGesture;
        return false;
    }
    dispatched = instance->provider.gesture(verb, params);
    if (dispatched)
    {
        ++entry->revision;
    }
    return true;
}

std::optional<contract::Json> PanelHost::get_state(const std::string& panel_id,
                                                   std::string& error_code,
                                                   const std::string& instance_id)
{
    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return std::nullopt;
    }
    const Instance* instance = resolve_instance(*entry, instance_id, error_code);
    if (instance == nullptr)
    {
        return std::nullopt;
    }
    if (instance->provider.get_state == nullptr)
    {
        error_code = kErrPanelNoState;
        return std::nullopt;
    }
    // The blob shape is `{schemaVersion, data}` and the VERSION comes from the manifest, not from
    // the provider: the roster is where a panel declares the version it writes today (04 §3), so a
    // provider cannot stamp a version the manifest disagrees with.
    gc::PanelState state;
    state.schema_version = entry->manifest.state.schema_version;
    state.data = instance->provider.get_state();
    return gc::persist_panel_state(state);
}

bool PanelHost::restore_state(const std::string& panel_id, const contract::Json& persisted,
                              bool& restored, std::string& code, std::string& diagnostic,
                              std::string& error_code, const std::string& instance_id)
{
    restored = false;
    code.clear();
    diagnostic.clear();

    Entry* entry = resolve_hosted(panel_id, error_code);
    if (entry == nullptr)
    {
        return false;
    }
    Instance* instance = resolve_instance(*entry, instance_id, error_code);
    if (instance == nullptr)
    {
        return false;
    }
    if (instance->provider.restore_state == nullptr)
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

    restored = instance->provider.restore_state(outcome.state->data);
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
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_optional_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(
                         kErrPanelBadParams,
                         "panel.render requires a string 'panelId' and an optional string "
                         "'instanceId'");
                 }
                 std::string error_code;
                 const std::optional<PanelRender> rendered = render(panel_id, error_code, instance_id);
                 if (!rendered.has_value())
                 {
                     return BridgeResult::error(
                         error_code, refusal_message(panel_id, error_code,
                                                     "panel '" + panel_id +
                                                         "' cannot be rendered by this build"));
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
                 // ECHOED, ALWAYS (c3) — including for a call that named none. A renderer holding
                 // several copies of one kind routes the payload on this, and a reply that carried
                 // only the kind would leave it guessing which slot to patch.
                 out.set("instanceId", contract::Json(rendered->instance_id));
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
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_string(request.params, "commandId", command_id) ||
                     !read_optional_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(
                         kErrPanelBadParams,
                         "panel.command requires string 'panelId' and 'commandId' and an optional "
                         "string 'instanceId'");
                 }
                 bool dispatched = false;
                 std::string error_code;
                 if (!invoke(panel_id, command_id, request.params, dispatched, error_code,
                             instance_id))
                 {
                     return BridgeResult::error(
                         error_code, refusal_message(panel_id, error_code,
                                                     "panel '" + panel_id + "' cannot dispatch '" +
                                                         command_id + "'"));
                 }
                 contract::Json out = contract::Json::object();
                 out.set("dispatched", contract::Json(dispatched));
                 out.set("instanceId",
                         contract::Json(addressed_instance(panel_id, instance_id)));
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
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_string(request.params, "verb", verb_token) ||
                     !read_optional_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.gesture requires string 'panelId' and 'verb' "
                                                "and an optional string 'instanceId'");
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
                 if (!gesture(panel_id, *verb, request.params, dispatched, error_code, instance_id))
                 {
                     // Same rule as panel.state.get below: this path also carries `panel.unknown`
                     // and `panel.not_hosted`, for which "does not accept gestures" is simply
                     // false. Report the code, not a guessed cause.
                     return BridgeResult::error(
                         error_code,
                         refusal_message(panel_id, error_code,
                                         "panel.gesture refused for '" + panel_id + "': " +
                                             error_code));
                 }
                 contract::Json out = contract::Json::object();
                 out.set("dispatched", contract::Json(dispatched));
                 out.set("instanceId",
                         contract::Json(addressed_instance(panel_id, instance_id)));
                 out.set("revision", contract::Json(revision(panel_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelStateGetMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_optional_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.state.get requires a string 'panelId' and an "
                                                "optional string 'instanceId'");
                 }
                 std::string error_code;
                 const std::optional<contract::Json> state =
                     get_state(panel_id, error_code, instance_id);
                 if (!state.has_value())
                 {
                     // The MESSAGE must not assert a cause the CODE contradicts. This path is
                     // reached for `panel.unknown` and `panel.not_hosted` as well as
                     // `panel.no_state`, so name the code instead of claiming the panel persists
                     // no state — a refusal that misreports its own reason costs a debugging round.
                     return BridgeResult::error(
                         error_code,
                         refusal_message(panel_id, error_code,
                                         "panel.state.get refused for '" + panel_id + "': " +
                                             error_code));
                 }
                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(panel_id));
                 out.set("instanceId",
                         contract::Json(addressed_instance(panel_id, instance_id)));
                 out.set("state", *state);
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kPanelStateSetMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_optional_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(kErrPanelBadParams,
                                                "panel.state.set requires a string 'panelId' and an "
                                                "optional string 'instanceId'");
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
                 if (!restore_state(panel_id, persisted, restored, code, diagnostic, error_code,
                                    instance_id))
                 {
                     return BridgeResult::error(
                         error_code, refusal_message(panel_id, error_code,
                                                     "panel '" + panel_id +
                                                         "' cannot restore state"));
                 }
                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(panel_id));
                 out.set("instanceId",
                         contract::Json(addressed_instance(panel_id, instance_id)));
                 out.set("restored", contract::Json(restored));
                 out.set("code", contract::Json(code));
                 out.set("diagnostic", contract::Json(diagnostic));
                 out.set("revision", contract::Json(revision(panel_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    // THE INSTANCE RELEASE VERB (c3). See kPanelInstanceCloseMethod on why release is explicit while
    // creation is implicit. `closed:false` for an id that is not live is an ORDINARY answer, not an
    // error: a double close, or a close racing a window teardown, is not a protocol fault and making
    // the renderer branch on an error for it would push a try/catch into every close path.
    ok = router.register_method(
             kPanelInstanceCloseMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string panel_id;
                 std::string instance_id;
                 if (!read_string(request.params, "panelId", panel_id) ||
                     !read_string(request.params, "instanceId", instance_id))
                 {
                     return BridgeResult::error(
                         kErrPanelBadParams,
                         "panel.instance.close requires string 'panelId' and 'instanceId'");
                 }
                 contract::Json out = contract::Json::object();
                 out.set("panelId", contract::Json(panel_id));
                 out.set("instanceId", contract::Json(instance_id));
                 out.set("closed", contract::Json(close_instance(panel_id, instance_id)));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    return ok;
}

} // namespace context::editor::shell

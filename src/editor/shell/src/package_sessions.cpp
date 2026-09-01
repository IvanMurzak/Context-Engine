// Per-package baseline daemon sessions + the `panel.daemon.call` fan-in route (see package_sessions.h).

#include "context/editor/shell/package_sessions.h"

#include "context/editor/shell/ext_scheme.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace context::editor::shell
{

namespace
{

// Read a required string member off a params object. False (leaving `out` untouched) when the member
// is absent or not a string — the caller answers kErrPackageBadParams. Mirrors panel_host.cpp's
// helper; the params here come from the SAME untrusted-renderer channel and get the same discipline.
[[nodiscard]] bool read_string(const contract::Json& params, const std::string& key, std::string& out)
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

// Is `topic` a PACKAGE-namespaced fact topic (as opposed to a contract-owned one)?
//
// The discriminator is the dot, and it is the SAME one `EventStream::package_topic_defect` uses one
// layer down: every contract-owned topic (`files`, `derivation`, `diagnostics`, `session`,
// `clients`, `log`) is a single bare segment, so "contains a dot" is exactly "is a package topic".
// Written as a predicate over the SHAPE rather than as a list of core names deliberately — a list
// here would have to track `Registry::topics()`, and a core topic this Shell had not heard of would
// then be treated as a package topic and FILTERED OUT of every package's stream, which is a silent
// delivery failure rather than a refusal.
[[nodiscard]] bool is_package_fact_topic(const std::string& topic)
{
    return topic.find('.') != std::string::npos;
}

} // namespace

const std::vector<std::string>& panel_callable_daemon_methods()
{
    // See the header for why each entry is here and — more importantly — why the absences are.
    static const std::vector<std::string> kAllowed = {
        "describe",           // the contract self-description (no project data)
        "query",              // the ONE authored-data read (R-CLI-012)
        "editor.scene-tree",  // the e05d3 composed scene projection
        "editor.inspect",     // the e05d3 composed entity projection
        // e13c-2 — the R-CLI-015 subscription protocol, admissible ONLY because package_events.h now
        // bounds the fan-out. All three or none: see the header's allowlist note.
        "subscribe",          // open a subscription on this package's baseline session
        "unsubscribe",        // close one (without it, every re-subscribe leaks a subId)
        "ack",                // advance the retention cursor (without it, the daemon's ring is pinned)
    };
    return kAllowed;
}

bool is_panel_callable_daemon_method(const std::string& method)
{
    // EXACT match, and deliberately NOT the dotted-prefix rule `is_forbidden_bridge_method` uses. That
    // one widens a DENYLIST (denying `instance` must also deny `instance.token`, or appending a
    // segment defeats it); widening an ALLOWLIST is the opposite operation and would grant every
    // `query.<anything>` a future task registers, sight unseen. An allowlist that grows on its own is
    // not one.
    const std::vector<std::string>& allowed = panel_callable_daemon_methods();
    return std::find(allowed.begin(), allowed.end(), method) != allowed.end();
}

PackageSessionHost::PackageSessionHost(ClientFactory factory, std::size_t max_sessions)
    : PackageSessionHost(std::move(factory), ScopeResolver{}, max_sessions)
{
}

PackageSessionHost::PackageSessionHost(ClientFactory factory, ScopeResolver scope_resolver,
                                       std::size_t max_sessions)
    : factory_(std::move(factory)), scope_resolver_(std::move(scope_resolver)),
      // 0 would mean "no package may ever call", which reads as a disabled feature rather than a
      // configuration error; clamping to 1 matches `set_max_connections`' own handling of 0 and keeps
      // the cap a bound rather than a switch.
      max_sessions_(max_sessions == 0 ? 1 : max_sessions)
{
}

std::string PackageSessionHost::attach_scope_for(const std::string& package_id) const
{
    // FAIL CLOSED IN BOTH DIRECTIONS (control 1): an ABSENT resolver is the e13c-1 deny-all build, and
    // a resolver that answers EMPTY — a grant document that could not be read, a package nobody
    // consented to, a wiring bug — lands on the same baseline rather than on `AttachOptions`' silent
    // default. The two are deliberately the SAME answer: there is no state in which "we do not know"
    // may grant more than "we know nothing was granted".
    if (!scope_resolver_)
    {
        return kPackageSessionScope;
    }
    std::string spec = scope_resolver_(package_id);
    return spec.empty() ? std::string(kPackageSessionScope) : spec;
}

void PackageSessionHost::set_fact_policy(TopicResolver publishes, SubscribeGate may_subscribe)
{
    fact_topics_ = std::move(publishes);
    fact_gate_ = std::move(may_subscribe);
}

bool PackageSessionHost::may_receive_fact(const std::string& package_id,
                                          const std::string& topic) const
{
    // A CONTRACT-OWNED topic is not this control's business: `files` / `diagnostics` / `session` are
    // the ordinary daemon stream every baseline client may read, and gating them here would be a
    // second, drifting copy of the R-SEC-007 scope decision the dispatcher already made.
    if (!is_package_fact_topic(topic))
    {
        return true;
    }
    // FAIL CLOSED with no gate: an unwired build delivers no package facts at all, which is
    // byte-for-byte the pre-d2 editor rather than an editor that quietly forwards everything.
    return fact_gate_ ? fact_gate_(package_id, topic) : false;
}

bool PackageSessionHost::has_session(const std::string& package_id) const
{
    return std::any_of(sessions_.begin(), sessions_.end(),
                       [&](const Session& s) { return s.package_id == package_id; });
}

void PackageSessionHost::reset()
{
    sessions_.clear();
    // WITH the sessions, deliberately — see the header. A buffered event's seq belongs to the
    // incarnation of the connection that delivered it, so surviving the connection would hand a panel
    // a cursor into a dead lifetime.
    events_.clear();
}

std::size_t PackageSessionHost::pump()
{
    std::size_t buffered = 0;
    for (Session& session : sessions_)
    {
        if (session.client == nullptr)
        {
            continue;
        }
        // BOUNDED PER PUMP: a responsiveness bound, not a memory one (package_events.h
        // § kMaxDrainedFramesPerPump). Anything still on the wire rides the next frame's pump.
        for (std::size_t drained = 0; drained < kMaxDrainedFramesPerPump; ++drained)
        {
            bool disconnected = false;
            // 0 ms — this runs on the owner loop, so it must never wait. A quiet session answers
            // nullopt immediately and costs one non-blocking read.
            // NON-const so the envelope below MOVES. `contract::Json` is a recursive value type, so
            // a copy here reallocates the whole tree — and `Client::poll_event` is deliberately
            // non-const-returning for exactly this (client.cpp, "the SDK's hottest path").
            std::optional<client::InboundFrame> frame = session.client->poll_event(0, disconnected);
            if (!frame.has_value())
            {
                // Nothing queued, or the peer went away. Either way this session has no more to give
                // THIS frame; a lost daemon is the lifecycle's business, not this pump's.
                break;
            }
            if (frame->kind == client::FrameKind::gap)
            {
                // THE DAEMON's ring outran us. Latch the flag WITHOUT touching `dropped`, so the two
                // causes stay distinguishable (package_events.h § mark_gap).
                events_.mark_gap(session.package_id);
                continue;
            }
            if (frame->kind != client::FrameKind::event)
            {
                // A response to an abandoned call, or a frame this SDK does not model. Ignorable by
                // construction (wire.h § FrameKind::unknown) — never fatal to a subscription.
                continue;
            }
            // THE WIRE ENVELOPE, CARRIED VERBATIM plus the `subId` it arrived under. The Shell never
            // interprets a package's events (the same opaque-payload discipline UiMirrorStore keeps),
            // but a package holding several subscriptions cannot demultiplex without the subId, and
            // the daemon puts it OUTSIDE the event object — so dropping it here would make multiple
            // subscriptions per package unusable.
            contract::Json delivered = std::move(frame->event);
            // ⚠ CONTROL 6's SECOND APPLICATION — THE DELIVERY FILTER, AND IT IS THE ONE THAT
            // ACTUALLY BINDS. `forward` refuses a `subscribe` that NAMES another package's topic,
            // but `subscribe` with an EMPTY topic list means EVERY topic (event_stream.h
            // § Subscriber::wants), and that request names nothing to refuse — so without this the
            // consent gate would be one `subscribe:{}` away from irrelevant. Filtering HERE, on the
            // one path every delivered event passes, is what makes the grant a property of what a
            // package RECEIVES rather than of what it asked for. Two controls, independently, in the
            // discipline the allowlist note states.
            //
            // A DROPPED FACT IS NOT A GAP. `gapped` means "your cursor is worthless, re-snapshot",
            // and a package that was never entitled to a topic has lost nothing it could act on —
            // latching it here would order a re-snapshot on every foreign publish for the life of
            // the window. It is counted instead (`events_filtered`), which is a wiring signal for a
            // human and invisible to the package, exactly as an unconsented fact should be.
            if (delivered.is_object() && delivered.contains("topic") &&
                delivered.at("topic").is_string() &&
                !may_receive_fact(session.package_id, delivered.at("topic").as_string()))
            {
                ++events_filtered_;
                continue;
            }
            if (delivered.is_object())
            {
                delivered.set("subId", contract::Json(std::move(frame->sub_id)));
            }
            events_.push(session.package_id, std::move(delivered));
            ++events_buffered_;
            ++buffered;
        }
    }
    return buffered;
}

PackageEventDrain PackageSessionHost::poll_events(const std::string& package_id)
{
    return events_.take(package_id);
}

client::Client* PackageSessionHost::session_for(const std::string& package_id,
                                                std::string& error_code, std::string& message)
{
    for (const Session& session : sessions_)
    {
        if (session.package_id == package_id)
        {
            // POOLED PER PACKAGE, so a package with panels in three windows holds ONE connection —
            // control 4's whole point (header § connection exhaustion).
            return session.client.get();
        }
    }

    // THE SUB-CAP, checked before the factory runs: an attach that would answer `daemon.busy` still
    // costs a connect + handshake, and refusing here is the answer that cannot starve anyone.
    if (sessions_.size() >= max_sessions_)
    {
        ++refused_capacity_;
        error_code = kErrPackageCapacity;
        message = "this editor already holds the maximum of " + std::to_string(max_sessions_) +
                  " package daemon sessions";
        return nullptr;
    }

    std::string factory_error;
    std::unique_ptr<client::Client> client = factory_ ? factory_(factory_error) : nullptr;
    if (client == nullptr)
    {
        error_code = kErrPackageNoSession;
        // The factory's own diagnostic is Shell/daemon state, so it is NOT echoed: this refusal
        // travels to editor-core and on to untrusted panel code, and a discovery error carries the
        // project path (ipc_bridge.h control 3 protects the token + endpoint, not a path). The
        // Shell-side channel keeps the detail.
        message = "no daemon session is available for package '" + package_id + "'";
        return nullptr;
    }

    // CONTROL 1 — THE SCOPE IS DECIDED HERE AND NOWHERE ELSE. Since e13c-4 the VALUE is derived from
    // the operator's persisted consent (`attach_scope_for`), but the SITE is unchanged: one call, at
    // the one attach, with no path by which a package's own request can reach it — `package_id` comes
    // from editor-core's per-panel binding, and the grant it indexes is a Shell document.
    // `options.token` is deliberately left EMPTY: `Client::attach` falls back to the D20 token
    // `connect_to_project` discovered, so the token never passes through this class (nor through the
    // request that triggered it).
    client::AttachOptions options;
    options.scope = attach_scope_for(package_id);

    std::string attach_error;
    if (!client->attach(options, attach_error))
    {
        error_code = kErrPackageNoSession;
        message = "the daemon refused a baseline session for package '" + package_id + "'";
        return nullptr;
    }

    sessions_.push_back(Session{package_id, std::move(client), {}});
    client::Client* opened = sessions_.back().client.get();

    // CONTROL 6's FIRST HALF — D4's "topics registered at install/load", applied at the one moment a
    // package's session exists and before any call of its own rides it.
    //
    // ⚠ BEST-EFFORT, AND FAIL-CLOSED WHEN IT FAILS. A daemon that does not serve `events.declare`
    // (an older build) answers `usage.unknown_verb`, and this must NOT abort the attach: the session
    // is the transport for `query` / `editor.inspect` / `subscribe` too, and killing it over the
    // fact bus would turn a missing feature into a dead panel. What the failure costs is exactly the
    // fact bus and nothing else — the topic stays unregistered, so this package's later
    // `panel.facts.publish` is refused `package.topic_undeclared` by the daemon, which is the
    // truthful diagnostic and the direction to fail in.
    //
    // Declared as ONE request rather than per topic: `events.declare` is all-or-nothing (dispatcher.cpp
    // states why), so a manifest with one malformed name registers none of them and the package's
    // publishes are refused uniformly instead of half working.
    if (fact_topics_)
    {
        const std::vector<std::string> topics = fact_topics_(package_id);
        if (!topics.empty())
        {
            contract::Json list = contract::Json::array();
            for (const std::string& topic : topics)
            {
                list.push_back(contract::Json(topic));
            }
            contract::Json declare_params = contract::Json::object();
            declare_params.set("topics", std::move(list));
            std::string declare_error;
            (void)opened->call("events.declare", declare_params, declare_error);
        }
    }
    return opened;
}

BridgeResult PackageSessionHost::forward(const std::string& package_id, const std::string& method,
                                          const contract::Json& params)
{
    // The id is validated with the SAME predicate the asset scheme mounts against
    // (`is_valid_package_id`), so a package cannot be one thing to the scheme and another to the
    // session table — two spellings of one package would be two connections and two `clients` rows.
    if (!is_valid_package_id(package_id))
    {
        return BridgeResult::error(kErrPackageBadParams,
                                   "panel.daemon.call requires a valid 'packageId'");
    }
    if (method.empty())
    {
        return BridgeResult::error(kErrPackageBadParams,
                                   "panel.daemon.call requires a non-empty string 'method'");
    }

    // CONTROL 2 (S4), AHEAD OF EVERYTHING ELSE. Checked before a session is opened, so a package
    // probing for un-allowlisted methods cannot consume a connection slot doing it — and checked
    // before the method reaches a wire at all, so the daemon's `read_query` default for an unknown
    // method (control 3 / S7) is never the thing standing between a panel and a backend verb.
    if (!is_panel_callable_daemon_method(method))
    {
        ++refused_methods_;
        // The refusal is IDENTICAL whether the method exists in the registry or not — the same rule
        // `bridge.commands.execute` follows for the same reason: a differentiated refusal is an
        // oracle for the daemon's whole verb surface.
        return BridgeResult::error(kErrPackageMethodNotAllowed,
                                   "'" + method + "' is not callable from a package panel");
    }

    // CONTROL 5 (S8), AHEAD OF THE WIRE — a package may only address subscriptions it MINTED.
    //
    // Checked here, before a session is opened, for control 2's own reason: an id-probing package
    // must not consume a connection slot doing it. The daemon cannot make this check for us — it
    // routes `unsubscribe`/`ack` to `serve_subscription` WITHOUT the connection's Session, and
    // matches on a globally-sequential `subId` that carries no owner (header § control 5).
    if (method == "unsubscribe" || method == "ack")
    {
        std::string sub_id;
        if (!read_string(params, "subId", sub_id) || sub_id.empty())
        {
            return BridgeResult::error(kErrPackageBadParams,
                                       "'" + method + "' requires a non-empty string 'subId'");
        }
        const Session* owner = nullptr;
        for (const Session& session : sessions_)
        {
            if (session.package_id == package_id)
            {
                owner = &session;
                break;
            }
        }
        const bool owns =
            owner != nullptr && std::find(owner->sub_ids.begin(), owner->sub_ids.end(), sub_id) !=
                                    owner->sub_ids.end();
        if (!owns)
        {
            ++refused_subscriptions_;
            // ONE code for "another client's" and "no such subscription" — see the header: a
            // differentiated refusal would rebuild the enumeration oracle this control closes.
            return BridgeResult::error(kErrPackageUnknownSubscription,
                                       "no subscription '" + sub_id + "' belongs to package '" +
                                           package_id + "'");
        }
    }
    else if (method == "subscribe")
    {
        // CONTROL 6's FIRST APPLICATION (editor-UX d2, D4) — the CONSENTED cross-package
        // subscription. A request that NAMES another package's topic is refused here, with a
        // diagnostic naming the topic, so a package author is told which grant is missing rather
        // than left watching a subscription that never fires.
        //
        // ⚠ THIS ALONE WOULD NOT BE A CONTROL, and saying so is the point of the pump filter. An
        // empty/absent `topics` means EVERY topic, and there is nothing in that request to refuse —
        // so this arm handles the NAMED case (the good diagnostic) and `pump()` handles what is
        // actually delivered (the binding one). Refusing the empty form instead would break every
        // package that legitimately watches the core stream, to close a hole the filter closes
        // without breaking anyone.
        if (params.is_object() && params.contains("topics") && params.at("topics").is_array())
        {
            const contract::Json& topics = params.at("topics");
            for (std::size_t i = 0; i < topics.size(); ++i)
            {
                if (!topics.at(i).is_string())
                {
                    continue; // the daemon's own parser ignores a non-string entry; so do we
                }
                const std::string& topic = topics.at(i).as_string();
                if (may_receive_fact(package_id, topic))
                {
                    continue;
                }
                ++refused_topics_;
                return BridgeResult::error(
                    kErrPackageTopicNotGranted,
                    "package '" + package_id + "' may not subscribe to '" + topic +
                        "': another package's fact topic requires the operator's consent, "
                        "clamped to what this package's manifest declared in events.subscribes[]");
            }
        }
        // THE SUBSCRIPTION SUB-CAP — control 5's second half. Without it a package can mint subIds
        // without bound; each costs the DAEMON a Subscriber with its own queue and one more fan-out
        // of every matching event, in the process that also serves the CLI and every AI client. The
        // 256-event buffer bounds what this editor HOLDS, never what the daemon does per subId.
        for (const Session& session : sessions_)
        {
            if (session.package_id == package_id &&
                session.sub_ids.size() >= kMaxSubscriptionsPerPackage)
            {
                ++refused_subscriptions_;
                return BridgeResult::error(
                    kErrPackageSubscriptionCapacity,
                    "package '" + package_id + "' already holds the maximum of " +
                        std::to_string(kMaxSubscriptionsPerPackage) + " subscriptions");
            }
        }
    }

    std::string error_code;
    std::string message;
    client::Client* client = session_for(package_id, error_code, message);
    if (client == nullptr)
    {
        return BridgeResult::error(error_code, message);
    }

    ++calls_forwarded_;
    std::string call_error;
    bool rejected_by_daemon = false;
    std::optional<contract::Json> result =
        client->call(method, params, call_error, &rejected_by_daemon);
    if (!result.has_value())
    {
        // THE DAEMON'S OWN CATALOG CODE, VERBATIM — `scope.denied` above all. This is the line that
        // makes the e13 DoD box "un-granted file_write / build_install rejected IN THE DISPATCHER"
        // OBSERVABLE from a panel: re-classifying the refusal here would hide which control fired, and
        // a panel author debugging "my write was refused" needs to know it was the scope and not this
        // Shell. `failure_code` is the R-CLI-008 rule in one place (a transport fault becomes
        // `internal.error`, a daemon refusal keeps its code), so it is used rather than re-derived.
        return BridgeResult::error(client->failure_code(kErrPackageNoSession),
                                   rejected_by_daemon
                                       ? "the daemon refused '" + method + "' for package '" +
                                             package_id + "'"
                                       : "'" + method + "' could not be delivered to the daemon");
    }

    // ⚠ CONTROL 6's THIRD APPLICATION — THE SUBSCRIBE **REPLY**, which neither of the other two can
    // see. `subscribe` answers with the daemon's CURRENT-STATE snapshot, and since d2 that snapshot
    // carries `packageFacts` (D5 rule 2 — retention doubles as snapshot-on-subscribe); a reconnect
    // carrying `sinceSeq` also gets the replayed `catchup` ring, which `replay_since` does NOT filter
    // by the subscription's topics. Neither travels through `pump()`, so relaying the reply verbatim
    // would hand a package every retained foreign fact the daemon holds in ONE allowlisted call —
    // the consent gate bypassed rather than applied, and by the cheaper of the two paths, since the
    // snapshot arrives whether or not any further event is ever published. Filtered here with the
    // SAME predicate the delivery filter uses, so there is one answer to "may this package see this
    // topic" rather than two that can drift.
    //
    // A filtered ENTRY IS NOT A GAP, for the pump filter's reason: `gapped` orders a re-snapshot, and
    // re-snapshotting would only reproduce the same filtered reply. Counted in `events_filtered()`
    // instead, which stays the one wiring signal for a human.
    if (method == "subscribe" && result->is_object() && result->contains("data") &&
        result->at("data").is_object())
    {
        contract::Json data = result->at("data");
        if (data.contains("snapshot") && data.at("snapshot").is_object() &&
            data.at("snapshot").contains("packageFacts") &&
            data.at("snapshot").at("packageFacts").is_array())
        {
            contract::Json snapshot = data.at("snapshot");
            const contract::Json& facts = snapshot.at("packageFacts");
            contract::Json kept = contract::Json::array();
            for (std::size_t i = 0; i < facts.size(); ++i)
            {
                const contract::Json& entry = facts.at(i);
                // A malformed entry is DROPPED rather than passed through: an entry whose topic this
                // Shell cannot read is one it cannot police either, and the deny direction is the
                // only one that stays a control if the reply's shape ever moves.
                if (entry.is_object() && entry.contains("topic") && entry.at("topic").is_string() &&
                    may_receive_fact(package_id, entry.at("topic").as_string()))
                {
                    kept.push_back(entry);
                }
                else
                {
                    ++events_filtered_;
                }
            }
            snapshot.set("packageFacts", std::move(kept));
            data.set("snapshot", std::move(snapshot));
        }
        if (data.contains("catchup") && data.at("catchup").is_array())
        {
            const contract::Json& catchup = data.at("catchup");
            contract::Json kept = contract::Json::array();
            for (std::size_t i = 0; i < catchup.size(); ++i)
            {
                const contract::Json& event = catchup.at(i);
                // The replayed events are the WIRE envelope, so an entry with no readable topic is a
                // shape this Shell does not model — passed through exactly as `pump()` passes one,
                // since `may_receive_fact` is about package topics and nothing else.
                if (!event.is_object() || !event.contains("topic") ||
                    !event.at("topic").is_string() ||
                    may_receive_fact(package_id, event.at("topic").as_string()))
                {
                    kept.push_back(event);
                }
                else
                {
                    ++events_filtered_;
                }
            }
            data.set("catchup", std::move(kept));
        }
        result->set("data", std::move(data));
    }

    // CONTROL 5's LEDGER, maintained from the DAEMON's own answer rather than from the request: the
    // subId a package owns is the one the daemon actually minted for it, so a package cannot claim an
    // id by asking for it. Re-found by package id because `session_for` may have grown `sessions_`
    // and invalidated any pointer taken above.
    if (method == "subscribe" || method == "unsubscribe")
    {
        for (Session& session : sessions_)
        {
            if (session.package_id != package_id)
            {
                continue;
            }
            if (method == "subscribe")
            {
                // THE R-CLI-008 ENVELOPE, read STRICTLY. `Client::call` hands back the daemon's
                // `result`, which IS the `{ok, data}` envelope — `serve_subscription` puts `subId`
                // inside `data`. Reading it strictly is the FAIL-CLOSED direction on purpose: if that
                // shape ever moves, no subId is recorded, every later `unsubscribe`/`ack` is refused
                // as unowned, and the feature degrades LOUDLY instead of the control quietly
                // admitting everything.
                std::string minted;
                if (result->is_object() && result->contains("data") &&
                    read_string(result->at("data"), "subId", minted) && !minted.empty() &&
                    std::find(session.sub_ids.begin(), session.sub_ids.end(), minted) ==
                        session.sub_ids.end())
                {
                    session.sub_ids.push_back(std::move(minted));
                }
            }
            else
            {
                // Ownership was proven above, so this erases exactly the package's own entry — and
                // frees a slot under the sub-cap, which is what makes `unsubscribe` the release
                // valve rather than a second way to spend the budget.
                std::string sub_id;
                (void)read_string(params, "subId", sub_id);
                const auto it = std::find(session.sub_ids.begin(), session.sub_ids.end(), sub_id);
                if (it != session.sub_ids.end())
                {
                    session.sub_ids.erase(it);
                }
            }
            break;
        }
    }
    return BridgeResult::ok(*result);
}

BridgeResult PackageSessionHost::publish_fact(const std::string& package_id,
                                              const std::string& topic,
                                              const contract::Json& payload)
{
    // The SAME id predicate `forward` uses, for the same reason: a package that is one thing to the
    // scheme and another to the session table would publish from a connection nothing attributes.
    if (!is_valid_package_id(package_id))
    {
        return BridgeResult::error(kErrPackageBadParams,
                                   "panel.facts.publish requires a valid 'packageId'");
    }
    if (topic.empty())
    {
        return BridgeResult::error(kErrPackageBadParams,
                                   "panel.facts.publish requires a non-empty string 'topic'");
    }
    // ⚠ THE DECLARATION CHECK IS **NOT** HERE. It is `PackageFactHost`'s, because the manifest is,
    // and this method is unreachable except through it (the header's note on why this is not an
    // allowlist entry). What IS here is the session, which is the only thing this class owns.
    std::string error_code;
    std::string message;
    client::Client* client = session_for(package_id, error_code, message);
    if (client == nullptr)
    {
        return BridgeResult::error(error_code, message);
    }
    contract::Json params = contract::Json::object();
    params.set("topic", contract::Json(topic));
    params.set("payload", payload);
    std::string call_error;
    bool rejected_by_daemon = false;
    const std::optional<contract::Json> result =
        client->call("events.publish", params, call_error, &rejected_by_daemon);
    if (!result.has_value())
    {
        // THE DAEMON'S OWN CODE, VERBATIM — `package.topic_undeclared`, `package.fact_reentrant`,
        // `package.fact_too_large`. Re-classifying them here would hide WHICH control fired, and
        // every one of them is something the package author has to act on differently.
        return BridgeResult::error(client->failure_code(kErrPackageNoSession),
                                   rejected_by_daemon
                                       ? "the daemon refused the fact published on '" + topic +
                                             "' by package '" + package_id + "'"
                                       : "the fact on '" + topic +
                                             "' could not be delivered to the daemon");
    }
    // COUNTED ON THE DAEMON'S ANSWER, not on the attempt: the accessor documents this as a fact the
    // daemon ACCEPTED (a D5 dedup counts — it was accepted and deliberately emitted nothing), so
    // incrementing before the call would fold every `topic_undeclared` / `fact_too_large` /
    // `fact_reentrant` refusal into the same number and leave it unable to answer the question it
    // exists for.
    ++facts_published_;
    return BridgeResult::ok(*result);
}

std::size_t PackageSessionHost::subscriptions_open(const std::string& package_id) const
{
    for (const Session& session : sessions_)
    {
        if (session.package_id == package_id)
        {
            return session.sub_ids.size();
        }
    }
    return 0;
}

bool PackageSessionHost::install(BridgeRouter& router)
{
    // e13c-2 — THE DRAIN. Registered first so a failure here is reported before the fan-in binding
    // succeeds and leaves the surface half-installed.
    //
    // ⚠ AN UNKNOWN PACKAGE IS AN EMPTY DRAIN, NOT A REFUSAL. editor-core polls on a fixed tick for
    // every package it has a panel mounted for, so "this package has not subscribed" is the ordinary
    // case; refusing it would make every idle tick a refusal on a channel whose live smokes assert
    // `bridge.refused() == 0`, which is exactly the regression session_bridge.h records for e06d.
    if (!router.register_method(
            kPanelEventsPollMethod,
            [this](const BridgeRequest& request) -> BridgeResult
            {
                std::string package_id;
                if (!read_string(request.params, "packageId", package_id) ||
                    !is_valid_package_id(package_id))
                {
                    // The SAME predicate `forward` validates against, for the same reason: a package
                    // that is one thing to the scheme and another to the buffer would drain a
                    // mailbox nothing fills.
                    return BridgeResult::error(kErrPackageBadParams,
                                               "panel.events.poll requires a valid 'packageId'");
                }
                // NON-const so the batch MOVES onto the reply. `drain` is a fully-owned local that
                // dies with this lambda, and `take()` already went to the trouble of moving each
                // event out of the deque — copying them back would deep-copy up to
                // `kMaxBufferedEventsPerPackage` JSON trees per poll, on the owner loop.
                PackageEventDrain drain = poll_events(package_id);
                contract::Json events = contract::Json::array();
                for (contract::Json& event : drain.events)
                {
                    events.push_back(std::move(event));
                }
                contract::Json result = contract::Json::object();
                result.set("events", std::move(events));
                // THE LOUD HALF, and the reason it rides the reply rather than a counter: this is the
                // only channel that reaches the PACKAGE, and a panel that missed events looks exactly
                // like a panel whose subject did not change (package_events.h § a drop is loud).
                result.set("dropped", contract::Json(drain.dropped));
                result.set("gapped", contract::Json(drain.gapped));
                return BridgeResult::ok(std::move(result));
            }))
    {
        return false;
    }

    return router.register_method(kPanelDaemonCallMethod,
                                  [this](const BridgeRequest& request) -> BridgeResult
                                  {
                                      std::string package_id;
                                      std::string method;
                                      if (!read_string(request.params, "packageId", package_id) ||
                                          !read_string(request.params, "method", method))
                                      {
                                          return BridgeResult::error(
                                              kErrPackageBadParams,
                                              "panel.daemon.call requires string 'packageId' and "
                                              "'method'");
                                      }
                                      // A MISSING `params` is the ordinary no-argument call, not a
                                      // malformed one: `Json::at` answers a shared null for a missing
                                      // key, and the daemon's own verbs read their arguments off an
                                      // object they may find empty. Refusing it here would make every
                                      // caller send `params:{}` to say nothing.
                                      return forward(package_id, method, request.params.at("params"));
                                  });
}

} // namespace context::editor::shell

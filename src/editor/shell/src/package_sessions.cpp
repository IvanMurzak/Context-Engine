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

} // namespace

const std::vector<std::string>& panel_callable_daemon_methods()
{
    // See the header for why each entry is here and — more importantly — why the absences are.
    static const std::vector<std::string> kAllowed = {
        "describe",           // the contract self-description (no project data)
        "query",              // the ONE authored-data read (R-CLI-012)
        "editor.scene-tree",  // the e05d3 composed scene projection
        "editor.inspect",     // the e05d3 composed entity projection
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
    : factory_(std::move(factory)),
      // 0 would mean "no package may ever call", which reads as a disabled feature rather than a
      // configuration error; clamping to 1 matches `set_max_connections`' own handling of 0 and keeps
      // the cap a bound rather than a switch.
      max_sessions_(max_sessions == 0 ? 1 : max_sessions)
{
}

bool PackageSessionHost::has_session(const std::string& package_id) const
{
    return std::any_of(sessions_.begin(), sessions_.end(),
                       [&](const Session& s) { return s.package_id == package_id; });
}

void PackageSessionHost::reset()
{
    sessions_.clear();
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

    // CONTROL 1 — THE SCOPE IS DECIDED HERE AND NOWHERE ELSE. `options.token` is deliberately left
    // EMPTY: `Client::attach` falls back to the D20 token `connect_to_project` discovered, so the
    // token never passes through this class (nor through the request that triggered it).
    client::AttachOptions options;
    options.scope = kPackageSessionScope;

    std::string attach_error;
    if (!client->attach(options, attach_error))
    {
        error_code = kErrPackageNoSession;
        message = "the daemon refused a baseline session for package '" + package_id + "'";
        return nullptr;
    }

    sessions_.push_back(Session{package_id, std::move(client)});
    return sessions_.back().client.get();
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
    const std::optional<contract::Json> result =
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
    return BridgeResult::ok(*result);
}

bool PackageSessionHost::install(BridgeRouter& router)
{
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

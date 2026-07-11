// R-EDIT-001 CEF extension sandbox policy (R-UI-007 / R-SEC-007, issue #152): the concrete, headless
// model of the editor-extension renderer trust boundary — its own boundary, distinct from the L-49
// TS/WASM tier.
//
// Because the editor GUI is CEF (R-UI-007), the "sandbox-respecting" extension contract is concrete:
// Node integration + native-addon access OFF, a per-extension isolated renderer with site isolation,
// a strict CSP, untrusted panels in sandbox-attributed iframes, and NO direct access to the daemon
// socket or attach token. The bridge exposed to an extension is capability-scoped — DEFAULT read/query
// only, never ambient file-write/build/install (those require an explicit R-SEC-007 grant). In v1 this
// is "design-time contract" (specified + shaped into the architecture); modelling it as a headless
// policy object makes it verbatim-applied by the host AND CI-assertable now.

#pragma once

#include "context/editor/bridge/scope.h"

#include <string>

namespace context::editor::gui::contract
{

// The strict default CSP for an extension renderer: no remote script, no eval, self origin only
// (data: images allowed for inline placeholder art). Grep-stable.
inline constexpr const char* kDefaultExtensionCsp =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'";

// The renderer trust boundary applied to one contribution. The four booleans are the non-negotiable
// half of R-EDIT-001 (they may not be relaxed by a contribution); `granted_scopes` is the
// capability-scoped bridge grant, defaulting to the R-SEC-007 read/query baseline (least privilege).
struct SandboxPolicy
{
    bool node_integration = false;       // native-addon / Node access — MUST stay false
    bool isolated_renderer = true;       // per-extension isolated renderer + site isolation
    bool sandboxed_iframe = true;        // untrusted panels load in a sandbox-attributed iframe
    bool daemon_socket_access = false;   // extensions get NO direct socket / attach-token access
    std::string csp = kDefaultExtensionCsp;
    // The bridge exposed to the extension is capability-scoped: default read/query ONLY. Higher
    // scopes require an explicit R-SEC-007 grant, never ambient.
    bridge::ScopeSet granted_scopes = bridge::ScopeSet::read_query();
};

// Is `policy` conformant with the non-negotiable R-EDIT-001 renderer trust boundary? The host refuses
// to create a renderer for a non-conformant policy. Conformant ==
//   node_integration == false && isolated_renderer && sandboxed_iframe && !daemon_socket_access &&
//   a non-empty CSP.
// (The scope grant is orthogonal — a higher grant is a legitimate, explicitly-authorized choice; it
// is enforced at the bridge, not here.)
[[nodiscard]] bool sandbox_conformant(const SandboxPolicy& policy);

} // namespace context::editor::gui::contract

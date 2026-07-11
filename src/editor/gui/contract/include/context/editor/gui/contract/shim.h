// The capability-scoped bridge shim (R-EDIT-001 / R-SEC-007, issue #152): the bridge an editor
// extension (or a built-in panel) talks to. It wraps the ONE bridge dispatcher and an attached
// session whose scopes are CLAMPED to the contribution's SandboxPolicy grant — so a panel is
// constrained EXACTLY like a scoped remote client, never implicitly trusted because it renders in the
// editor. Every call routes through the dispatcher's per-method R-SEC-007 scope check; there is no
// ambient elevation. This makes "no direct daemon-socket access; capability-scoped bridge" concrete:
// a panel never holds the socket/token — it holds this shim, over the in-process dispatcher.

#pragma once

#include "context/editor/gui/contract/sandbox.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include <string>
#include <variant>

namespace context::editor::gui::contract
{

// The CLI/RPC contract cluster. Aliased because our own namespace leaf is also `contract`, which
// would shadow a bare `contract::` reference.
namespace cli = ::context::editor::contract;

class ExtensionBridge
{
public:
    // Wrap an already-attached session. `dispatcher` must outlive the shim (non-owning).
    ExtensionBridge(const bridge::Dispatcher& dispatcher, bridge::Session session)
        : dispatcher_(&dispatcher), session_(std::move(session))
    {
    }

    // Attach-and-clamp factory: negotiate a session on `dispatcher` for `handshake`, requesting AT
    // MOST `policy.granted_scopes` (the dispatcher further clamps to the launch-time ceiling). Returns
    // the shim on success or the hard-fail Envelope (e.g. handshake.incompatible_protocol) on failure.
    [[nodiscard]] static std::variant<ExtensionBridge, cli::Envelope>
    attach(const bridge::Dispatcher& dispatcher, const cli::ClientHandshake& handshake,
           const SandboxPolicy& policy);

    // Invoke one bridge method through the capability-scoped session. A method requiring a scope the
    // grant did not include returns the scope.denied envelope (enforced in the dispatcher, R-SEC-007).
    [[nodiscard]] cli::Envelope invoke(const std::string& method, const cli::Json& params) const;

    [[nodiscard]] const bridge::Session& session() const noexcept { return session_; }

private:
    const bridge::Dispatcher* dispatcher_;
    bridge::Session session_;
};

} // namespace context::editor::gui::contract

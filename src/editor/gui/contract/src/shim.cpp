// The capability-scoped bridge shim implementation.

#include "context/editor/gui/contract/shim.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include <string>
#include <variant>

namespace context::editor::gui::contract
{

std::variant<ExtensionBridge, cli::Envelope>
ExtensionBridge::attach(const bridge::Dispatcher& dispatcher, const cli::ClientHandshake& handshake,
                        const SandboxPolicy& policy)
{
    // Request AT MOST the policy's grant; the dispatcher clamps further to the launch-time ceiling.
    bridge::Dispatcher::AttachResult result = dispatcher.attach(handshake, policy.granted_scopes);
    if (const cli::Envelope* env = std::get_if<cli::Envelope>(&result))
    {
        return *env; // hard-fail (e.g. handshake.incompatible_protocol)
    }
    return ExtensionBridge(dispatcher, std::get<bridge::Session>(std::move(result)));
}

cli::Envelope ExtensionBridge::invoke(const std::string& method, const cli::Json& params) const
{
    return dispatcher_->dispatch(method, params, session_);
}

} // namespace context::editor::gui::contract

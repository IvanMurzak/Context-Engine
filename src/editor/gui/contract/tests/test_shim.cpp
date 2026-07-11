// Capability-scoped bridge shim tests (R-EDIT-001 / R-SEC-007): a default (read/query) extension
// bridge can run a read verb but is DENIED a build/install verb at the dispatcher — proving a panel
// is constrained exactly like a scoped remote client, not implicitly trusted. An explicit higher
// grant flows through. A protocol mismatch hard-fails the attach.

#include "context/editor/gui/contract/sandbox.h"
#include "context/editor/gui/contract/shim.h"

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include "contract_test.h"

#include <variant>

using namespace context::editor::gui::contract;
namespace bridge = context::editor::bridge;
namespace cli = context::editor::contract;

int main()
{
    // --- default read/query shim: `describe` works, `package.add` is scope-denied ---------------
    {
        bridge::Dispatcher dispatcher; // no stream, no backend, ceiling = all scopes
        SandboxPolicy policy;          // default grant = read/query baseline
        cli::ClientHandshake handshake;
        handshake.protocol_major = cli::kProtocolMajor;
        handshake.capabilities = {"describe"};

        auto attached = ExtensionBridge::attach(dispatcher, handshake, policy);
        CHECK(std::holds_alternative<ExtensionBridge>(attached));
        const ExtensionBridge& shim = std::get<ExtensionBridge>(attached);
        CHECK(shim.session().attached);
        // the shim's session holds only the read/query baseline
        CHECK(!shim.session().scopes.has(bridge::Scope::build_install));

        // a read/query verb succeeds through the capability-scoped bridge
        const cli::Envelope described = shim.invoke("describe", cli::Json::object());
        CHECK(described.ok());

        // a build/install verb is DENIED at the dispatcher (R-SEC-007) — no ambient elevation
        const cli::Envelope install = shim.invoke("package.add", cli::Json::object());
        CHECK(!install.ok());
        CHECK(install.error().has_value());
        CHECK(install.error()->code == bridge::kScopeDeniedCode);
    }

    // --- an explicitly-granted higher scope flows through (file-write NOT scope-denied) ----------
    {
        bridge::Dispatcher dispatcher;
        SandboxPolicy policy;
        bridge::ScopeSet scopes = bridge::ScopeSet::read_query();
        scopes.grant(bridge::Scope::file_write);
        policy.granted_scopes = scopes;

        cli::ClientHandshake handshake;
        handshake.protocol_major = cli::kProtocolMajor;

        auto attached = ExtensionBridge::attach(dispatcher, handshake, policy);
        CHECK(std::holds_alternative<ExtensionBridge>(attached));
        const ExtensionBridge& shim = std::get<ExtensionBridge>(attached);
        CHECK(shim.session().scopes.has(bridge::Scope::file_write));

        // a file-write verb passes the scope gate (its backing is unimplemented at M1, but the point
        // is it is NOT scope-denied — the grant flowed through).
        const cli::Envelope set = shim.invoke("set", cli::Json::object());
        if (set.error().has_value())
        {
            CHECK(set.error()->code != bridge::kScopeDeniedCode);
        }
    }

    // --- attach hard-fails on an out-of-window protocol -----------------------------------------
    {
        bridge::Dispatcher dispatcher;
        SandboxPolicy policy;
        cli::ClientHandshake handshake;
        handshake.protocol_major = cli::kProtocolMajor + 1; // outside {kProtocolMajor}

        auto attached = ExtensionBridge::attach(dispatcher, handshake, policy);
        CHECK(std::holds_alternative<cli::Envelope>(attached));
        const cli::Envelope& env = std::get<cli::Envelope>(attached);
        CHECK(!env.ok());
        CHECK(env.error().has_value());
        CHECK(env.error()->code == "handshake.incompatible_protocol");
    }

    GUI_CONTRACT_TEST_MAIN_END();
}

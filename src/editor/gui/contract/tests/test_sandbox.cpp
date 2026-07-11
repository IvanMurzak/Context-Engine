// R-EDIT-001 sandbox-policy tests: the default is least privilege + fully conformant; each
// non-negotiable field, when relaxed, makes the policy non-conformant.

#include "context/editor/gui/contract/sandbox.h"

#include "context/editor/bridge/scope.h"

#include "contract_test.h"

#include <string>

using namespace context::editor::gui::contract;

int main()
{
    // --- the default policy: least privilege (read/query only) + conformant --------------------
    {
        SandboxPolicy p;
        CHECK(p.node_integration == false);
        CHECK(p.isolated_renderer);
        CHECK(p.sandboxed_iframe);
        CHECK(p.daemon_socket_access == false);
        CHECK(!p.csp.empty());
        CHECK(std::string(p.csp) == std::string(kDefaultExtensionCsp));
        // default grant = R-SEC-007 read/query baseline; NOT file-write/build/install
        CHECK(!p.granted_scopes.has(context::editor::bridge::Scope::file_write));
        CHECK(!p.granted_scopes.has(context::editor::bridge::Scope::build_install));
        CHECK(!p.granted_scopes.has(context::editor::bridge::Scope::session_control));
        CHECK(p.granted_scopes.has(context::editor::bridge::Scope::read_query)); // baseline always held
        CHECK(sandbox_conformant(p));
    }

    // --- each relaxation breaks conformance -----------------------------------------------------
    {
        SandboxPolicy p;
        p.node_integration = true; // native-addon access must stay off
        CHECK(!sandbox_conformant(p));
    }
    {
        SandboxPolicy p;
        p.isolated_renderer = false;
        CHECK(!sandbox_conformant(p));
    }
    {
        SandboxPolicy p;
        p.sandboxed_iframe = false;
        CHECK(!sandbox_conformant(p));
    }
    {
        SandboxPolicy p;
        p.daemon_socket_access = true; // extensions get no direct socket/token
        CHECK(!sandbox_conformant(p));
    }
    {
        SandboxPolicy p;
        p.csp.clear();
        CHECK(!sandbox_conformant(p));
    }

    // --- an explicitly-granted higher scope is orthogonal to conformance ------------------------
    {
        SandboxPolicy p;
        context::editor::bridge::ScopeSet scopes = context::editor::bridge::ScopeSet::read_query();
        scopes.grant(context::editor::bridge::Scope::file_write);
        p.granted_scopes = scopes;
        // a higher grant is a legitimate authorized choice; conformance is about the renderer boundary
        CHECK(sandbox_conformant(p));
        CHECK(p.granted_scopes.has(context::editor::bridge::Scope::file_write));
    }

    GUI_CONTRACT_TEST_MAIN_END();
}

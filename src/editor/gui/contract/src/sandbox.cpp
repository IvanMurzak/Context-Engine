// R-EDIT-001 CEF extension sandbox policy: the conformance predicate.

#include "context/editor/gui/contract/sandbox.h"

namespace context::editor::gui::contract
{

bool sandbox_conformant(const SandboxPolicy& policy)
{
    return policy.node_integration == false && policy.isolated_renderer && policy.sandboxed_iframe &&
           !policy.daemon_socket_access && !policy.csp.empty();
}

} // namespace context::editor::gui::contract

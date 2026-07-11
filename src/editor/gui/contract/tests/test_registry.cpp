// R-EDIT-001 extension-registry tests: register/resolve, deny-by-default on the two contract
// invariants (out-of-window version, non-conformant sandbox), and unique ids.

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/contract/sandbox.h"

#include "contract_test.h"

#include <string>

using namespace context::editor::gui::contract;

namespace
{

Contribution make_panel(std::string id)
{
    Contribution c;
    c.id = std::move(id);
    c.kind = ContributionKind::panel;
    c.title = "Panel";
    // contract_version + sandbox default to conformant values
    return c;
}

} // namespace

int main()
{
    // --- happy path: register + find + resolve --------------------------------------------------
    {
        ExtensionRegistry reg;
        RegistrationResult r = reg.register_contribution(make_panel("builtin.problems"));
        CHECK(r.ok);
        CHECK(reg.size() == 1);
        CHECK(reg.contains("builtin.problems"));
        CHECK(reg.find("builtin.problems") != nullptr);
        CHECK(reg.find("nope") == nullptr);

        // an inspector keyed by a component type resolves by (kind, target)
        Contribution insp;
        insp.id = "builtin.transform-inspector";
        insp.kind = ContributionKind::inspector;
        insp.target = "core.Transform";
        insp.title = "Transform";
        CHECK(reg.register_contribution(insp).ok);
        const Contribution* found = reg.resolve(ContributionKind::inspector, "core.Transform");
        CHECK(found != nullptr);
        CHECK(found->id == "builtin.transform-inspector");
        CHECK(reg.resolve(ContributionKind::inspector, "core.Missing") == nullptr);
        CHECK(reg.by_kind(ContributionKind::inspector).size() == 1);
        CHECK(reg.by_kind(ContributionKind::panel).size() == 1);
    }

    // --- deny: out-of-window contract version ---------------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = make_panel("bad.version");
        c.contract_version = kContractMajor + 1;
        RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrUnsupportedContractVersion);
        CHECK(reg.size() == 0); // registry unchanged on refusal
    }

    // --- deny: non-conformant sandbox -----------------------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = make_panel("bad.sandbox");
        c.sandbox.node_integration = true; // breaks conformance
        RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrSandboxNonconformant);
        CHECK(reg.size() == 0);
    }

    // --- deny: duplicate id ---------------------------------------------------------------------
    {
        ExtensionRegistry reg;
        CHECK(reg.register_contribution(make_panel("dup")).ok);
        RegistrationResult r = reg.register_contribution(make_panel("dup"));
        CHECK(!r.ok);
        CHECK(r.error_code == kErrDuplicateContribution);
        CHECK(reg.size() == 1);
    }

    // --- contribution-kind tokens ---------------------------------------------------------------
    {
        CHECK(std::string(contribution_kind_token(ContributionKind::panel)) == "panel");
        CHECK(std::string(contribution_kind_token(ContributionKind::inspector)) == "inspector");
        CHECK(std::string(contribution_kind_token(ContributionKind::gizmo)) == "gizmo");
        CHECK(std::string(contribution_kind_token(ContributionKind::asset_kind_editor)) ==
              "asset-kind-editor");
    }

    GUI_CONTRACT_TEST_MAIN_END();
}

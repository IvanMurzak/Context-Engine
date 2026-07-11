// R-EDIT-001 extension registry implementation.

#include "context/editor/gui/contract/registry.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/sandbox.h"

#include <string>
#include <vector>

namespace context::editor::gui::contract
{

RegistrationResult ExtensionRegistry::register_contribution(Contribution contribution)
{
    if (contribution.contract_version != kContractMajor)
    {
        return RegistrationResult::failure(
            kErrUnsupportedContractVersion,
            "contribution \"" + contribution.id + "\" declares contract major " +
                std::to_string(contribution.contract_version) + " outside the supported window {" +
                std::to_string(kContractMajor) + "}");
    }
    if (!sandbox_conformant(contribution.sandbox))
    {
        return RegistrationResult::failure(
            kErrSandboxNonconformant,
            "contribution \"" + contribution.id +
                "\" has a non-conformant renderer sandbox (node integration must be off; isolated "
                "renderer + sandboxed iframe on; no daemon-socket access; non-empty CSP)");
    }
    if (contains(contribution.id))
    {
        return RegistrationResult::failure(
            kErrDuplicateContribution,
            "a contribution with id \"" + contribution.id + "\" is already registered");
    }
    contributions_.push_back(std::move(contribution));
    return RegistrationResult::success();
}

bool ExtensionRegistry::contains(const std::string& id) const
{
    return find(id) != nullptr;
}

const Contribution* ExtensionRegistry::find(const std::string& id) const
{
    for (const Contribution& c : contributions_)
    {
        if (c.id == id)
        {
            return &c;
        }
    }
    return nullptr;
}

const Contribution* ExtensionRegistry::resolve(ContributionKind kind, const std::string& target) const
{
    for (const Contribution& c : contributions_)
    {
        if (c.kind == kind && c.target == target)
        {
            return &c;
        }
    }
    return nullptr;
}

std::vector<const Contribution*> ExtensionRegistry::by_kind(ContributionKind kind) const
{
    std::vector<const Contribution*> out;
    for (const Contribution& c : contributions_)
    {
        if (c.kind == kind)
        {
            out.push_back(&c);
        }
    }
    return out;
}

} // namespace context::editor::gui::contract

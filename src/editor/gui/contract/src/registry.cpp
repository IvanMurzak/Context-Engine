// R-EDIT-001 extension registry implementation.

#include "context/editor/gui/contract/registry.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/sandbox.h"

#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::gui::contract
{

namespace
{

// The manifest-v2 structural invariants (04 §3). Returns the reason a manifest is invalid, or an
// empty string when it is well-formed. Deny-by-default: a manifest that cannot be rendered coherently
// is refused at registration rather than half-honoured at panel-open time.
std::string manifest_defect(const Contribution& c)
{
    if (c.id.empty())
    {
        return "the contribution id is empty (ids are the registry's primary key)";
    }
    if (c.content.type == ContentType::iframe && c.content.entry.empty())
    {
        return "content.type is \"iframe\" but content.entry names no URL to load";
    }
    if (c.content.type == ContentType::uitree && !c.content.entry.empty())
    {
        return "content.type is \"uitree\" (the panel model IS the content) but content.entry is "
               "set to \"" +
               c.content.entry + "\"";
    }
    if (c.state.schema_version == 0)
    {
        return "state.schemaVersion is 0 — a persisted D6 blob must carry a version >= 1";
    }
    if (c.dock.min_width < 0 || c.dock.min_height < 0)
    {
        return "dock.minSize is negative (" + std::to_string(c.dock.min_width) + ", " +
               std::to_string(c.dock.min_height) + ")";
    }
    for (std::size_t i = 0; i < c.commands.size(); ++i)
    {
        if (c.commands[i].id.empty())
        {
            return "manifest command #" + std::to_string(i) + " has an empty id";
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (c.commands[j].id == c.commands[i].id)
            {
                return "manifest command id \"" + c.commands[i].id + "\" is declared twice";
            }
        }
    }
    return {};
}

} // namespace

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
    if (const std::string defect = manifest_defect(contribution); !defect.empty())
    {
        return RegistrationResult::failure(
            kErrInvalidManifest,
            "contribution \"" + contribution.id + "\" has an invalid manifest: " + defect);
    }
    for (const std::string& capability : contribution.capabilities)
    {
        if (!capability_supported(capability))
        {
            return RegistrationResult::failure(
                kErrUnknownCapability,
                "contribution \"" + contribution.id + "\" requests the unknown capability \"" +
                    capability + "\" (the manifest vocabulary is closed — see extension.h)");
        }
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

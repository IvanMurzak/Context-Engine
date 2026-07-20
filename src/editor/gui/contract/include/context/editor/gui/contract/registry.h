// R-EDIT-001 extension registry (issue #152): the versioned registry a package (or a built-in)
// registers editor-UI contributions into. Deny-by-default on the two contract invariants — an
// out-of-window contract version and a non-conformant renderer sandbox are REFUSED — and ids are
// unique. Inspectors/gizmos/asset-kind editors are resolvable by their target so a panel host can ask
// "which inspector renders this component type?".
//
// The result type is a bespoke RegistrationResult with grep-stable local codes rather than the frozen
// R-CLI-008 error catalog: F0b introduces NO new CLI catalog code (protocolMajor unchanged; the
// contract-freeze gate stays green).

#pragma once

#include "context/editor/gui/contract/extension.h"

#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::gui::contract
{

// Grep-stable local refusal codes (NOT R-CLI-008 catalog codes — see the file header).
inline constexpr const char* kErrDuplicateContribution = "gui.duplicate_contribution";
inline constexpr const char* kErrUnsupportedContractVersion = "gui.unsupported_contract_version";
inline constexpr const char* kErrSandboxNonconformant = "gui.sandbox_nonconformant";
// M9 e05b (manifest v2): the v2 members carry their own deny-by-default invariants — a structurally
// invalid manifest, or a capability outside the closed vocabulary (extension.h).
inline constexpr const char* kErrInvalidManifest = "gui.invalid_manifest";
inline constexpr const char* kErrUnknownCapability = "gui.unknown_capability";

struct RegistrationResult
{
    bool ok = false;
    std::string error_code; // empty when ok
    std::string message;

    [[nodiscard]] static RegistrationResult success() { return RegistrationResult{true, "", ""}; }
    [[nodiscard]] static RegistrationResult failure(std::string code, std::string message)
    {
        return RegistrationResult{false, std::move(code), std::move(message)};
    }
};

class ExtensionRegistry
{
public:
    // Register a contribution. Refuses (returns a failure result, leaving the registry unchanged) an
    // out-of-window contract_version (kErrUnsupportedContractVersion), a non-conformant sandbox
    // policy (kErrSandboxNonconformant), a structurally invalid manifest (kErrInvalidManifest), an
    // unknown capability token (kErrUnknownCapability), or a duplicate id
    // (kErrDuplicateContribution). Refusal precedence is that order — the two original invariants are
    // still reported first, so an existing caller's diagnostics are unchanged.
    RegistrationResult register_contribution(Contribution contribution);

    [[nodiscard]] bool contains(const std::string& id) const;
    [[nodiscard]] const Contribution* find(const std::string& id) const;

    // Resolve the contribution of `kind` whose `target` matches (e.g. the inspector for a component
    // type). Returns nullptr when none is registered. `target` is ignored for free-floating panels.
    [[nodiscard]] const Contribution* resolve(ContributionKind kind, const std::string& target) const;

    [[nodiscard]] std::vector<const Contribution*> by_kind(ContributionKind kind) const;
    [[nodiscard]] std::size_t size() const noexcept { return contributions_.size(); }
    [[nodiscard]] const std::vector<Contribution>& all() const noexcept { return contributions_; }

private:
    std::vector<Contribution> contributions_;
};

} // namespace context::editor::gui::contract

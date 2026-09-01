// R-EDIT-001 extension registry implementation.

#include "context/editor/gui/contract/registry.h"

#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/sandbox.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::gui::contract
{

namespace
{

// The manifest capability token that authorizes a bridge scope, or nullptr for the always-held
// R-SEC-007 baseline: read_query needs no declaration (ScopeSet::has reports it unconditionally), so
// demanding one would refuse every contribution that declares no capabilities at all.
//
// A switch with NO default clause on purpose. The grant-vs-declaration check below is only as complete
// as this mapping, so a new bridge::Scope enumerator must break THIS build (-Wswitch under -Werror /
// MSVC C4062) rather than sail through as a scope no manifest is ever required to declare — a check
// that silently fails open is worse than no check. Same grep-stable token-table idiom as
// contribution_kind_token / dock_zone_token in extension.cpp.
const char* manifest_capability_for(bridge::Scope scope)
{
    switch (scope)
    {
    case bridge::Scope::read_query:
        return nullptr;
    case bridge::Scope::file_write:
        return kCapabilityFileWrite;
    case bridge::Scope::session_control:
        return kCapabilitySessionControl;
    case bridge::Scope::build_install:
        return kCapabilityBuildInstall;
    }
    return nullptr;
}

// Every scope the grant check walks, paired with the switch above: extending the vocabulary breaks
// that switch, and the compiler error lands directly beside this list.
constexpr bridge::Scope kGrantableScopes[] = {
    bridge::Scope::read_query,
    bridge::Scope::file_write,
    bridge::Scope::session_control,
    bridge::Scope::build_install,
};

bool declares_capability(const Contribution& c, const char* token)
{
    return std::find(c.capabilities.begin(), c.capabilities.end(), token) != c.capabilities.end();
}

// --- the manifest-v3 name grammar (04 §2) ---------------------------------------------------------
// Lowercase dot-separated segments of `[a-z0-9][a-z0-9-]*` — the SAME grammar `validatePackageTopic`
// (uibus.ts) and `validatePackageCommandId` (panelverbs.ts) already apply on the renderer side, which
// is the discipline 04 §2 names. It is re-stated in C++ rather than mirrored from there because the
// two guard different doors: those guard the window-local tier-2 bus, this guards ADMISSION to the
// registry, and a name refused here never reaches a bus at all.

// `editor` is the editor's own namespace (uibus.ts's RESERVED_PACKAGE_IDS, and the prefix under which
// the closed nine-member `editor.ui` topic set lives). A package may declare nothing inside it.
constexpr const char* kReservedNamespace = "editor";

bool is_name_segment(const std::string& segment)
{
    if (segment.empty())
    {
        return false;
    }
    const auto first = static_cast<unsigned char>(segment.front());
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9')))
    {
        return false;
    }
    for (const char ch : segment)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
        {
            return false;
        }
    }
    return true;
}

// Is `name` a well-formed dotted name? A leading dot, a trailing dot and a doubled dot all produce an
// EMPTY segment, which `is_name_segment` refuses — so the three do not need their own cases.
bool is_segmented_name(const std::string& name)
{
    if (name.empty())
    {
        return false;
    }
    std::size_t start = 0;
    while (true)
    {
        const std::size_t dot = name.find('.', start);
        const std::string segment =
            name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!is_name_segment(segment))
        {
            return false;
        }
        if (dot == std::string::npos)
        {
            return true;
        }
        start = dot + 1;
    }
}

// Is `name` a real SUB-name of `owner` — `<owner>.<something>`, never the bare owner id? Both halves
// matter, exactly as validatePackageTopic states: the bare package id is a namespace, not a member of
// it, and accepting it would let one package's topic and its id be the same string.
bool is_namespaced_under(const std::string& name, const std::string& owner)
{
    return name.size() > owner.size() + 1 && name.compare(0, owner.size(), owner) == 0 &&
           name[owner.size()] == '.';
}

bool is_reserved_name(const std::string& name)
{
    return name == kReservedNamespace || is_namespaced_under(name, kReservedNamespace);
}

// The v3 `instances` invariants (04 §2): `max` is meaningful ONLY for `limited`. Both directions are
// REFUSED rather than repaired — a `limited` with no ceiling has not said what it asked for, and a
// `max` on a mode that cannot use it is a statement the registry would otherwise silently discard.
std::string instances_defect(const InstanceSpec& instances)
{
    const std::string mode = instance_mode_token(instances.mode);
    if (instances.mode == InstanceMode::limited)
    {
        if (instances.max <= 0)
        {
            return "instances.mode is \"limited\" but instances.max is " +
                   std::to_string(instances.max) +
                   " — a limited panel must state a positive maximum";
        }
        return {};
    }
    if (instances.max != 0)
    {
        return "instances.max is " + std::to_string(instances.max) + " but instances.mode is \"" +
               mode + "\" — max is meaningful only for \"limited\"";
    }
    return {};
}

// The v3 `path` invariants (04 §2). `path` is slash-separated DISPLAY text for the Window menu's
// panel tree (d1), not a filesystem path: nothing resolves it, so the rules are exactly the ones that
// keep the tree renderable — empty means top level, and no segment may be blank.
std::string path_defect(const std::string& path)
{
    if (path.empty())
    {
        return {}; // top level
    }
    if (path.front() == '/' || path.back() == '/')
    {
        return "path \"" + path +
               "\" has a leading or trailing \"/\" (it is display text, not a filesystem path; "
               "empty means top level)";
    }
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = path.find('/', start);
        const std::string segment =
            path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        // A segment of only blanks is an EMPTY LABEL, which renders as a nameless menu row — the same
        // defect as `//` with one more space in it, so it is refused by the same rule rather than by
        // a separate one nobody would think to look for.
        if (segment.find_first_not_of(" \t") == std::string::npos)
        {
            return "path \"" + path + "\" has an empty segment";
        }
        if (slash == std::string::npos)
        {
            return {};
        }
        start = slash + 1;
    }
}

// The v3 namespacing invariants for one declared-name list (`selection.subjects`, `events.publishes`,
// `events.subscribes`), with the discipline of validatePackageTopic / validatePackageCommandId.
//
// `owned` says WHOSE namespace the name must sit in. It is true for the two lists that declare what
// this contribution PRODUCES — a selection subject it defines, a topic it publishes — because those
// are claims about its own vocabulary. It is false for `events.subscribes`, and that asymmetry is the
// whole point of D4: subscribing is exactly where a package names ANOTHER package's topic (the
// install-time consented, deny-by-default grant), so the rule there is "namespaced under SOMEBODY",
// never "namespaced under you".
//
// A BUILT-IN (empty `package_id`) is held only to the grammar: the editor's own contributions may name
// the contract-owned kinds (`entity`, `file`, `asset`) unnamespaced, which is precisely the right a
// package does not have.
std::string names_defect(const Contribution& c, const std::vector<std::string>& names,
                         const char* member, bool owned)
{
    // THE DECLARING ID IS CHECKED HERE, NOT UP IN manifest_defect, and only when there is something to
    // namespace — a deliberately narrow scope. `shell::is_valid_package_id` is LOOSER than this
    // grammar (it admits `_`, which no topic segment may contain), so a blanket check would newly
    // refuse to INSTALL an underscore-named package that declares no v3 names at all, turning a
    // vocabulary rule into an install regression. Scoped this way, such a package installs exactly as
    // it does today and is told precisely why it cannot name a subject or a topic. It also keeps every
    // diagnostic below honest: each quotes the namespace, so a malformed one must be reported as ITS
    // own fault rather than as the entry's.
    if (!names.empty() && !c.package_id.empty() && !is_segmented_name(c.package_id))
    {
        return std::string(member) + " cannot be namespaced: the declaring package id \"" +
               c.package_id + "\" is not a valid namespace (lowercase dotted segments)";
    }
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        const std::string& name = names[i];
        if (!is_segmented_name(name))
        {
            return std::string(member) + " entry \"" + name +
                   "\" is not a valid name (lowercase dotted segments)";
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (names[j] == name)
            {
                return std::string(member) + " declares \"" + name + "\" twice";
            }
        }
        if (c.package_id.empty())
        {
            continue; // a built-in: the contract owns the unnamespaced vocabulary
        }
        if (is_reserved_name(name))
        {
            return std::string(member) + " entry \"" + name + "\" is inside the reserved \"" +
                   kReservedNamespace + "\" namespace";
        }
        if (owned)
        {
            if (!is_namespaced_under(name, c.package_id))
            {
                return std::string(member) + " entry \"" + name +
                       "\" is not namespaced under its declaring package \"" + c.package_id + "\"";
            }
        }
        else if (name.find('.') == std::string::npos)
        {
            return std::string(member) + " entry \"" + name +
                   "\" is not namespaced under any package (a package may subscribe to another "
                   "package's topic, but never to an unnamespaced contract-owned name)";
        }
    }
    return {};
}

// The manifest-v2 structural invariants (04 §3) plus the manifest-v3 ones (04 §2). Returns the reason
// a manifest is invalid, or an empty string when it is well-formed. Deny-by-default: a manifest that
// cannot be rendered coherently is refused at registration rather than half-honoured at panel-open
// time.
std::string manifest_defect_impl(const Contribution& c)
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
    if (c.content.type == ContentType::local && !c.content.entry.empty())
    {
        // Same rule as `uitree`, for the same reason: there is nothing to LOAD. A `local` panel is
        // rendered by editor-core from the kit, so an `entry` here could only be a URL nothing
        // fetches — silently ignored config is how a manifest starts lying about what it does.
        return "content.type is \"local\" (editor-core renders the panel) but content.entry is set "
               "to \"" +
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
    // --- manifest v3 (04 §2) ----------------------------------------------------------------------
    if (const std::string defect = instances_defect(c.instances); !defect.empty())
    {
        return defect;
    }
    if (const std::string defect = path_defect(c.path); !defect.empty())
    {
        return defect;
    }
    if (const std::string defect =
            names_defect(c, c.selection.subjects, "selection.subjects", true);
        !defect.empty())
    {
        return defect;
    }
    if (const std::string defect = names_defect(c, c.events.publishes, "events.publishes", true);
        !defect.empty())
    {
        return defect;
    }
    if (const std::string defect = names_defect(c, c.events.subscribes, "events.subscribes", false);
        !defect.empty())
    {
        return defect;
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
    // The manifest `capabilities` list is what a contribution ASKS for; `sandbox.granted_scopes` is
    // what it actually HOLDS on the bridge (shim.cpp attaches the session with exactly that set). A
    // grant the manifest never declared is precisely the AMBIENT privilege R-SEC-007 forbids — and it
    // fails OPEN, since a reviewer reading the manifest would never see it. Refuse the mismatch here,
    // at the one registration choke point, so the declaration is an upper bound on the grant rather
    // than decoration. (Direction matters: declaring MORE than is granted stays legal — that is a
    // contribution asking for a capability the operator has not yet extended.)
    for (const bridge::Scope scope : kGrantableScopes)
    {
        const char* token = manifest_capability_for(scope);
        if (token == nullptr)
        {
            continue; // the read/query baseline is held by every contribution, declared or not
        }
        if (c.sandbox.granted_scopes.has(scope) && !declares_capability(c, token))
        {
            return "the sandbox grants \"" + std::string(token) +
                   "\" but the manifest capabilities do not declare it (a grant may never exceed the "
                   "declared manifest)";
        }
    }
    return {};
}

} // namespace

std::string manifest_defect(const Contribution& contribution)
{
    return manifest_defect_impl(contribution);
}

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
    if (const std::string defect = manifest_defect_impl(contribution); !defect.empty())
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

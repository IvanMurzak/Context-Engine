// The built-in roster + panel-manifest-v2 tests (M9 e05b, R-EDIT-001 / design 04 §3): the single
// global roster registers cleanly under deny-by-default, every built-in declares a well-formed v2
// manifest, and the new v2 invariants (content/state/dock/commands/capabilities) are each REFUSED
// when violated — happy path, edge cases, and failure paths (R-QA-013).

#include "context/editor/gui/contract/builtin_roster.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/contract/registry.h"

#include "contract_test.h"

#include <set>
#include <string>

using namespace context::editor::gui::contract;

namespace
{

// A minimal, valid v2 panel manifest — the base every failure-path case below mutates ONE field of.
Contribution valid_panel(std::string id)
{
    Contribution c;
    c.id = std::move(id);
    c.kind = ContributionKind::panel;
    c.title = "Panel";
    // contract_version, sandbox, content (uitree), state (v1), dock all default to valid values.
    return c;
}

} // namespace

int main()
{
    // --- the contract major is v2 (the M9 e05b manifest break) -----------------------------------
    {
        CHECK(kContractMajor == 2);
        // A default-constructed Contribution declares the CURRENT major, so every in-repo caller that
        // does not set contract_version explicitly moved with the bump.
        CHECK(Contribution{}.contract_version == kContractMajor);
    }

    // --- the roster is non-empty, duplicate-free, and REGISTERS under deny-by-default -------------
    {
        bool all_ok = false;
        const ExtensionRegistry registry = make_builtin_registry(&all_ok);
        CHECK(all_ok); // every built-in satisfies every contract invariant it is itself subject to
        CHECK(!builtin_contributions().empty());
        CHECK(registry.size() == builtin_contributions().size());

        std::set<std::string> ids;
        for (const Contribution& c : builtin_contributions())
        {
            CHECK(ids.insert(c.id).second); // a duplicate roster id is itself a defect
            CHECK(!c.id.empty());
            CHECK(!c.title.empty());
            CHECK(c.contract_version == kContractMajor);
            // Every built-in is a headless uitree panel (its C++ model IS the content).
            CHECK(c.content.type == ContentType::uitree);
            CHECK(c.content.entry.empty());
            // D6: every panel declares a state schema version (>= 1) — "state contract on EVERY panel".
            CHECK(c.state.schema_version >= 1);
            CHECK(c.dock.min_width >= 0 && c.dock.min_height >= 0);
            CHECK(sandbox_conformant(c.sandbox));
            // Every declared capability is on the closed vocabulary.
            for (const std::string& cap : c.capabilities)
            {
                CHECK(capability_supported(cap));
            }
            // Each is findable in the roster registry by id.
            CHECK(registry.find(c.id) != nullptr);
        }

        // A-F2: the session-undo surface is ON the roster (it was absent from BOTH anchors pre-e05b).
        CHECK(ids.count("builtin.session.undo") == 1);
        // The panels the M5 exit gate names are still there after the promotion.
        for (const char* id : {"placeholder", "builtin.scene-tree", "builtin.inspector",
                               "builtin.viewport", "builtin.playbar", "builtin.problems"})
        {
            CHECK(ids.count(id) == 1);
        }
    }

    // --- the roster is a stable, shared instance (built once, same order every call) --------------
    {
        CHECK(&builtin_contributions() == &builtin_contributions());
        CHECK(builtin_contributions().front().id == "placeholder");
    }

    // --- manifest v2 accepts a fully-populated iframe contribution --------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("ext.hello-panel");
        c.icon = "sparkle";
        c.dock.default_zone = DockZone::right;
        c.dock.singleton = true;
        c.dock.min_width = 280;
        c.dock.min_height = 200;
        c.content.type = ContentType::iframe;
        c.content.entry = "context-ext://hello/panel.html";
        c.state.schema_version = 3;
        c.capabilities = {kCapabilityReadQuery, kCapabilityUiEvents};
        c.commands = {CommandContribution{"hello.greet", "Greet", "panelFocus == hello"},
                      CommandContribution{"hello.wave", "Wave", ""}};
        c.themes = {"themes/hello.theme.json"};
        CHECK(reg.register_contribution(c).ok);

        const Contribution* got = reg.find("ext.hello-panel");
        CHECK(got != nullptr);
        CHECK(got->content.type == ContentType::iframe);
        CHECK(got->content.entry == "context-ext://hello/panel.html");
        CHECK(got->state.schema_version == 3);
        CHECK(got->dock.default_zone == DockZone::right);
        CHECK(got->dock.singleton);
        CHECK(got->capabilities.size() == 2);
        CHECK(got->commands.size() == 2);
        CHECK(got->commands[0].when == "panelFocus == hello");
        CHECK(got->themes.size() == 1);
        CHECK(got->icon == "sparkle");
    }

    // --- deny: an iframe contribution with no entry URL -------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.iframe-no-entry");
        c.content.type = ContentType::iframe; // entry left empty
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
        CHECK(reg.size() == 0); // registry unchanged on refusal
    }

    // --- deny: a uitree contribution that ALSO names an entry URL ---------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.uitree-with-entry");
        c.content.entry = "context-ext://sneaky/panel.html";
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrInvalidManifest);
    }

    // --- deny: a zero state schema version (a persisted D6 blob could never match) -----------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.state-v0");
        c.state.schema_version = 0;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);
    }

    // --- deny: a negative dock minimum size --------------------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.dock");
        c.dock.min_height = -1;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);
    }

    // --- deny: an empty id -------------------------------------------------------------------------
    {
        ExtensionRegistry reg;
        CHECK(reg.register_contribution(valid_panel("")).error_code == kErrInvalidManifest);
    }

    // --- deny: an empty or duplicated manifest command id ------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution empty_cmd = valid_panel("bad.command-empty");
        empty_cmd.commands = {CommandContribution{"", "No id", ""}};
        CHECK(reg.register_contribution(empty_cmd).error_code == kErrInvalidManifest);

        Contribution dup_cmd = valid_panel("bad.command-dup");
        dup_cmd.commands = {CommandContribution{"a.b", "First", ""},
                            CommandContribution{"a.b", "Second", ""}};
        CHECK(reg.register_contribution(dup_cmd).error_code == kErrInvalidManifest);
        CHECK(reg.size() == 0);
    }

    // --- deny: a capability outside the closed vocabulary ------------------------------------------
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.capability");
        c.capabilities = {kCapabilityReadQuery, "root_access"};
        const RegistrationResult r = reg.register_contribution(c);
        CHECK(!r.ok);
        CHECK(r.error_code == kErrUnknownCapability);
        CHECK(reg.size() == 0);
        // An unknown capability is REFUSED, never silently dropped to a weaker grant.
        CHECK(!capability_supported("root_access"));
        CHECK(!capability_supported(""));
        CHECK(!capability_supported("READ_QUERY")); // the vocabulary is case-SENSITIVE
        CHECK(capability_supported(kCapabilityReadQuery));
        CHECK(capability_supported(kCapabilityFileWrite));
        CHECK(capability_supported(kCapabilitySessionControl));
        CHECK(capability_supported(kCapabilityBuildInstall));
        CHECK(capability_supported(kCapabilityUiEvents));
    }

    // --- refusal precedence: the two ORIGINAL invariants still report first ------------------------
    // (an existing caller's diagnostics are unchanged by the new manifest checks)
    {
        ExtensionRegistry reg;
        Contribution c = valid_panel("bad.everything");
        c.contract_version = kContractMajor + 1;
        c.sandbox.node_integration = true;
        c.state.schema_version = 0;
        c.capabilities = {"root_access"};
        CHECK(reg.register_contribution(c).error_code == kErrUnsupportedContractVersion);

        c.contract_version = kContractMajor;
        CHECK(reg.register_contribution(c).error_code == kErrSandboxNonconformant);

        c.sandbox.node_integration = false;
        CHECK(reg.register_contribution(c).error_code == kErrInvalidManifest);

        c.state.schema_version = 1;
        CHECK(reg.register_contribution(c).error_code == kErrUnknownCapability);

        c.capabilities.clear();
        CHECK(reg.register_contribution(c).ok);
    }

    // --- the v2 token tables -----------------------------------------------------------------------
    {
        CHECK(std::string(dock_zone_token(DockZone::left)) == "left");
        CHECK(std::string(dock_zone_token(DockZone::right)) == "right");
        CHECK(std::string(dock_zone_token(DockZone::top)) == "top");
        CHECK(std::string(dock_zone_token(DockZone::bottom)) == "bottom");
        CHECK(std::string(dock_zone_token(DockZone::center)) == "center");
        CHECK(std::string(content_type_token(ContentType::uitree)) == "uitree");
        CHECK(std::string(content_type_token(ContentType::iframe)) == "iframe");
    }

    GUI_CONTRACT_TEST_MAIN_END();
}

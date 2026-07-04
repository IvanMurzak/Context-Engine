// Registry conformance tests — the M1 headline:
//   * CLI ≡ RPC ≡ MCP ≡ introspection PARITY (R-CLI-009): every registered verb projects onto all
//     four surfaces with identical identity + params + flags. A verb/flag added ONCE to registry
//     appears on every surface; a divergence fails the build (the R-CLI-009 CI proof).
//   * `context describe --json` is SCHEMA-VALIDATED (R-CLI-013): the emitted document re-parses and
//     carries every required section with the right shapes.
// (R-QA-013: happy + structural failure coverage.)

#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "contract_test.h"

#include <set>
#include <string>
#include <vector>

using namespace context::editor::contract;

namespace
{
std::vector<std::string> name_list(const Json& array)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < array.size(); ++i)
        out.push_back(array.at(i).as_string());
    return out;
}
} // namespace

int main()
{
    const Registry& reg = Registry::instance();
    const std::size_t n = reg.verbs().size();
    CHECK(n >= 4); // describe, new, set, package add

    const Json cli = reg.cli_surface();
    const Json rpc = reg.rpc_surface();
    const Json mcp = reg.mcp_surface();

    // --- every surface has exactly one entry per registered verb -------------------------------
    CHECK(cli.size() == n);
    CHECK(rpc.size() == n);
    CHECK(mcp.size() == n);

    const Json describe = reg.describe();
    const Json& contract = describe.at("contract");
    const Json& d_verbs = contract.at("verbs");
    const Json& d_rpc = contract.at("rpcMethods");
    const Json& d_mcp = contract.at("mcpTools");
    CHECK(d_verbs.size() == n);
    CHECK(d_rpc.size() == n);
    CHECK(d_mcp.size() == n);

    // --- PARITY: per verb, identity + params + flags line up across ALL surfaces ---------------
    std::set<std::string> rpc_methods;
    std::set<std::string> mcp_tools;
    for (std::size_t i = 0; i < n; ++i)
    {
        const VerbSpec& v = reg.verbs()[i];
        const Json& c = cli.at(i);
        const Json& r = rpc.at(i);
        const Json& m = mcp.at(i);
        const Json& dv = d_verbs.at(i);

        // identity: ns/noun/verb identical on every surface
        for (const Json* surface : {&c, &r, &m, &dv})
        {
            CHECK(surface->at("ns").as_string() == v.ns);
            CHECK(surface->at("noun").as_string() == v.noun);
            CHECK(surface->at("verb").as_string() == v.verb);
        }

        // stable method-id + tool name propagate (R-CLI-004 stable ids)
        CHECK(r.at("method").as_string() == v.rpc_method);
        CHECK(m.at("tool").as_string() == v.mcp_tool);
        CHECK(dv.at("rpcMethod").as_string() == v.rpc_method);
        CHECK(dv.at("mcpTool").as_string() == v.mcp_tool);
        CHECK(!v.rpc_method.empty());
        CHECK(!v.mcp_tool.empty());
        CHECK(rpc_methods.insert(v.rpc_method).second); // unique method-ids
        CHECK(mcp_tools.insert(v.mcp_tool).second);     // unique tool names

        // params: the SAME name list on cli, rpc, mcp (the "added once, everywhere" proof)
        const std::vector<std::string> cli_params = name_list(c.at("params"));
        CHECK(cli_params == name_list(r.at("params")));
        CHECK(cli_params == name_list(m.at("params")));
        CHECK(cli_params == name_list(m.at("inputSchema").at("params")));

        // flags: core flags + verb flags, identical on every surface
        const std::vector<std::string> cli_flags = name_list(c.at("flags"));
        CHECK(cli_flags == name_list(r.at("flags")));
        CHECK(cli_flags == name_list(m.at("flags")));
        CHECK(cli_flags == name_list(m.at("inputSchema").at("flags")));
        // the fixed core-flag set is present on every verb (R-CLI-007)
        CHECK(cli_flags.size() >= reg.core_flags().size());
    }

    // --- describe SCHEMA VALIDATION (R-CLI-013) ------------------------------------------------
    {
        // The emitted document re-parses (it is valid JSON).
        const Json parsed = Json::parse(describe.dump());
        const Json& c = parsed.at("contract");
        CHECK(c.is_object());

        // Required top-level sections, each with the right shape.
        CHECK(c.at("protocol").is_object());
        CHECK(c.at("protocol").at("protocolMajor").as_int() == 0);
        CHECK(c.at("protocol").at("capabilities").is_array());
        CHECK(c.at("coreFlags").is_array());
        CHECK(c.at("coreFlags").size() == reg.core_flags().size());
        CHECK(c.at("verbs").is_array());
        CHECK(c.at("rpcMethods").is_array());
        CHECK(c.at("mcpTools").is_array());
        CHECK(c.at("eventTopics").is_array());
        CHECK(c.at("eventTopics").size() >= 1);
        CHECK(c.at("errorCatalog").is_array());
        CHECK(c.at("errorCatalog").size() >= 1);
        // The file-kind schema section (R-CLI-005 / R-DATA-006, M2 wave 2): one entry per
        // registered kind — enumerated LIVE from the registry's registration set.
        const Json& kinds = c.at("fileKinds");
        CHECK(kinds.is_array());
        CHECK(kinds.size() == reg.file_kinds().size());
        CHECK(kinds.size() >= 2); // the engine kinds: ctx:scene + ctx:project
        bool saw_scene = false;
        for (std::size_t i = 0; i < kinds.size(); ++i)
        {
            const Json& k = kinds.at(i);
            CHECK(!k.at("id").as_string().empty());
            CHECK(k.at("version").as_int() >= 1);        // versioned publication
            CHECK(k.at("schema").is_object());           // the full published JSON Schema
            CHECK(k.at("fields").is_array());            // the derived per-field x-ctx-* index
            saw_scene = saw_scene || k.at("id").as_string() == "ctx:scene";
        }
        CHECK(saw_scene); // the M1 scene placeholder, migrated onto the mechanism
        // The units-law metadata reaches `describe` (R-DATA-006: x-ctx-units surfaced through
        // schema introspection so agents render/reason about units without guessing).
        const FileKindSpec* scene = reg.find_file_kind("ctx:scene");
        CHECK(scene != nullptr);
        CHECK(reg.find_file_kind("ctx:nope") == nullptr);
        bool saw_rad_units = false;
        const Json& scene_fields = scene->entry.at("fields");
        for (std::size_t i = 0; i < scene_fields.size(); ++i)
            saw_rad_units =
                saw_rad_units || scene_fields.at(i).at("units").as_string() == "rad";
        CHECK(saw_rad_units); // camera fov declares radians (never degrees)
        CHECK(c.at("componentTypes").is_array()); // reserved until R-LANG-010 lands

        // Each verb entry carries the full contract shape.
        for (std::size_t i = 0; i < c.at("verbs").size(); ++i)
        {
            const Json& v = c.at("verbs").at(i);
            CHECK(!v.at("verb").as_string().empty());
            CHECK(!v.at("command").as_string().empty());
            CHECK(!v.at("summary").as_string().empty());
            CHECK(!v.at("rpcMethod").as_string().empty());
            CHECK(!v.at("mcpTool").as_string().empty());
            CHECK(v.at("params").is_array());
            CHECK(v.at("flags").is_array());
        }

        // Each error-catalog entry carries code + exitCode + origin.
        for (std::size_t i = 0; i < c.at("errorCatalog").size(); ++i)
        {
            const Json& e = c.at("errorCatalog").at(i);
            CHECK(!e.at("code").as_string().empty());
            CHECK(e.at("exitCode").as_int() >= 1);
            CHECK(!e.at("origin").as_string().empty());
        }
    }

    // --- the CLI command strings reflect the grammar (global vs noun-scoped) --------------------
    {
        const VerbSpec* describe_verb = reg.find_verb("", "", "describe");
        CHECK(describe_verb != nullptr);
        CHECK(describe_verb->cli_command() == "context describe");
        const VerbSpec* pkg = reg.find_verb("", "package", "add");
        CHECK(pkg != nullptr);
        CHECK(pkg->cli_command() == "context package add");
        CHECK(pkg->rpc_method == "package.add");
        CHECK(pkg->mcp_tool == "context_package_add");
    }

    CONTRACT_TEST_MAIN_END();
}

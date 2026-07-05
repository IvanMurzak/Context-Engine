// Importer isolation v1 slice: jail read/write scoping, scrubbed env, honest OS-support reporting,
// and the run_isolated() policy gate (network + jail-escape refusals, and the happy-path audit).

#include "context/editor/import/isolated_runner.h"
#include "context/editor/import/sandbox.h"

#include "import_test.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace context::editor::import;

namespace
{
bool has_code(const std::vector<ImportDiagnostic>& diags, const char* code)
{
    for (const ImportDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}

// A trivial deterministic importer used only to exercise run_isolated()'s policy gate. Its import
// body is reached ONLY on the happy path — the network / jail-escape refusals short-circuit before it.
class StubImporter final : public Importer
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "stub"; }
    [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {".stub"}; }
    [[nodiscard]] std::uint32_t derived_format_version(ArtifactKind) const noexcept override
    {
        return 1;
    }
    [[nodiscard]] ImportResult import(const ImportInput& input) const override
    {
        ImportResult r;
        DerivedArtifact a;
        a.name = "stub";
        a.bytes = std::string(input.source_bytes);
        a.derived_format_version = 1;
        r.artifacts.push_back(std::move(a));
        r.ok = true;
        return r;
    }
};
} // namespace

int main()
{
    // R-SEC-010: the scrubbed environment is minimal + non-secret, NOT the parent env.
    {
        const auto env = scrubbed_environment();
        bool has_lang = false;
        for (const auto& [key, value] : env)
        {
            // No secret-shaped variable ever leaks into the importer child.
            CHECK(key.find("TOKEN") == std::string::npos);
            CHECK(key.find("SECRET") == std::string::npos);
            CHECK(key.find("KEY") == std::string::npos);
            if (key == "LANG")
                has_lang = true;
        }
        CHECK(has_lang); // C locale is pinned (determinism aid)
    }

    // R-SEC-008 / R-SEC-006: reads confined to the jail; writes confined to the output key.
    {
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/hero.png";
        policy.output_key = "/project/.cache/png/texture/abc";

        // Reads: anywhere inside the jail yes; outside no.
        CHECK(read_permitted(policy, "/project/assets/hero.png"));
        CHECK(read_permitted(policy, "/project/other/file"));
        CHECK(!read_permitted(policy, "/etc/passwd"));
        CHECK(!read_permitted(policy, "/elsewhere/secrets")); // outside the jail root

        // Writes: only under the output key.
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc"));
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc/blob"));
        CHECK(!write_permitted(policy, "/project/assets/hero.png")); // inside jail but not the key
        CHECK(!write_permitted(policy, "/etc/passwd"));              // outside jail
    }

    // Empty jail / output => nothing permitted (a malformed policy fails closed).
    {
        SandboxPolicy empty;
        CHECK(!read_permitted(empty, "/anything"));
        CHECK(!write_permitted(empty, "/anything"));
        SandboxPolicy no_out;
        no_out.jail_root = "/project";
        CHECK(read_permitted(no_out, "/project/x"));
        CHECK(!write_permitted(no_out, "/project/x")); // no output key -> no writes
    }

    // HONEST staging: a primitive is named, but v1 does NOT claim it is enforced in-process, and a
    // follow-up note is always present (never a silent assumption).
    {
        const OsSandboxSupport support = os_sandbox_support();
        CHECK(!support.primitive.empty());
        CHECK(!support.enforced);       // the subprocess/seccomp lockdown is the staged rollout
        CHECK(!support.follow_up.empty());
    }

    // run_isolated() enforces the policy CONTRACT the subprocess runner will later apply at the
    // syscall layer (R-SEC-006/008/010). Its two refusal branches ARE the isolation guarantee, so
    // they are covered here directly (the fuzz replay only ever exercises the well-formed policy).
    {
        const StubImporter stub;
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/in.stub";
        policy.output_key = "/project/.cache/stub/out";

        // Happy path: a jailed source imports, and the audit records EXACTLY the granted envelope
        // (input + output key, no network, and — honest staging — no OS primitive enforced in v1).
        {
            ImportInput in;
            in.source_path = policy.input_path;
            in.source_bytes = "payload";
            const IsolatedImport iso = run_isolated(stub, in, policy);
            CHECK(iso.result.ok);
            CHECK(iso.result.artifacts.size() == 1);
            CHECK(iso.audit.input_path == policy.input_path);
            CHECK(iso.audit.output_key == policy.output_key);
            CHECK(!iso.audit.network_allowed);
            CHECK(!iso.audit.os_primitive_enforced); // v1 never claims a lockdown it does not apply
        }

        // Network requested -> REFUSED as import.jail_escape (R-SEC-010: never a v1 default), and the
        // importer is never invoked (no artifacts).
        {
            ImportInput in;
            in.source_path = policy.input_path;
            in.source_bytes = "payload";
            SandboxPolicy net = policy;
            net.allow_network = true;
            const IsolatedImport iso = run_isolated(stub, in, net);
            CHECK(!iso.result.ok);
            CHECK(iso.result.artifacts.empty());
            CHECK(has_code(iso.result.diagnostics, "import.jail_escape"));
        }

        // A source path OUTSIDE the jail root -> REFUSED as import.jail_escape (R-SEC-008), never
        // imported. This is the "jail escape" path the CMake test comment promises is covered.
        {
            ImportInput in;
            in.source_path = "/etc/passwd"; // escapes the jail root
            in.source_bytes = "payload";
            const IsolatedImport iso = run_isolated(stub, in, policy);
            CHECK(!iso.result.ok);
            CHECK(iso.result.artifacts.empty());
            CHECK(has_code(iso.result.diagnostics, "import.jail_escape"));
        }
    }

    IMPORT_TEST_MAIN_END();
}

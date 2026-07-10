// Importer isolation slice: the narrowed read scope (owner ruling, issue #72), write scoping, scrubbed
// env, honest OS-support reporting, the portable subprocess result codec, the run_isolated() +
// run_subprocess() policy gates, and — on Linux — the real seccomp-bpf permit-vs-deny syscall filter.

#include "context/editor/import/isolated_runner.h"
#include "context/editor/import/sandbox.h"

#include "import_test.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

// A trivial deterministic importer used to exercise the runners' policy gates + the subprocess
// round-trip. Its import body echoes the source bytes back as a single artifact — reached only on the
// happy path (the network / jail-escape refusals short-circuit before it).
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

    // Read scope (OWNER RULING, issue #72): input-bytes-only, with a declared-read-paths escape hatch,
    // all ⊆ the jail. Writes confined to the output key.
    {
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/hero.png";
        policy.output_key = "/project/.cache/png/texture/abc";

        // Reads: the source bytes yes; an ARBITRARY in-jail path NO (the owner ruling NARROWED this
        // from the former jail-wide grant); outside the jail no.
        CHECK(read_permitted(policy, "/project/assets/hero.png")); // the input bytes
        CHECK(!read_permitted(policy, "/project/other/file"));     // in-jail but undeclared -> denied
        CHECK(!read_permitted(policy, "/etc/passwd"));
        CHECK(!read_permitted(policy, "/elsewhere/secrets")); // outside the jail root

        // Writes: only under the output key.
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc"));
        CHECK(write_permitted(policy, "/project/.cache/png/texture/abc/blob"));
        CHECK(!write_permitted(policy, "/project/assets/hero.png")); // inside jail but not the key
        CHECK(!write_permitted(policy, "/etc/passwd"));              // outside jail
    }

    // The declared-read-paths escape hatch (owner ruling, issue #72): an importer may DECLARE sibling
    // assets it needs; the grant is explicit + per-path + never widens past the jail.
    {
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/hero.gltf";

        // Baseline: only the input is readable.
        CHECK(read_permitted(policy, "/project/assets/hero.gltf"));
        CHECK(!read_permitted(policy, "/project/assets/hero.bin")); // undeclared sibling -> denied

        // Declare a sibling dir -> its subtree becomes readable, the input stays readable, an
        // UNdeclared sibling stays denied.
        SandboxPolicy declared = policy;
        declared.declared_read_paths.push_back("/project/assets/buffers");
        CHECK(read_permitted(declared, "/project/assets/buffers"));            // the declared dir
        CHECK(read_permitted(declared, "/project/assets/buffers/mesh0.bin"));  // under the declared dir
        CHECK(read_permitted(declared, "/project/assets/hero.gltf"));          // input still readable
        CHECK(!read_permitted(declared, "/project/assets/hero.bin"));          // still undeclared

        // Outer bound: a declared path OUTSIDE the jail can never widen the read set past it.
        SandboxPolicy escaper = policy;
        escaper.declared_read_paths.push_back("/etc");
        CHECK(!read_permitted(escaper, "/etc/passwd"));
    }

    // A malformed policy fails closed: no jail => nothing; and with a jail but NO input/declared paths,
    // nothing is readable even inside the jail (the narrowed default is input-bytes-only, not jail-wide).
    {
        SandboxPolicy empty;
        CHECK(!read_permitted(empty, "/anything"));
        CHECK(!write_permitted(empty, "/anything"));
        SandboxPolicy no_reads;
        no_reads.jail_root = "/project";
        CHECK(!read_permitted(no_reads, "/project/x")); // in-jail but neither input nor declared
        CHECK(!write_permitted(no_reads, "/project/x")); // no output key -> no writes
    }

    // HONEST staging (R-SEC-006): the primitive is named, and `enforced` is reported TRUTHFULLY per OS
    // — seccomp-bpf IS enforced on Linux now; Windows/macOS remain tracked follow-ups (not enforced).
    {
        const OsSandboxSupport support = os_sandbox_support();
        CHECK(!support.primitive.empty());
#if defined(__linux__)
        CHECK(support.enforced);                  // seccomp-bpf lockdown is live on Linux
        CHECK(support.primitive == "seccomp-bpf");
        CHECK(support.follow_up.empty());         // enforced => no outstanding follow-up note
#else
        CHECK(!support.enforced);       // Windows AppContainer / macOS sandbox-exec are follow-ups
        CHECK(!support.follow_up.empty()); // a tracked note is always present when not enforced
#endif
    }

    // The portable subprocess result codec: an ImportResult round-trips byte-for-byte across the pipe
    // frame, and a truncated / over-long / empty frame is REJECTED (never a crash) — proven on every
    // host, independent of the platform fork path.
    {
        ImportResult r;
        r.ok = true;
        DerivedArtifact mesh;
        mesh.kind = ArtifactKind::mesh;
        mesh.name = "mesh";
        mesh.derived_format_version = 7;
        mesh.bytes.push_back('\x00'); // embedded NUL — the frame is binary-clean, not C-string bound
        mesh.bytes.push_back('\x01');
        mesh.bytes.append("mid");
        mesh.bytes.push_back('\xFF');
        r.artifacts.push_back(mesh);
        DerivedArtifact audio;
        audio.kind = ArtifactKind::audio;
        audio.name = "audio.pcm";
        audio.derived_format_version = 3; // empty bytes on purpose
        r.artifacts.push_back(audio);
        r.diagnostics.push_back({"import.decode_failed", "bad chunk"});

        const std::string frame = encode_import_result(r);
        ImportResult back;
        CHECK(decode_import_result(frame, back));
        CHECK(back.ok);
        CHECK(back.artifacts.size() == 2);
        CHECK(back.artifacts[0].kind == ArtifactKind::mesh);
        CHECK(back.artifacts[0].name == "mesh");
        CHECK(back.artifacts[0].bytes == r.artifacts[0].bytes); // NUL-containing bytes preserved
        CHECK(back.artifacts[0].derived_format_version == 7);
        CHECK(back.artifacts[1].kind == ArtifactKind::audio);
        CHECK(back.artifacts[1].bytes.empty());
        CHECK(back.diagnostics.size() == 1);
        CHECK(back.diagnostics[0].code == "import.decode_failed");
        CHECK(back.diagnostics[0].message == "bad chunk");

        ImportResult rejected;
        CHECK(!decode_import_result(std::string_view(frame).substr(0, frame.size() - 1), rejected));
        CHECK(!decode_import_result(frame + std::string(1, '\x00'), rejected)); // trailing garbage
        CHECK(!decode_import_result(std::string_view{}, rejected));             // empty frame
    }

    // run_isolated() enforces the policy CONTRACT the subprocess runner applies at the syscall layer
    // (R-SEC-006/008/010). Its two refusal branches ARE the isolation guarantee, so they are covered
    // here directly (the fuzz replay only ever exercises the well-formed policy).
    {
        const StubImporter stub;
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/in.stub";
        policy.output_key = "/project/.cache/stub/out";

        // Happy path: a jailed source imports, and the audit records EXACTLY the granted envelope
        // (input + output key, no network). The in-process reference runner never claims an OS
        // primitive it did not install (honest staging), so os_primitive_enforced stays false here.
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
            CHECK(!iso.audit.os_primitive_enforced); // in-process runner never enforces a primitive
        }

        // Network requested -> REFUSED as import.jail_escape (R-SEC-010), importer never invoked.
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

        // A source path OUTSIDE the narrowed read scope -> REFUSED as import.jail_escape, never imported.
        {
            ImportInput in;
            in.source_path = "/etc/passwd"; // escapes the read scope
            in.source_bytes = "payload";
            const IsolatedImport iso = run_isolated(stub, in, policy);
            CHECK(!iso.result.ok);
            CHECK(iso.result.artifacts.empty());
            CHECK(has_code(iso.result.diagnostics, "import.jail_escape"));
        }
    }

    // run_subprocess() (issue #72): on Linux the pure importer runs in a fork()ed, seccomp-locked child
    // and the result is piped back (os_primitive_enforced=true); elsewhere it delegates to the
    // in-process slice (false). Either way the result is IDENTICAL (importers are pure), and the
    // parent-side policy refusals hold before any fork.
    {
        const StubImporter stub;
        SandboxPolicy policy;
        policy.jail_root = "/project";
        policy.input_path = "/project/assets/in.stub";
        policy.output_key = "/project/.cache/stub/out";

        // Happy path — the child's ImportResult survives the round-trip byte-for-byte, and the audit's
        // enforced flag mirrors this platform's OS-primitive support.
        {
            ImportInput in;
            in.source_path = policy.input_path;
            in.source_bytes = "payload-bytes";
            const IsolatedImport iso = run_subprocess(stub, in, policy);
            CHECK(iso.result.ok);
            CHECK(iso.result.artifacts.size() == 1);
            CHECK(iso.result.artifacts[0].name == "stub");
            CHECK(iso.result.artifacts[0].bytes == "payload-bytes"); // round-tripped through the pipe
            CHECK(iso.audit.input_path == policy.input_path);
            CHECK(!iso.audit.network_allowed);
            CHECK(iso.audit.os_primitive_enforced == os_sandbox_support().enforced);
        }

        // Refusals happen in the PARENT (never a fork): network + out-of-scope source both refuse.
        {
            ImportInput in;
            in.source_path = policy.input_path;
            in.source_bytes = "payload";
            SandboxPolicy net = policy;
            net.allow_network = true;
            const IsolatedImport iso = run_subprocess(stub, in, net);
            CHECK(!iso.result.ok);
            CHECK(iso.result.artifacts.empty());
            CHECK(has_code(iso.result.diagnostics, "import.jail_escape"));
        }
        {
            ImportInput in;
            in.source_path = "/etc/passwd";
            in.source_bytes = "payload";
            const IsolatedImport iso = run_subprocess(stub, in, policy);
            CHECK(!iso.result.ok);
            CHECK(has_code(iso.result.diagnostics, "import.jail_escape"));
        }
    }

#if defined(__linux__)
    // The REAL seccomp-bpf syscall filter (Linux, the enforced platform): in a fork()ed child that has
    // called apply_importer_sandbox(), a DENIED syscall (openat -> a fresh file, socket -> network)
    // fails closed with EPERM, while a PERMITTED syscall (write to an already-open descriptor) succeeds.
    // This is the "test permitted vs denied syscalls" gate (issue #72). NOTE: apply_importer_sandbox()
    // is irreversible, so it is called ONLY in the child — never in this test process.
    {
        int pfd[2];
        CHECK(::pipe(pfd) == 0);
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0)
        {
            ::close(pfd[0]);
            const SandboxApplyResult applied = apply_importer_sandbox();
            if (!applied.applied)
                ::_exit(10); // the filter must install on Linux

            // DENIED: opening a fresh file (input-bytes-only => no open* capability).
            const int fd = ::openat(AT_FDCWD, "/etc/passwd", O_RDONLY);
            const bool open_denied = (fd < 0 && errno == EPERM);
            if (fd >= 0)
                ::close(fd);

            // DENIED: a network socket (no ambient network, R-SEC-010).
            const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
            const bool socket_denied = (sock < 0 && errno == EPERM);
            if (sock >= 0)
                ::close(sock);

            // PERMITTED: writing to the already-open result pipe descriptor.
            const char ok = 'y';
            const bool write_permitted_syscall = (::write(pfd[1], &ok, 1) == 1);

            ::_exit((open_denied && socket_denied && write_permitted_syscall) ? 0 : 11);
        }

        // Parent: drain the permitted-write byte, then confirm the child validated every case (exit 0).
        ::close(pfd[1]);
        char drained = 0;
        while (::read(pfd[0], &drained, 1) < 0 && errno == EINTR)
        {
        }
        ::close(pfd[0]);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
        {
        }
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
#else
    // Non-Linux: apply_importer_sandbox() is a safe no-op (no enforced primitive yet) — calling it in
    // this process cannot lock it down. It honestly reports not-applied with the intended primitive.
    {
        const SandboxApplyResult applied = apply_importer_sandbox();
        CHECK(!applied.applied);
        CHECK(applied.primitive == os_sandbox_support().primitive);
    }
#endif

    IMPORT_TEST_MAIN_END();
}

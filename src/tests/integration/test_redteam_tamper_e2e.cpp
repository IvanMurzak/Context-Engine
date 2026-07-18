// Trust-tier ADVERSARIAL VALIDATION — `security-redteam-tamper-e2e` (M8.5 Wave-1 a17, issue #283):
// the M8.5 fail-closed exit clause, driven END-TO-END through the real a08 verify-before-execute fetch
// path — NOT just a unit test of the classifier (that is m8-exit-2 / test_verify_signature).
//
// The attack: a fetched, trust-bearing "version archive" (the R-VER-004 / R-SEC-009 fetched engine
// toolchain + export template the build consumes) is TAMPERED after signing — its bytes are modified
// while the filename stays valid-looking AND the length is UNCHANGED (the subtlest corruption: nothing
// a name/size heuristic would catch). It MUST be refused, machine-readably, before it is ever used —
// verify-before-use, fail closed (R-SEC-009 / L-58).
//
// The proof is a POSITIVE/TAMPER CONTRAST over a REAL Ed25519 signature (ssh-keygen, present on every
// CI runner), so the refusal is not vacuous:
//   1. In-process real crypto: an authentic signature VERIFIES; the same artifact with ONE byte flipped
//      (identical length, valid name) is REFUSED — proving the gate authenticates content, not the name.
//   2. Through the real `context` binary's fetch path (a true process boundary): the authentic archive
//      builds green (--dry-run), the TAMPERED archive is refused with a machine-readable envelope
//      `build.toolchain_fetch_failed` (the engine-fetched toolchain) and a tampered export template with
//      `build.template_unverified` — both BEFORE the artifact is used, never "used with a warning".
// If ssh-keygen is somehow absent, the crypto contrast is skipped but the UNCONDITIONAL fail-closed
// refusal (an invalid/absent signature is refused through the CLI) still runs — the gate never opens.
//
// Runs in the general ctest step (spawns the built `context` binary; ~seconds). No secrets: the
// ephemeral key is minted at test time and never leaves the temp dir (the production private key lives
// only in the environment-protected release secret — docs/signing.md).

#include "m8_exit_test.h"

#include "context/common/subprocess.h"
#include "context/common/verify_signature.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace sp = context::common::subprocess;
namespace common = context::common;
using context::editor::contract::Json;
using context::tests::m8::report;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace fs = std::filesystem;

namespace
{

// The CLI verifies the fetched toolchain / export template against the pinned trust root under the
// PRODUCTION release identity + artifact namespace (build_command.cpp hardcodes the defaults). The
// ephemeral trust root pins a test key under EXACTLY those, so an authentic ephemeral signature passes
// the real CLI gate — no production private key needed.
constexpr const char* kIdentity = "context-engine-release";
constexpr const char* kNamespace = "context-engine-artifact";

std::string read_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_bytes(const fs::path& p, const std::string& bytes)
{
    std::error_code ec;
    if (p.has_parent_path())
        fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Run one shell command, swallowing a MetacharacterError (a temp path with a shell metacharacter — not
// this test's concern) as a non-fatal failure. Returns the exit code, or -999 if it could not build.
int run(const std::string& command)
{
    return sp::run_command(command);
}

// Author a minimal BUILDABLE Linux project (mirrors src/cli/tests/test_build_command.cpp): a valid
// project.json + an L-33-hex-id scene with a camera, so `context build --target linux` succeeds and the
// authentic-artifact positive control is a clean ok:true.
fs::path make_project(const fs::path& dir)
{
    write_bytes(dir / "project.json",
                R"({"$schema":"ctx:project","scene":"scenes/main.scene.json","version":1})");
    write_bytes(dir / "scenes" / "main.scene.json",
                R"({"$schema":"ctx:scene","version":1,"entities":[
                     {"id":"aaaa0000aaaa0001","name":"Camera","components":{"camera":{"fov":1.0}}},
                     {"id":"aaaa0000aaaa0002","name":"Player","components":{"transform":{}}}]})");
    return dir;
}

// Drive the REAL `context build` binary, capturing its R-CLI-008 envelope from stdout. Returns the
// parsed envelope (null Json on a launch/parse failure).
Json run_context_build(const std::vector<std::string>& args)
{
    const fs::path out = sp::make_scratch_path("ctx-redteam-build", ".json");
    sp::ScratchFile guard(out);
    try
    {
        std::string cmd = sp::quote_argument(CONTEXT_BINARY) + " build";
        for (const std::string& a : args)
            cmd += " " + sp::quote_argument(a);
        cmd += " > " + sp::quote_argument(out.string());
        (void)run(cmd);
    }
    catch (const sp::MetacharacterError&)
    {
        return Json();
    }
    const std::string text = sp::read_file(out);
    try
    {
        return Json::parse(text);
    }
    catch (const std::exception&)
    {
        return Json();
    }
}

bool envelope_ok(const Json& env)
{
    return env.contains("ok") && env.at("ok").is_bool() && env.at("ok").as_bool();
}

std::string envelope_code(const Json& env)
{
    if (!env.contains("error") || !env.at("error").contains("code"))
        return std::string();
    return env.at("error").at("code").as_string();
}

// Mint an ephemeral ed25519 key (private + .pub) via ssh-keygen; return true + the private-key path on
// success. Empty passphrase; the key never leaves `dir`.
bool mint_key(const fs::path& dir, fs::path& key_out)
{
    const fs::path key = dir / "signer";
    try
    {
        const std::string cmd = sp::quote_argument("ssh-keygen") + " -t ed25519 -f " +
                                sp::quote_argument(key.string()) + " -N \"\" -C ephemeral-redteam";
        if (run(cmd) != 0)
            return false;
    }
    catch (const sp::MetacharacterError&)
    {
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(fs::path(key.string() + ".pub"), ec))
        return false;
    key_out = key;
    return true;
}

// Sign `artifact` detached with `key` under `kNamespace`; ssh-keygen writes `<artifact>.sig`.
bool sign(const fs::path& key, const fs::path& artifact, fs::path& sig_out)
{
    try
    {
        const std::string cmd = sp::quote_argument("ssh-keygen") + " -Y sign -f " +
                                sp::quote_argument(key.string()) + " -n " +
                                sp::quote_argument(kNamespace) + " " +
                                sp::quote_argument(artifact.string());
        if (run(cmd) != 0)
            return false;
    }
    catch (const sp::MetacharacterError&)
    {
        return false;
    }
    const fs::path sig = fs::path(artifact.string() + ".sig");
    std::error_code ec;
    if (!fs::is_regular_file(sig, ec))
        return false;
    sig_out = sig;
    return true;
}

// Write an allowed_signers trust root pinning `key.pub` under the production release identity +
// namespace (so the real CLI gate, which uses those, accepts an authentic ephemeral signature).
bool write_trust_root(const fs::path& key, const fs::path& dest)
{
    const std::string pub = read_bytes(fs::path(key.string() + ".pub"));
    // Keep the first two whitespace-separated fields (key-type + base64 blob).
    std::istringstream ss(pub);
    std::string type, blob;
    if (!(ss >> type >> blob))
        return false;
    write_bytes(dest, std::string(kIdentity) + " namespaces=\"" + kNamespace + "\" " + type + " " +
                          blob + " ephemeral-redteam\n");
    return true;
}

} // namespace

int main()
{
    const fs::path base = sp::make_scratch_path("ctx-redteam-e2e", "");
    std::error_code ec;
    fs::create_directories(base, ec);

    const fs::path project = make_project(base / "proj");

    // A fetched, trust-bearing "version archive" (engine toolchain) with a valid-looking name.
    const std::string archive_name = "context-engine-toolchain-9.9.9-linux-x64.tar";
    const fs::path archive = base / archive_name;
    // Deterministic pseudo-content (512 bytes) — a stand-in for the toolchain tarball bytes.
    std::string bytes;
    bytes.reserve(512);
    for (int i = 0; i < 512; ++i)
        bytes.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
    write_bytes(archive, bytes);

    // --- attempt the REAL ephemeral-key crypto setup (ssh-keygen; present on every CI runner) --------
    bool crypto_ok = false;
    fs::path key, sig, trust_root = base / "allowed_signers";
    if (mint_key(base, key) && sign(key, archive, sig) && write_trust_root(key, trust_root))
    {
        const common::VerifyOutcome pos =
            common::verify_signature(archive, sig, trust_root, kIdentity, kNamespace);
        crypto_ok = pos.ok(); // gate the crypto contrast on a working ssh-keygen + signature
    }

    if (crypto_ok)
    {
        // === 1. In-process REAL crypto: authentic verifies; a same-length byte-flip is REFUSED ========
        // The tamper keeps the filename valid-looking AND the length identical — only the CONTENT moves.
        const fs::path tampered = base / archive_name; // same valid-looking basename, different dir
        const fs::path tampered_dir = base / "tampered";
        fs::create_directories(tampered_dir, ec);
        const fs::path tampered_path = tampered_dir / archive_name;
        std::string tampered_bytes = bytes;
        tampered_bytes[7] = static_cast<char>(tampered_bytes[7] ^ 0x40); // flip ONE byte in place
        write_bytes(tampered_path, tampered_bytes);

        // Length is UNCHANGED and the name is still a valid version-archive name.
        CHECK(read_bytes(tampered_path).size() == bytes.size());
        CHECK(tampered_path.filename().string() == archive_name);

        const common::VerifyOutcome authentic =
            common::verify_signature(archive, sig, trust_root, kIdentity, kNamespace);
        CHECK(authentic.ok()); // non-vacuous: an authentic signature DOES verify
        const common::VerifyOutcome tampered_verify =
            common::verify_signature(tampered_path, sig, trust_root, kIdentity, kNamespace);
        CHECK(!tampered_verify.ok());
        CHECK(tampered_verify.status == common::VerifyStatus::Refused); // fail closed on tamper

        // A signature minted for THIS namespace cannot be replayed into another, and the production
        // trust root (pins only the real release key) refuses the ephemeral key — no downgrade surface.
        CHECK(!common::verify_signature(archive, sig, trust_root, kIdentity, "other-namespace").ok());

        // === 2. Through the real `context` binary's a08 fetch path (process boundary) =================
        // Positive control: the AUTHENTIC toolchain archive verifies THROUGH the CLI and the build is
        // green — proving the gate is not vacuously refusing everything.
        const Json ok_env = run_context_build({"--target", "linux", "--project", project.string(),
                                               "--dry-run", "--toolchain-artifact", archive.string(),
                                               "--toolchain-sig", sig.string(), "--trust-root",
                                               trust_root.string()});
        CHECK(envelope_ok(ok_env));

        // The attack: the TAMPERED toolchain archive (valid name, unchanged length) is REFUSED
        // machine-readably — build.toolchain_fetch_failed — BEFORE it is used to build. Fail closed.
        const Json bad_env = run_context_build({"--target", "linux", "--project", project.string(),
                                                "--dry-run", "--toolchain-artifact",
                                                tampered_path.string(), "--toolchain-sig", sig.string(),
                                                "--trust-root", trust_root.string()});
        CHECK(!envelope_ok(bad_env));
        CHECK(envelope_code(bad_env) == "build.toolchain_fetch_failed");

        // The sibling fetch surface — the R-BUILD-004 export template (--runtime host binary): a
        // tampered template is refused with build.template_unverified, never packed into an artifact.
        const fs::path runtime = base / "context-runtime-9.9.9-linux-x64";
        write_bytes(runtime, bytes);
        fs::path runtime_sig;
        if (sign(key, runtime, runtime_sig))
        {
            const fs::path tampered_runtime = tampered_dir / "context-runtime-9.9.9-linux-x64";
            std::string rt = bytes;
            rt[3] = static_cast<char>(rt[3] ^ 0x40); // same-length byte flip
            write_bytes(tampered_runtime, rt);
            CHECK(read_bytes(tampered_runtime).size() == bytes.size());

            const Json rt_ok = run_context_build(
                {"--target", "linux", "--project", project.string(), "--dry-run", "--runtime",
                 runtime.string(), "--runtime-sig", runtime_sig.string(), "--trust-root",
                 trust_root.string()});
            CHECK(envelope_ok(rt_ok)); // authentic template verifies through the CLI

            const Json rt_bad = run_context_build(
                {"--target", "linux", "--project", project.string(), "--dry-run", "--runtime",
                 tampered_runtime.string(), "--runtime-sig", runtime_sig.string(), "--trust-root",
                 trust_root.string()});
            CHECK(!envelope_ok(rt_bad));
            CHECK(envelope_code(rt_bad) == "build.template_unverified");
        }
    }
    else
    {
        std::fprintf(stderr,
                     "[redteam-tamper] ssh-keygen crypto path unavailable — running the "
                     "unconditional fail-closed refusal only\n");
    }

    // === UNCONDITIONAL fail-closed refusal (runs with OR without ssh-keygen) =========================
    // An artifact presented with an invalid / absent signature is refused through the CLI fetch path —
    // the gate never opens fail-OPEN. (A nonexistent signature is the canonical unsigned => refused
    // case, decided before ssh-keygen is even consulted, so this holds on any host.)
    {
        const fs::path prod_root =
            fs::path(CONTEXT_TRUST_ROOT); // the pinned PRODUCTION allowed_signers
        const fs::path absent_sig = base / "does-not-exist.sig";
        const Json refused = run_context_build({"--target", "linux", "--project", project.string(),
                                                "--dry-run", "--toolchain-artifact", archive.string(),
                                                "--toolchain-sig", absent_sig.string(), "--trust-root",
                                                prod_root.string()});
        CHECK(!envelope_ok(refused));
        CHECK(envelope_code(refused) == "build.toolchain_fetch_failed");
    }

    fs::remove_all(base, ec);

    return report("security-redteam-tamper-e2e",
                  "a tampered signed version archive (valid name, unchanged length) is refused "
                  "machine-readably through the a08 verify-before-execute fetch path (R-SEC-009 fail "
                  "closed); an authentic signature verifies");
}

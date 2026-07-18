// `context build` CLI tests (M8 task a05, issue #257) — the DoD-1 END-TO-END: a real on-disk project
// builds a real Linux pack headless, the R-CLI-008 envelope carries the build's generation + artifact
// pointers, and the written bytes are a valid pack. Plus the CLI-layer failure paths (missing --target,
// missing project → template_unverified, unknown target → toolchain_fetch_failed), --dry-run (plan
// without writing), and per-verb `--help`. The orchestrator's exhaustive R-QA-011 malformed/failure
// corpus (every build.* code) lives in src/editor/build/tests/test_build_orchestrator.cpp; here we prove
// the CLI wiring + disk IO.
//
// The project is authored with L-33 stable ids (16..32 lowercase hex) on every entity — the correct
// authored form composition requires; an id-less scene is excluded by compose (compose.missing_id) and
// would fail-closed with build.template_unverified.

#include "context/cli/app.h"
#include "context/cli/build_command.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/editor/pack/pack_reader.h"
#include "cli_test.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>

using namespace context::cli;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace pack = context::editor::pack;
namespace fs = std::filesystem;

namespace
{
void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

fs::path unique_temp_dir(const std::string& tag)
{
    static int counter = 0;
    const fs::path dir =
        fs::temp_directory_path() /
        ("ctx-build-" + tag + "-" + std::to_string(++counter) + "-" +
         std::to_string(static_cast<long long>(
             fs::file_time_type::clock::now().time_since_epoch().count() & 0xffffff)));
    remove_quiet(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Author a minimal buildable project: project.json + an L-33-id-bearing scene with a camera.
fs::path make_project(const std::string& tag)
{
    const fs::path dir = unique_temp_dir(tag);
    write_file(dir / "project.json",
               R"({"$schema":"ctx:project","scene":"scenes/main.scene.json","version":1})");
    write_file(dir / "scenes" / "main.scene.json",
               R"({"$schema":"ctx:scene","version":1,"entities":[
                    {"id":"aaaa0000aaaa0001","name":"Camera","components":{"camera":{"fov":1.0}}},
                    {"id":"aaaa0000aaaa0002","name":"Player","components":{"transform":{}}}]})");
    return dir;
}

std::string read_bytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Build a minimal PE/COFF image whose Security data-directory (index 4) Size is non-zero iff `signed_`
// — the exact bit the a10 signing hook (pe_has_authenticode_signature) keys off. Written to `path` so a
// CLI `--sign` run can inspect it as the shipped runtime binary. PE32 layout suffices for the test.
void write_pe(const fs::path& path, bool signed_)
{
    std::string b(0x100, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    const auto put_le = [&b](std::size_t off, unsigned long value, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            b[off + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    };
    put_le(0x3C, 0x40, 4); // e_lfanew
    b[0x40] = 'P';
    b[0x41] = 'E';
    const std::size_t opt_off = 0x40 + 4 + 20;
    put_le(opt_off, 0x010b, 2);     // PE32 magic
    put_le(opt_off + 92, 16, 4);    // NumberOfRvaAndSizes
    const std::size_t sec = opt_off + 96 + 4 * 8; // Security dir entry (index 4)
    put_le(sec + 4, signed_ ? 0x2A0 : 0, 4);      // Size — non-zero ⇒ signature present
    write_file(path, b);
}

// Build a minimal thin 64-bit little-endian Mach-O for the a13 signing hook (macho_has_code_signature).
// When `signed_`, the referenced `__LINKEDIT` blob is a WELL-FORMED code-signature SuperBlob → CodeDirectory
// whose flags carry CS_ADHOC iff `adhoc` — so the hook reports a real Developer-ID signature (adhoc=false)
// as "signed" and the arm64 auto-embedded ad-hoc signature (adhoc=true) as "unsigned". When !signed_ the
// LC_CODE_SIGNATURE datasize is 0. Signature-blob fields are big-endian regardless of the header's
// little-endian. Written to `path` so a CLI `--sign --target macos` run can inspect it as the runtime.
void write_macho(const fs::path& path, bool signed_, bool adhoc = false)
{
    const std::size_t sig_off = 0x30;             // right after the 32-byte header + 16-byte load command
    const std::size_t cd_off = 20;                // CodeDirectory offset within the blob (12 hdr + 8 idx)
    const std::size_t cd_len = 32;                // covers magic(0)+length(4)+version(8)+flags(12)
    const std::size_t sig_len = cd_off + cd_len;  // SuperBlob(12) + one BlobIndex(8) + CodeDirectory
    std::string b(signed_ ? sig_off + sig_len : 0x40, '\0');
    const auto put_le = [&b](std::size_t off, std::size_t value, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            b[off + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    };
    const auto put_be = [&b](std::size_t off, std::size_t value, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            b[off + i] = static_cast<char>((value >> (8 * (n - 1 - i))) & 0xFF);
    };
    put_le(0, 0xFEEDFACFul, 4); // MH_MAGIC_64 stored little-endian (file bytes CF FA ED FE)
    put_le(16, 1, 4);           // ncmds = 1
    put_le(20, 16, 4);          // sizeofcmds = 16
    const std::size_t lc = 32;  // load commands begin after the 64-bit mach_header
    put_le(lc, 0x1Dul, 4);      // cmd = LC_CODE_SIGNATURE
    put_le(lc + 4, 16, 4);      // cmdsize = 16
    put_le(lc + 8, signed_ ? sig_off : 0, 4);  // dataoff
    put_le(lc + 12, signed_ ? sig_len : 0, 4); // datasize — non-zero ⇒ signature blob present
    if (signed_)
    {
        // CS_SuperBlob (big-endian): magic, length, count, then one CS_BlobIndex {type, offset}.
        put_be(sig_off + 0, 0xFADE0CC0ul, 4);   // CSMAGIC_EMBEDDED_SIGNATURE
        put_be(sig_off + 4, sig_len, 4);        // length
        put_be(sig_off + 8, 1, 4);              // count = 1
        put_be(sig_off + 12, 0, 4);             // index[0].type = CSSLOT_CODEDIRECTORY
        put_be(sig_off + 16, cd_off, 4);        // index[0].offset (within blob)
        // CS_CodeDirectory (big-endian): magic, length, version, flags (only magic + flags are parsed).
        const std::size_t cd = sig_off + cd_off;
        put_be(cd + 0, 0xFADE0C02ul, 4);                    // CSMAGIC_CODEDIRECTORY
        put_be(cd + 4, cd_len, 4);                          // length
        put_be(cd + 8, 0x00020400ul, 4);                    // version (cosmetic)
        put_be(cd + 12, adhoc ? 0x00000002ul : 0ul, 4);     // flags: CS_ADHOC iff adhoc
    }
    write_file(path, b);
}
} // namespace

int main()
{
    // --- DoD 1: an authored project builds a real Linux pack end-to-end ------------------------------
    {
        const fs::path dir = make_project("e2e");
        const fs::path out = dir / "build" / "game.pack";
        const std::map<std::string, std::string> flags = {
            {"target", "linux"}, {"project", dir.string()}, {"out", out.generic_string()}};
        const Envelope e = run_build(flags);
        CHECK(e.ok());

        // The envelope carries the build's generation + artifact pointers (the DoD-1 envelope shape).
        const Json& data = e.data();
        CHECK(data.at("target").as_string() == "linux");
        CHECK(!data.at("generation").as_string().empty()); // 64-bit content identity, decimal string
        CHECK(data.at("generation").as_string() != "0");
        // a06: the adapter is a machine-readable object (the default flavor is desktop, render present).
        const Json& adapter = data.at("adapter");
        CHECK(adapter.at("supported").as_bool());
        CHECK(!adapter.at("stub").as_bool());
        CHECK(adapter.at("flavor").as_string() == "desktop");
        CHECK(adapter.at("renderPresent").as_bool());
        CHECK(adapter.at("runtimeBinary").as_string() == "context-runtime");
        CHECK(adapter.at("deterministicModuloLink").as_bool());
        CHECK(adapter.at("layout").is_array());
        const Json& artifact = data.at("artifact");
        CHECK(artifact.at("written").as_bool());
        CHECK(artifact.at("packSize").as_int() > 0);
        CHECK(!artifact.at("packHash").as_string().empty());
        CHECK(artifact.at("entityCount").as_int() == 2);
        CHECK(artifact.at("packPath").as_string() == out.generic_string());

        // The written bytes ARE a valid pack (read_pack verifies every chunk's content hash).
        CHECK(fs::exists(out));
        const pack::ParsedPack parsed = pack::read_pack(read_bytes(out));
        CHECK(parsed.ok);

        // Reproducible: a second build writes the byte-identical pack (R-FILE-010 cache property).
        const std::string first_pack = read_bytes(out);
        const Envelope again = run_build(flags);
        CHECK(again.ok());
        CHECK(read_bytes(out) == first_pack);
        CHECK(again.data().at("generation").as_string() == data.at("generation").as_string());

        remove_quiet(dir);
    }

    // --- a10 Windows adapter: supported, .exe binary, requiresSigning in the envelope ----------------
    {
        const fs::path dir = make_project("win");
        const fs::path out = dir / "build" / "game.pack";
        const Envelope e = run_build({{"target", "windows"},
                                      {"flavor", "server"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()}});
        CHECK(e.ok());
        const Json& adapter = e.data().at("adapter");
        CHECK(adapter.at("supported").as_bool());
        CHECK(adapter.at("flavor").as_string() == "server");
        CHECK(!adapter.at("renderPresent").as_bool());
        CHECK(adapter.at("requiresSigning").as_bool()); // windows requires Authenticode (R-SEC-003)
        CHECK(adapter.at("runtimeBinary").as_string() == "context-runtime-server.exe");
        remove_quiet(dir);
    }

    // --- a13 macOS adapter: supported, suffix-less Mach-O binary, requiresSigning in the envelope -----
    {
        const fs::path dir = make_project("mac");
        const fs::path out = dir / "build" / "game.pack";
        const Envelope e = run_build({{"target", "macos"},
                                      {"flavor", "desktop"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()}});
        CHECK(e.ok());
        const Json& adapter = e.data().at("adapter");
        CHECK(adapter.at("supported").as_bool());
        CHECK(adapter.at("flavor").as_string() == "desktop");
        CHECK(adapter.at("renderPresent").as_bool());
        CHECK(adapter.at("requiresSigning").as_bool()); // macOS requires Developer ID + notarization
        CHECK(adapter.at("runtimeBinary").as_string() == "context-runtime"); // no `.exe` suffix (Mach-O)
        remove_quiet(dir);
    }

    // --- a11 web adapter: supported (desktop-only), wasm+js runtime, renderPresent, no signing --------
    {
        const fs::path dir = make_project("web");
        const fs::path out = dir / "build" / "web.pack";
        const Envelope e = run_build(
            {{"target", "web"}, {"project", dir.string()}, {"out", out.generic_string()}});
        CHECK(e.ok());
        const Json& data = e.data();
        CHECK(data.at("target").as_string() == "web");
        const Json& adapter = data.at("adapter");
        CHECK(adapter.at("supported").as_bool());
        CHECK(!adapter.at("stub").as_bool());
        CHECK(adapter.at("flavor").as_string() == "desktop"); // web is desktop-only
        CHECK(adapter.at("renderPresent").as_bool());          // WebGPU-only
        CHECK(!adapter.at("requiresSigning").as_bool());       // no v1 web code-signing
        CHECK(adapter.at("runtimeBinary").as_string() == "context-runtime.wasm");
        CHECK(adapter.at("runtimeLoader").as_string() == "context-runtime.js");
        // A real pack was written for the web target too (web is a first-class pack platform).
        CHECK(fs::exists(out));
        remove_quiet(dir);
    }

    // --- a11 web --emit-artifact: assembles the 5-file bundle (wasm + js + index.html + pack + manifest)
    {
        const fs::path dir = make_project("web-emit");
        const fs::path out = dir / "build" / "web.pack";
        const fs::path wasm = dir / "context-runtime.wasm";
        const fs::path js = dir / "context-runtime.js";
        write_file(wasm, "\0asm\x01\x00\x00\x00fake-wasm-module-bytes"); // a stand-in wasm blob
        write_file(js, "// fake emscripten glue\nvar Module;\n");
        const fs::path tar = dir / "dist" / "web-bundle.tar";
        const Envelope e = run_build({{"target", "web"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", wasm.string()},
                                      {"runtime-loader", js.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(e.ok());
        const Json& emit = e.data().at("artifactEmit");
        CHECK(emit.at("emitted").as_bool());
        CHECK(emit.at("entryCount").as_int() == 5); // bin/wasm + bin/js + index.html + content/ + manifest
        CHECK(fs::exists(tar));
        remove_quiet(dir);
    }

    // --- a11 web --emit-artifact WITHOUT --runtime-loader declares the reason (never a silent skip) ---
    {
        const fs::path dir = make_project("web-noloader");
        const fs::path wasm = dir / "context-runtime.wasm";
        write_file(wasm, "\0asm\x01\x00\x00\x00fake");
        const fs::path tar = dir / "dist" / "web.tar";
        const Envelope e = run_build({{"target", "web"},
                                      {"project", dir.string()},
                                      {"runtime", wasm.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(e.ok());
        CHECK(!e.data().at("artifactEmit").at("emitted").as_bool());
        CHECK(!e.data().at("artifactEmit").at("reason").as_string().empty());
        remove_quiet(dir);
    }

    // --- a11 web --smoke: not-run in the native runtime (its smoke gate is the render-web CI job) ------
    {
        const fs::path dir = make_project("web-smoke");
        const Envelope e =
            run_build({{"target", "web"}, {"project", dir.string()}, {"smoke", "true"}});
        CHECK(e.ok());
        CHECK(!e.data().at("smoke").at("ran").as_bool());
        CHECK(!e.data().at("smoke").at("reason").as_string().empty());
        remove_quiet(dir);
    }

    // --- a10 signing hook (--sign): an UNSIGNED windows runtime is an explicit never-silent WARNING ---
    {
        const fs::path dir = make_project("sign-unsigned");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path rt = dir / "context-runtime.exe";
        write_pe(rt, /*signed_=*/false); // a well-formed PE with an EMPTY Certificate Table
        const Envelope e = run_build({{"target", "windows"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", rt.string()},
                                      {"sign", ""}});
        CHECK(e.ok()); // an unsigned artifact is a WARNING, not a build failure
        const Json& signing = e.data().at("signing");
        CHECK(signing.at("required").as_bool());
        CHECK(signing.at("state").as_string() == "unsigned");
        CHECK(!signing.at("signed").as_bool());
        CHECK(signing.at("code").as_string() == "build.artifact_unsigned");
        CHECK(signing.at("primary").as_string() == "azure-trusted-signing");
        CHECK(signing.at("timestampRequired").as_bool());
        CHECK(!signing.at("warning").as_string().empty()); // NEVER silent
        // The warning ALSO surfaces at the envelope top level.
        CHECK(!e.warnings().empty());
        remove_quiet(dir);
    }

    // --- a10 signing hook (--sign): a SIGNED windows runtime reports state "signed", no warning -------
    {
        const fs::path dir = make_project("sign-signed");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path rt = dir / "context-runtime.exe";
        write_pe(rt, /*signed_=*/true); // a well-formed PE with a NON-EMPTY Certificate Table
        const Envelope e = run_build({{"target", "windows"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", rt.string()},
                                      {"sign", ""}});
        CHECK(e.ok());
        const Json& signing = e.data().at("signing");
        CHECK(signing.at("state").as_string() == "signed");
        CHECK(signing.at("signed").as_bool());
        CHECK(!signing.contains("code"));
        CHECK(e.warnings().empty());
        remove_quiet(dir);
    }

    // --- a13 signing hook (--sign): an UNSIGNED macOS runtime is an explicit never-silent WARNING -----
    {
        const fs::path dir = make_project("mac-sign-unsigned");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path rt = dir / "context-runtime";
        write_macho(rt, /*signed_=*/false); // a well-formed Mach-O with a zero-datasize LC_CODE_SIGNATURE
        const Envelope e = run_build({{"target", "macos"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", rt.string()},
                                      {"sign", ""}});
        CHECK(e.ok()); // an unsigned/un-notarized artifact is a WARNING, not a build failure
        const Json& signing = e.data().at("signing");
        CHECK(signing.at("required").as_bool());
        CHECK(signing.at("state").as_string() == "unsigned");
        CHECK(!signing.at("signed").as_bool());
        CHECK(signing.at("code").as_string() == "build.artifact_unsigned");
        CHECK(signing.at("method").as_string() == "developer-id-notarization");
        CHECK(signing.at("primary").as_string() == "apple-notary");
        CHECK(signing.at("notarizationRequired").as_bool()); // macOS-only field
        CHECK(!signing.contains("fallback"));                // no v1 macOS fallback (omitted)
        CHECK(!signing.at("warning").as_string().empty());   // NEVER silent
        CHECK(!e.warnings().empty());
        remove_quiet(dir);
    }

    // --- a13 signing hook (--sign): a SIGNED macOS runtime reports state "signed", no warning ----------
    {
        const fs::path dir = make_project("mac-sign-signed");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path rt = dir / "context-runtime";
        write_macho(rt, /*signed_=*/true); // a Mach-O with a NON-EMPTY LC_CODE_SIGNATURE blob
        const Envelope e = run_build({{"target", "macos"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", rt.string()},
                                      {"sign", ""}});
        CHECK(e.ok());
        const Json& signing = e.data().at("signing");
        CHECK(signing.at("state").as_string() == "signed");
        CHECK(signing.at("signed").as_bool());
        CHECK(!signing.contains("code"));
        CHECK(e.warnings().empty());
        remove_quiet(dir);
    }

    // --- a13 signing hook (--sign): an AD-HOC-only macOS runtime is UNSIGNED (arm64 CI parity) ---------
    // Apple Silicon (arm64) linkers auto-embed an ad-hoc signature (CS_ADHOC, no signing identity) into
    // every Mach-O at link time — the exact per-PR macos-export CI scenario. A mere-presence check
    // mis-reported it "signed"; parsing the CodeDirectory CS_ADHOC flag keeps the report "unsigned".
    {
        const fs::path dir = make_project("mac-sign-adhoc");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path rt = dir / "context-runtime";
        write_macho(rt, /*signed_=*/true, /*adhoc=*/true); // a Mach-O carrying ONLY an ad-hoc signature
        const Envelope e = run_build({{"target", "macos"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", rt.string()},
                                      {"sign", ""}});
        CHECK(e.ok()); // an unsigned/ad-hoc artifact is a WARNING, not a build failure
        const Json& signing = e.data().at("signing");
        CHECK(signing.at("state").as_string() == "unsigned");
        CHECK(!signing.at("signed").as_bool());
        CHECK(signing.at("code").as_string() == "build.artifact_unsigned");
        CHECK(!signing.at("warning").as_string().empty()); // NEVER silent
        CHECK(!e.warnings().empty());
        remove_quiet(dir);
    }

    // --- a10 signing hook (--sign) on a NON-signing target reports "not-required", never a warning ----
    {
        const fs::path dir = make_project("sign-linux");
        const fs::path out = dir / "build" / "game.pack";
        const Envelope e = run_build({{"target", "linux"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"sign", ""}});
        CHECK(e.ok());
        const Json& signing = e.data().at("signing");
        CHECK(!signing.at("required").as_bool());
        CHECK(signing.at("state").as_string() == "not-required");
        CHECK(e.warnings().empty());
        remove_quiet(dir);
    }

    // --- --dry-run: plans + verifies without writing the pack ----------------------------------------
    {
        const fs::path dir = make_project("dry");
        const fs::path out = dir / "build" / "game.pack";
        const std::map<std::string, std::string> flags = {{"target", "linux"},
                                                          {"project", dir.string()},
                                                          {"out", out.generic_string()},
                                                          {"dry-run", "true"}};
        const Envelope e = run_build(flags);
        CHECK(e.ok());
        CHECK(!e.data().at("artifact").at("written").as_bool());
        CHECK(!fs::exists(out)); // dry-run wrote nothing
        remove_quiet(dir);
    }

    // --- missing --target is a usage error -----------------------------------------------------------
    {
        const Envelope e = run_build({});
        CHECK(!e.ok());
        CHECK(e.error().has_value());
        CHECK(e.error()->code == "usage.missing_argument");
    }

    // --- a project with no manifest fails the pre-build template verification -------------------------
    {
        const fs::path dir = unique_temp_dir("nomanifest");
        std::error_code ec;
        fs::create_directories(dir, ec);
        const Envelope e = run_build({{"target", "linux"}, {"project", dir.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.template_unverified");
        remove_quiet(dir);
    }

    // --- an unknown target has no toolchain manifest entry -------------------------------------------
    {
        const fs::path dir = make_project("badtarget");
        const Envelope e = run_build({{"target", "playstation"}, {"project", dir.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.toolchain_fetch_failed");
        remove_quiet(dir);
    }

    // --- through the CLI grammar: `context build --target linux --project <dir>` --------------------
    {
        const fs::path dir = make_project("cli");
        const fs::path out = dir / "build" / "game.pack";
        const Envelope e = run({"build", "--target", "linux", "--project", dir.string(), "--out",
                                out.generic_string()});
        CHECK(e.ok());
        CHECK(fs::exists(out));
        remove_quiet(dir);
    }

    // --- a06 --flavor server: the headless adapter (render absent) reports its plan -----------------
    {
        const fs::path dir = make_project("server");
        const Envelope e =
            run_build({{"target", "linux"}, {"flavor", "server"}, {"project", dir.string()}});
        CHECK(e.ok());
        const Json& adapter = e.data().at("adapter");
        CHECK(adapter.at("supported").as_bool());
        CHECK(adapter.at("flavor").as_string() == "server");
        CHECK(!adapter.at("renderPresent").as_bool());
        CHECK(adapter.at("runtimeBinary").as_string() == "context-runtime-server");
        remove_quiet(dir);
    }

    // --- a06 unknown --flavor is a usage error (never a silent fallback) -----------------------------
    {
        const fs::path dir = make_project("badflavor");
        const Envelope e =
            run_build({{"target", "linux"}, {"flavor", "console"}, {"project", dir.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "usage.invalid");
        remove_quiet(dir);
    }

    // --- a06 --emit-artifact assembles the runnable tarball (R-BUILD-005) ----------------------------
    {
        const fs::path dir = make_project("artifact");
        const fs::path out = dir / "build" / "game.pack";
        const fs::path runtime = dir / "fake-runtime.bin"; // stand-in for the shipped host binary
        write_file(runtime, "\x7f\x45\x4c\x46 fake elf bytes for the export template");
        const fs::path tar = dir / "dist" / "game-linux-server.tar";
        const Envelope e = run_build({{"target", "linux"},
                                      {"flavor", "server"},
                                      {"project", dir.string()},
                                      {"out", out.generic_string()},
                                      {"runtime", runtime.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(e.ok());
        const Json& emit = e.data().at("artifactEmit");
        CHECK(emit.at("emitted").as_bool());
        CHECK(emit.at("entryCount").as_int() == 4); // bin/ + content/ + launch.sh + manifest
        CHECK(fs::exists(tar));
        CHECK(fs::file_size(tar) > 0);
        remove_quiet(dir);
    }

    // --- a06 --emit-artifact without --runtime declares the reason (never a silent skip) ------------
    {
        const fs::path dir = make_project("noreq");
        const fs::path tar = dir / "dist" / "game.tar";
        const Envelope e = run_build({{"target", "linux"},
                                      {"project", dir.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(e.ok());
        CHECK(!e.data().at("artifactEmit").at("emitted").as_bool());
        CHECK(!e.data().at("artifactEmit").at("reason").as_string().empty());
        remove_quiet(dir);
    }

    // --- a06 --smoke without --runtime declares ran=false machine-readably (R-BUILD-009) ------------
    {
        const fs::path dir = make_project("smoke");
        const Envelope e =
            run_build({{"target", "linux"}, {"project", dir.string()}, {"smoke", "true"}});
        CHECK(e.ok());
        CHECK(!e.data().at("smoke").at("ran").as_bool());
        CHECK(!e.data().at("smoke").at("reason").as_string().empty());
        remove_quiet(dir);
    }

    // --- a06 --smoke-ticks rejects a negative value (the "non-negative integer" contract holds) ------
    {
        const fs::path dir = make_project("smokeneg");
        const Envelope e = run_build({{"target", "linux"},
                                      {"project", dir.string()},
                                      {"smoke", "true"},
                                      {"smoke-ticks", "-5"}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "usage.invalid");
        remove_quiet(dir);
    }

    // --- a08 verify-before-use: the export template (--runtime) fails closed on a bad signature ------
    // R-SEC-009: when a detached signature is supplied for the R-BUILD-004 export template, it MUST
    // verify against the pinned trust root or the build is refused with build.template_unverified.
    // Robust whether or not ssh-keygen is on PATH: an invalid signature is Refused, an absent verifier
    // is a ConfigError, and BOTH map to the same fail-closed contract code.
    {
        const fs::path dir = make_project("verifytmpl");
        const fs::path runtime = dir / "fake-runtime.bin";
        write_file(runtime, "\x7f\x45\x4c\x46 fake export template bytes");
        const fs::path badsig = dir / "fake-runtime.bin.sig";
        write_file(badsig, "-----BEGIN SSH SIGNATURE-----\nnot-a-real-signature\n-----END SSH SIGNATURE-----\n");
        const fs::path root = dir / "allowed_signers";
        write_file(root, "# a throwaway trust root — the signature is invalid regardless\n");
        const fs::path tar = dir / "dist" / "game.tar";
        const Envelope e = run_build({{"target", "linux"},
                                      {"flavor", "server"},
                                      {"project", dir.string()},
                                      {"out", (dir / "build" / "s.pack").generic_string()},
                                      {"runtime", runtime.string()},
                                      {"runtime-sig", badsig.string()},
                                      {"trust-root", root.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.template_unverified");
        CHECK(!fs::exists(tar)); // the unverified template was never packed
        remove_quiet(dir);
    }

    // --- a08 verify-before-use: a template signature with NO --trust-root fails closed (non-TOFU) ----
    {
        const fs::path dir = make_project("verifynoroot");
        const fs::path runtime = dir / "fake-runtime.bin";
        write_file(runtime, "\x7f\x45\x4c\x46 fake");
        const fs::path badsig = dir / "fake-runtime.bin.sig";
        write_file(badsig, "not-a-signature\n");
        const Envelope e = run_build({{"target", "linux"},
                                      {"project", dir.string()},
                                      {"runtime", runtime.string()},
                                      {"runtime-sig", badsig.string()},
                                      {"emit-artifact", (dir / "dist" / "g.tar").generic_string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.template_unverified");
        remove_quiet(dir);
    }

    // --- a08 verify-before-use: the engine-fetched toolchain fails closed on a bad signature --------
    // A supplied toolchain-artifact signature that does not verify → build.toolchain_fetch_failed
    // (an unverifiable fetch is a failed fetch), refused BEFORE the build runs.
    {
        const fs::path dir = make_project("verifytc");
        const fs::path tc = dir / "toolchain.tar";
        write_file(tc, "fake engine-mirrored toolchain artifact");
        const fs::path badsig = dir / "toolchain.tar.sig";
        write_file(badsig, "not-a-signature\n");
        const fs::path root = dir / "allowed_signers";
        write_file(root, "# throwaway trust root\n");
        const Envelope e = run_build({{"target", "linux"},
                                      {"project", dir.string()},
                                      {"toolchain-artifact", tc.string()},
                                      {"toolchain-sig", badsig.string()},
                                      {"trust-root", root.string()}});
        CHECK(!e.ok());
        CHECK(e.error()->code == "build.toolchain_fetch_failed");
        remove_quiet(dir);
    }

    // --- a08 opt-in: --emit-artifact WITHOUT a signature still emits (unchanged; the verify gate is
    // the ready mechanism, activating only once a --runtime-sig is supplied — CI-safe by construction).
    {
        const fs::path dir = make_project("verifyoptin");
        const fs::path runtime = dir / "fake-runtime.bin";
        write_file(runtime, "\x7f\x45\x4c\x46 unsigned template");
        const fs::path tar = dir / "dist" / "game.tar";
        const Envelope e = run_build({{"target", "linux"},
                                      {"flavor", "server"},
                                      {"project", dir.string()},
                                      {"out", (dir / "build" / "s.pack").generic_string()},
                                      {"runtime", runtime.string()},
                                      {"emit-artifact", tar.generic_string()}});
        CHECK(e.ok()); // no signature ⇒ verification not requested ⇒ unchanged a06 behavior
        CHECK(e.data().at("artifactEmit").at("emitted").as_bool());
        remove_quiet(dir);
    }

    // --- per-verb --help + global --help emit the contract self-description --------------------------
    {
        const Envelope verb_help = run({"build", "--help"});
        CHECK(verb_help.ok());
        CHECK(verb_help.data().at("verb").as_string() == "build");
        CHECK(verb_help.data().at("rpcMethod").as_string() == "build");

        const Envelope global_help = run({"--help"});
        CHECK(global_help.ok());
        CHECK(global_help.data().at("contract").at("verbs").is_array());
    }

    CLI_TEST_MAIN_END();
}

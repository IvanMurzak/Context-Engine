// build-test_signing (M8 task a10) — the pure Authenticode signing hook. Coverage (R-QA-013 happy +
// edge + failure): plan_signing (windows required / other targets not-required); evaluate_signing (the
// signed / unsigned never-silent WARNING states + the build.artifact_unsigned code); and
// pe_has_authenticode_signature over synthetic PE fixtures (a non-empty vs empty Certificate Table on
// both PE32 and PE32+, plus malformed / non-PE inputs). No filesystem, no subprocess, no secret.

#include "context/editor/build/build_errors.h"
#include "context/editor/build/signing.h"

#include "build_test.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace build = context::editor::build;

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void put_le(std::string& b, std::size_t off, std::uint32_t value, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        b[off + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
}

// Build a minimal, well-formed PE/COFF image whose Security data-directory (IMAGE_DIRECTORY_ENTRY_SECURITY,
// index 4) has a non-zero Size iff `signed_` — the exact bit pe_has_authenticode_signature keys off.
// `pe32plus` selects the PE32+ (0x20b) optional-header layout vs PE32 (0x10b).
std::string make_pe(bool signed_, bool pe32plus)
{
    std::string b(0x100, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    constexpr std::uint32_t kPeOff = 0x40;
    put_le(b, 0x3C, kPeOff, 4); // e_lfanew
    b[kPeOff] = 'P';
    b[kPeOff + 1] = 'E';
    // b[kPeOff+2] and +3 stay '\0'.
    const std::size_t opt_off = kPeOff + 4 + 20; // COFF header is 20 bytes
    put_le(b, opt_off, pe32plus ? 0x020b : 0x010b, 2); // optional-header magic
    const std::size_t num_rva_off = opt_off + (pe32plus ? 108 : 92);
    const std::size_t data_dir_off = opt_off + (pe32plus ? 112 : 96);
    put_le(b, num_rva_off, 16, 4); // NumberOfRvaAndSizes (>= 5 so the Security entry exists)
    const std::size_t sec_entry = data_dir_off + 4 * 8; // index 4, 8 bytes/entry
    put_le(b, sec_entry, 0x1000, 4);                    // VirtualAddress (ignored by the presence check)
    put_le(b, sec_entry + 4, signed_ ? 0x2A0 : 0, 4);   // Size — non-zero ⇒ a Certificate Table present
    return b;
}
} // namespace

int main()
{
    // --- plan_signing: windows requires Authenticode; every other target does not --------------------
    {
        const build::SigningPlan win = build::plan_signing("windows");
        CHECK(win.required);
        CHECK(win.target == "windows");
        CHECK(win.method == std::string(build::kSigningMethodAuthenticode));
        CHECK(win.tool == std::string(build::kSigningTool));
        CHECK(win.primary == std::string(build::kSigningPrimaryAzure));
        CHECK(win.fallback == std::string(build::kSigningFallbackDevCert));
        CHECK(win.timestamp_required); // RFC-3161 timestamp mandatory (short-lived certs)

        for (const char* t : {"linux", "macos", "web", "playstation"})
        {
            const build::SigningPlan p = build::plan_signing(t);
            CHECK(!p.required);
            CHECK(!p.timestamp_required);
        }
    }

    // --- evaluate_signing: a not-required target is "not-required", never a warning ------------------
    {
        const build::SigningReport r =
            build::evaluate_signing(build::plan_signing("linux"), {/*requested=*/true, false, true});
        CHECK(!r.required);
        CHECK(r.state == std::string(build::kSigningStateNotRequired));
        CHECK(r.code.empty());
        CHECK(r.warning.empty());
    }

    // --- evaluate_signing: required + signed ⇒ "signed", no warning ----------------------------------
    {
        build::SigningInputs in;
        in.requested = true;
        in.artifact_signed = true;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("windows"), in);
        CHECK(r.required);
        CHECK(r.signed_);
        CHECK(r.state == std::string(build::kSigningStateSigned));
        CHECK(r.code.empty());
        CHECK(r.warning.empty());
        CHECK(r.primary == std::string(build::kSigningPrimaryAzure)); // the plan is echoed
    }

    // --- evaluate_signing: required + requested but UNSIGNED ⇒ explicit never-silent WARNING ----------
    {
        build::SigningInputs in;
        in.requested = true;
        in.artifact_signed = false;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("windows"), in);
        CHECK(r.required);
        CHECK(!r.signed_);
        CHECK(r.state == std::string(build::kSigningStateUnsigned));
        CHECK(r.code == std::string(build::kBuildArtifactUnsignedCode));
        CHECK(!r.warning.empty()); // NEVER silent
        CHECK(contains(r.warning, "NOT Authenticode-signed"));
        CHECK(contains(r.warning, "SmartScreen"));
    }

    // --- evaluate_signing: required but signing NOT requested ⇒ still an explicit unsigned WARNING ----
    {
        build::SigningInputs in;
        in.requested = false;
        in.artifact_signed = false;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("windows"), in);
        CHECK(r.state == std::string(build::kSigningStateUnsigned));
        CHECK(r.code == std::string(build::kBuildArtifactUnsignedCode));
        CHECK(contains(r.warning, "signing was not requested"));
    }

    // --- evaluate_signing: required, binary unreadable ⇒ fail-closed unsigned WARNING -----------------
    {
        build::SigningInputs in;
        in.requested = true;
        in.artifact_signed = false;
        in.binary_available = false;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("windows"), in);
        CHECK(r.state == std::string(build::kSigningStateUnsigned));
        CHECK(contains(r.warning, "could not be read"));
    }

    // --- pe_has_authenticode_signature: a non-empty Certificate Table ⇒ signed (PE32 + PE32+) --------
    {
        CHECK(build::pe_has_authenticode_signature(make_pe(/*signed_=*/true, /*pe32plus=*/false)));
        CHECK(build::pe_has_authenticode_signature(make_pe(/*signed_=*/true, /*pe32plus=*/true)));
        // An empty Security directory ⇒ unsigned.
        CHECK(!build::pe_has_authenticode_signature(make_pe(/*signed_=*/false, /*pe32plus=*/false)));
        CHECK(!build::pe_has_authenticode_signature(make_pe(/*signed_=*/false, /*pe32plus=*/true)));
    }

    // --- pe_has_authenticode_signature: malformed / non-PE inputs are safely "unsigned" --------------
    {
        CHECK(!build::pe_has_authenticode_signature(""));            // empty
        CHECK(!build::pe_has_authenticode_signature("not a pe file")); // no MZ
        CHECK(!build::pe_has_authenticode_signature("MZ"));          // truncated (no PE header)
        std::string mz(0x100, '\0');                                // MZ but no "PE\0\0" at e_lfanew
        mz[0] = 'M';
        mz[1] = 'Z';
        CHECK(!build::pe_has_authenticode_signature(mz));
    }

    BUILD_TEST_MAIN_END();
}

// build-test_signing (M8 tasks a10 + a13) — the pure OS code-signing hook. Coverage (R-QA-013 happy +
// edge + failure): plan_signing (windows Authenticode required / macOS Developer-ID-notarization required
// / other targets not-required); evaluate_signing (the signed / unsigned never-silent WARNING states +
// the build.artifact_unsigned code, with method-aware SmartScreen/Gatekeeper phrasing); and the two pure
// presence parsers — pe_has_authenticode_signature over synthetic PE fixtures (a non-empty vs empty
// Certificate Table on both PE32 and PE32+) and macho_has_code_signature over synthetic Mach-O fixtures (a
// non-empty vs empty LC_CODE_SIGNATURE datasize on a thin 64-bit image + a fat/universal wrapper) — plus
// malformed / non-image inputs for both. No filesystem, no subprocess, no secret.

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

void put_be(std::string& b, std::size_t off, std::uint32_t value, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
        b[off + i] = static_cast<char>((value >> (8 * (n - 1 - i))) & 0xFF);
}

// Build a minimal thin 64-bit little-endian Mach-O (the macOS ARM64/x86_64 case) with a single
// LC_CODE_SIGNATURE (0x1D) load command whose datasize is non-zero iff `signed_` — the exact bit
// macho_has_code_signature keys off. mach_header_64 is 32 bytes; the load command follows it.
std::string make_macho(bool signed_)
{
    std::string b(0x40, '\0');
    put_le(b, 0, 0xFEEDFACFu, 4); // MH_MAGIC_64 stored little-endian (file bytes CF FA ED FE)
    put_le(b, 12, 2, 4);          // filetype MH_EXECUTE (cosmetic for the presence parser)
    put_le(b, 16, 1, 4);          // ncmds = 1
    put_le(b, 20, 16, 4);         // sizeofcmds = 16 (one linkedit_data_command)
    const std::size_t lc = 32;    // load commands begin after the 64-bit mach_header
    put_le(b, lc, 0x1Du, 4);      // cmd = LC_CODE_SIGNATURE
    put_le(b, lc + 4, 16, 4);     // cmdsize = 16
    put_le(b, lc + 8, 0x30, 4);   // dataoff (ignored by the presence check)
    put_le(b, lc + 12, signed_ ? 0x120 : 0, 4); // datasize — non-zero ⇒ a __LINKEDIT signature blob
    return b;
}

// Wrap a signed thin slice in a fat/universal header (FAT_MAGIC — fields BIG-endian). A universal binary
// is signed iff a slice carries a signature (codesign signs every arch slice).
std::string make_fat_signed()
{
    const std::string slice = make_macho(/*signed_=*/true);
    constexpr std::size_t slice_off = 4096; // page-ish offset for the slice payload
    std::string b(slice_off + slice.size(), '\0');
    put_be(b, 0, 0xCAFEBABEu, 4);                                    // FAT_MAGIC (fields big-endian)
    put_be(b, 4, 1, 4);                                              // nfat_arch = 1
    // fat_arch[0] at offset 8: cputype(4) cpusubtype(4) offset(4) size(4) align(4).
    put_be(b, 8 + 8, static_cast<std::uint32_t>(slice_off), 4);      // offset
    put_be(b, 8 + 12, static_cast<std::uint32_t>(slice.size()), 4);  // size
    for (std::size_t i = 0; i < slice.size(); ++i)
        b[slice_off + i] = slice[i];
    return b;
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
    // --- plan_signing: windows requires Authenticode --------------------------------------------------
    {
        const build::SigningPlan win = build::plan_signing("windows");
        CHECK(win.required);
        CHECK(win.target == "windows");
        CHECK(win.method == std::string(build::kSigningMethodAuthenticode));
        CHECK(win.tool == std::string(build::kSigningTool));
        CHECK(win.primary == std::string(build::kSigningPrimaryAzure));
        CHECK(win.fallback == std::string(build::kSigningFallbackDevCert));
        CHECK(win.timestamp_required); // RFC-3161 timestamp mandatory (short-lived certs)
        CHECK(!win.notarization_required); // notarization is a macOS-only requirement
    }

    // --- plan_signing: macOS requires Developer ID + notarization (a13) ------------------------------
    {
        const build::SigningPlan mac = build::plan_signing("macos");
        CHECK(mac.required);
        CHECK(mac.target == "macos");
        CHECK(mac.method == std::string(build::kSigningMethodDeveloperId));
        CHECK(mac.tool == std::string(build::kSigningToolCodesign));
        CHECK(mac.primary == std::string(build::kSigningPrimaryAppleNotary));
        CHECK(mac.fallback.empty()); // no v1 fallback — only the API-key notary path ships
        CHECK(mac.timestamp_required);    // codesign secure timestamp mandatory
        CHECK(mac.notarization_required); // notarytool submit + stapler staple to ship (Gatekeeper)
    }

    // --- plan_signing: linux / web / unknown targets require no code-signing --------------------------
    {
        for (const char* t : {"linux", "web", "playstation"})
        {
            const build::SigningPlan p = build::plan_signing(t);
            CHECK(!p.required);
            CHECK(!p.timestamp_required);
            CHECK(!p.notarization_required);
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

    // --- evaluate_signing (macOS, a13): required + signed ⇒ "signed", notarizationRequired echoed -----
    {
        build::SigningInputs in;
        in.requested = true;
        in.artifact_signed = true;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("macos"), in);
        CHECK(r.required);
        CHECK(r.signed_);
        CHECK(r.state == std::string(build::kSigningStateSigned));
        CHECK(r.code.empty());
        CHECK(r.warning.empty());
        CHECK(r.notarization_required); // notarization is a further ship requirement, echoed on the report
        CHECK(r.primary == std::string(build::kSigningPrimaryAppleNotary));
    }

    // --- evaluate_signing (macOS): required + UNSIGNED ⇒ never-silent Gatekeeper/notarization WARNING --
    {
        build::SigningInputs in;
        in.requested = true;
        in.artifact_signed = false;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("macos"), in);
        CHECK(r.state == std::string(build::kSigningStateUnsigned));
        CHECK(r.code == std::string(build::kBuildArtifactUnsignedCode));
        CHECK(!r.warning.empty()); // NEVER silent
        CHECK(contains(r.warning, "Developer-ID-signed"));
        CHECK(contains(r.warning, "Gatekeeper"));   // macOS gate (NOT SmartScreen)
        CHECK(contains(r.warning, "notarized"));    // notarization is called out
        CHECK(!contains(r.warning, "SmartScreen")); // no Windows phrasing leaks into the macOS warning
    }

    // --- evaluate_signing (macOS): required but signing NOT requested ⇒ still an explicit WARNING ------
    {
        build::SigningInputs in;
        in.requested = false;
        in.artifact_signed = false;
        in.binary_available = true;
        const build::SigningReport r = build::evaluate_signing(build::plan_signing("macos"), in);
        CHECK(r.state == std::string(build::kSigningStateUnsigned));
        CHECK(contains(r.warning, "signing was not requested"));
        CHECK(contains(r.warning, "Gatekeeper"));
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
        // A file-controlled e_lfanew near 0xFFFFFFFF must NOT wrap the pe_off+4 bounds check into an
        // out-of-range index — a malformed header is simply "unsigned", never an OOB read.
        std::string huge_lfanew(0x100, '\0');
        huge_lfanew[0] = 'M';
        huge_lfanew[1] = 'Z';
        put_le(huge_lfanew, 0x3C, 0xFFFFFFFEu, 4); // e_lfanew just below UINT32_MAX
        CHECK(!build::pe_has_authenticode_signature(huge_lfanew));
    }

    // --- macho_has_code_signature (a13): a non-empty LC_CODE_SIGNATURE datasize ⇒ signed --------------
    {
        CHECK(build::macho_has_code_signature(make_macho(/*signed_=*/true)));   // thin 64-bit signed
        CHECK(!build::macho_has_code_signature(make_macho(/*signed_=*/false))); // datasize 0 ⇒ unsigned
        // A fat/universal binary is signed iff a slice carries a signature.
        CHECK(build::macho_has_code_signature(make_fat_signed()));
    }

    // --- macho_has_code_signature: malformed / non-Mach-O inputs are safely "unsigned" ---------------
    {
        CHECK(!build::macho_has_code_signature(""));               // empty
        CHECK(!build::macho_has_code_signature("not a mach-o"));   // wrong magic
        CHECK(!build::macho_has_code_signature("\xCF\xFA"));       // truncated magic
        // Valid 64-bit magic but ncmds claims a load command past the end of the buffer — bounds-checked
        // to "unsigned", never an OOB read.
        std::string truncated(0x20, '\0');
        put_le(truncated, 0, 0xFEEDFACFu, 4);
        put_le(truncated, 16, 4, 4);  // ncmds = 4 (but there are no load-command bytes)
        put_le(truncated, 20, 64, 4); // sizeofcmds
        CHECK(!build::macho_has_code_signature(truncated));
        // A fat header whose nfat_arch is absurdly large must not be trusted — capped, reported unsigned.
        std::string bad_fat(0x40, '\0');
        put_be(bad_fat, 0, 0xCAFEBABEu, 4);
        put_be(bad_fat, 4, 0xFFFFFFFFu, 4); // nfat_arch = UINT32_MAX
        CHECK(!build::macho_has_code_signature(bad_fat));
    }

    BUILD_TEST_MAIN_END();
}

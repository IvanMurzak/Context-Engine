// The Authenticode code-signing hook — the pure plan + report + PE-signature presence parser (see
// signing.h). No filesystem, no subprocess, no secret: the CLI supplies the observed signing state and
// this module computes the machine-readable, never-silent signing report.

#include "context/editor/build/signing.h"

#include "context/editor/build/build_errors.h"

#include <cstddef>
#include <cstdint>

namespace context::editor::build
{

namespace
{

// Read a little-endian unsigned integer of `n` bytes (n ∈ {2,4}) at `off`. Returns false (out=0) when
// the read would run past the end of `bytes` — every PE field access is bounds-checked, so a truncated
// or malformed header is simply "no signature", never an out-of-range read.
[[nodiscard]] bool read_le(std::string_view bytes, std::size_t off, std::size_t n, std::uint32_t& out)
{
    out = 0;
    if (off + n > bytes.size())
        return false;
    for (std::size_t i = 0; i < n; ++i)
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off + i])) << (8 * i);
    return true;
}

// The IMAGE_DIRECTORY_ENTRY_SECURITY index in the optional header's data directory (the Certificate
// Table — where an Authenticode WIN_CERTIFICATE blob is referenced).
constexpr std::uint32_t kSecurityDirIndex = 4;
constexpr std::uint16_t kPe32Magic = 0x010b;
constexpr std::uint16_t kPe32PlusMagic = 0x020b;

} // namespace

bool pe_has_authenticode_signature(std::string_view pe_bytes)
{
    // MZ DOS header.
    if (pe_bytes.size() < 0x40 || static_cast<unsigned char>(pe_bytes[0]) != 'M' ||
        static_cast<unsigned char>(pe_bytes[1]) != 'Z')
        return false;

    // e_lfanew (offset 0x3C) → the PE header offset.
    std::uint32_t pe_off = 0;
    if (!read_le(pe_bytes, 0x3C, 4, pe_off))
        return false;
    // "PE\0\0" signature.
    if (pe_off + 4 > pe_bytes.size() || pe_bytes[pe_off] != 'P' || pe_bytes[pe_off + 1] != 'E' ||
        pe_bytes[pe_off + 2] != '\0' || pe_bytes[pe_off + 3] != '\0')
        return false;

    // The COFF header is 20 bytes after "PE\0\0"; the optional header follows it.
    const std::size_t opt_off = static_cast<std::size_t>(pe_off) + 4 + 20;
    std::uint32_t magic = 0;
    if (!read_le(pe_bytes, opt_off, 2, magic))
        return false;

    // The data directory location depends on the optional-header magic (PE32 vs PE32+): NumberOfRvaAndSizes
    // sits at opt_off+92 (PE32) / opt_off+108 (PE32+); the DataDirectory array begins right after it.
    std::size_t num_rva_off = 0;
    std::size_t data_dir_off = 0;
    if (magic == kPe32Magic)
    {
        num_rva_off = opt_off + 92;
        data_dir_off = opt_off + 96;
    }
    else if (magic == kPe32PlusMagic)
    {
        num_rva_off = opt_off + 108;
        data_dir_off = opt_off + 112;
    }
    else
    {
        return false; // not a recognized PE optional header
    }

    std::uint32_t num_rva = 0;
    if (!read_le(pe_bytes, num_rva_off, 4, num_rva))
        return false;
    if (num_rva <= kSecurityDirIndex) // no Certificate Table entry in this image
        return false;

    // Each data-directory entry is 8 bytes: VirtualAddress(4) + Size(4). A non-zero Size on the Security
    // entry means a Certificate Table (the Authenticode signature blob) is present.
    const std::size_t sec_entry = data_dir_off + static_cast<std::size_t>(kSecurityDirIndex) * 8;
    std::uint32_t cert_size = 0;
    if (!read_le(pe_bytes, sec_entry + 4, 4, cert_size))
        return false;
    return cert_size != 0;
}

SigningPlan plan_signing(std::string_view target)
{
    SigningPlan plan;
    plan.target = std::string(target);
    // The v1 code-signing prerequisite: Windows Authenticode. Linux / web / (macOS handled by a13) have
    // no v1 Authenticode leg here — required stays false (the honest not-required plan).
    if (target == "windows")
    {
        plan.required = true;
        plan.method = kSigningMethodAuthenticode;
        plan.tool = kSigningTool;
        plan.primary = kSigningPrimaryAzure;
        plan.fallback = kSigningFallbackDevCert;
        plan.timestamp_required = true;
    }
    return plan;
}

SigningReport evaluate_signing(const SigningPlan& plan, const SigningInputs& inputs)
{
    SigningReport r;
    r.required = plan.required;
    r.requested = inputs.requested;
    r.method = plan.method;
    r.tool = plan.tool;
    r.primary = plan.primary;
    r.fallback = plan.fallback;
    r.timestamp_required = plan.timestamp_required;

    if (!plan.required)
    {
        // No code-signing prerequisite for this target — nothing to warn about.
        r.state = kSigningStateNotRequired;
        r.signed_ = false;
        return r;
    }

    r.signed_ = inputs.artifact_signed;
    if (inputs.artifact_signed)
    {
        r.state = kSigningStateSigned;
        return r;
    }

    // Required but NOT signed — an EXPLICIT, never-silent warning state (DoD). This covers both the
    // "signing not requested" case and the "requested but no signing identity was available" case (a
    // fork PR with no secrets): either way the shipped artifact is unsigned and the operator must see it.
    r.state = kSigningStateUnsigned;
    r.code = std::string(kBuildArtifactUnsignedCode);
    if (!inputs.binary_available)
    {
        r.warning = "the " + plan.target +
                    " runtime binary could not be read to check its Authenticode signature; treat the "
                    "artifact as UNSIGNED (fail-closed) — sign it via " +
                    plan.primary + " (or the " + plan.fallback + " fallback) before shipping";
    }
    else if (!inputs.requested)
    {
        r.warning = "the " + plan.target +
                    " artifact is NOT Authenticode-signed and signing was not requested (--sign); this "
                    "binary will trip SmartScreen and must be signed via " +
                    plan.primary + " (or the " + plan.fallback + " fallback) before shipping";
    }
    else
    {
        r.warning = "the " + plan.target +
                    " artifact is NOT Authenticode-signed (no signing identity was available); this "
                    "binary will trip SmartScreen and must be signed via " +
                    plan.primary + " (or the " + plan.fallback +
                    " fallback), with a mandatory RFC-3161 timestamp, before shipping";
    }
    return r;
}

} // namespace context::editor::build

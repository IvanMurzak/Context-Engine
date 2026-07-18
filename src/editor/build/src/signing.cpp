// The Authenticode code-signing hook — the pure plan + report + PE-signature presence parser (see
// signing.h). No filesystem, no subprocess, no secret: the CLI supplies the observed signing state and
// this module computes the machine-readable, never-silent signing report.

#include "context/editor/build/signing.h"

#include "context/editor/build/build_errors.h"
#include "context/editor/build/doctor.h" // signing_requirements — the shared target→requirement list

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    // "PE\0\0" signature. Widen pe_off to std::size_t BEFORE adding 4: pe_off is a file-controlled
    // std::uint32_t, so `pe_off + 4` would wrap in 32-bit arithmetic (e_lfanew near 0xFFFFFFFF), pass the
    // bounds check, then index ~4 GB out of range — the size_t cast keeps the whole read bounds-checked.
    if (static_cast<std::size_t>(pe_off) + 4 > pe_bytes.size() || pe_bytes[pe_off] != 'P' ||
        pe_bytes[pe_off + 1] != 'E' || pe_bytes[pe_off + 2] != '\0' || pe_bytes[pe_off + 3] != '\0')
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

namespace
{

// Mach-O magic bytes AS READ big-endian from file offset 0 (so a little-endian macOS binary — file bytes
// CF FA ED FE — reads here as 0xCFFAEDFE = the CIGAM64 constant). A thin Mach-O stores its header in its
// OWN endianness (MAGIC = same-endian, CIGAM = byte-swapped); a fat/universal header stores its fields
// big-endian (FAT_MAGIC) or little-endian (FAT_CIGAM).
constexpr std::uint32_t kMachoMagic32 = 0xFEEDFACEu; // 32-bit, header fields big-endian
constexpr std::uint32_t kMachoCigam32 = 0xCEFAEDFEu; // 32-bit, header fields little-endian
constexpr std::uint32_t kMachoMagic64 = 0xFEEDFACFu; // 64-bit, header fields big-endian
constexpr std::uint32_t kMachoCigam64 = 0xCFFAEDFEu; // 64-bit, header fields little-endian (macOS case)
constexpr std::uint32_t kFatMagic = 0xCAFEBABEu;     // fat header, fields big-endian
constexpr std::uint32_t kFatCigam = 0xBEBAFECAu;     // fat header, fields little-endian
constexpr std::uint32_t kLcCodeSignature = 0x1Du;    // LC_CODE_SIGNATURE load command
constexpr std::uint32_t kFatArchLimit = 64;          // sane cap on nfat_arch (guards a crafted header)

// The __LINKEDIT code-signature blob format (referenced by LC_CODE_SIGNATURE's dataoff/datasize). ALL of
// its fields are big-endian regardless of the Mach-O's own endianness. A CS_SuperBlob (magic + length +
// count + `count` {type,offset} index entries) wraps one-or-more sub-blobs; the primary CodeDirectory
// lives in the CSSLOT_CODEDIRECTORY slot and carries the setup/mode flags. CS_ADHOC marks an AD-HOC
// signature — one with NO signing identity — which Apple Silicon (arm64) linkers auto-embed at LINK time
// so the binary can execute; it is NOT a distributable / Developer-ID signature, so an ad-hoc-only
// signature must read as UNSIGNED (else every unsigned per-PR arm64 build would look "signed").
constexpr std::uint32_t kCsMagicEmbeddedSignature = 0xFADE0CC0u; // CS_SuperBlob magic
constexpr std::uint32_t kCsMagicCodeDirectory = 0xFADE0C02u;     // CS_CodeDirectory magic
constexpr std::uint32_t kCsSlotCodeDirectory = 0x0u;             // the primary CodeDirectory index slot
constexpr std::uint32_t kCsAdhoc = 0x00000002u;                  // CS_ADHOC bit in CodeDirectory.flags
constexpr std::uint32_t kCsSuperBlobIndexLimit = 64;             // sane cap on the SuperBlob index count

// Read a 4-byte unsigned int at `off` in the given endianness. false (out=0) if the read runs past end —
// every field access is bounds-checked, so a truncated/malformed image is simply "no signature".
[[nodiscard]] bool read_u32(std::string_view bytes, std::size_t off, bool big_endian, std::uint32_t& out)
{
    out = 0;
    if (off + 4 > bytes.size())
        return false;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const std::uint32_t b = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[off + i]));
        out |= big_endian ? (b << (8 * (3 - i))) : (b << (8 * i));
    }
    return true;
}

// Is the __LINKEDIT code-signature blob `sig` (a CS_SuperBlob, or rarely a bare CS_CodeDirectory) a REAL
// distributable signature — i.e. is its primary CodeDirectory present, parseable, and NOT flagged
// CS_ADHOC? Apple Silicon linkers auto-embed an AD-HOC signature into every Mach-O at link time with no
// Developer-ID identity; that must read as UNSIGNED here, else an unsigned per-PR arm64 build looks
// "signed". Every signature-blob field is big-endian (network order), independent of the Mach-O's own
// endianness. Fail-closed: an ad-hoc, malformed, or unparseable blob ⇒ false (never report a
// distributable signature we cannot affirmatively confirm).
[[nodiscard]] bool codesig_is_real_signature(std::string_view sig)
{
    std::uint32_t magic = 0;
    if (!read_u32(sig, 0, /*big_endian=*/true, magic))
        return false;

    // Locate the primary CodeDirectory: usually a SuperBlob whose index points at the CSSLOT_CODEDIRECTORY
    // entry; a bare CodeDirectory (no SuperBlob wrapper) is also accepted (cd_off stays 0).
    std::size_t cd_off = 0;
    if (magic == kCsMagicEmbeddedSignature)
    {
        std::uint32_t count = 0;
        if (!read_u32(sig, 8, /*big_endian=*/true, count) || count == 0 ||
            count > kCsSuperBlobIndexLimit)
            return false;
        bool found = false;
        for (std::uint32_t i = 0; i < count && !found; ++i)
        {
            const std::size_t entry = 12 + static_cast<std::size_t>(i) * 8; // CS_BlobIndex is 8 bytes
            std::uint32_t type = 0;
            std::uint32_t off = 0;
            if (!read_u32(sig, entry, /*big_endian=*/true, type) ||
                !read_u32(sig, entry + 4, /*big_endian=*/true, off))
                return false;
            if (type == kCsSlotCodeDirectory)
            {
                cd_off = off; // offset is relative to the SuperBlob (== `sig`) start
                found = true;
            }
        }
        if (!found)
            return false;
    }
    else if (magic != kCsMagicCodeDirectory)
    {
        return false; // not a recognized code-signature blob
    }

    // CS_CodeDirectory: magic(0) length(4) version(8) flags(12) … — only magic + flags are inspected.
    std::uint32_t cd_magic = 0;
    if (!read_u32(sig, cd_off, /*big_endian=*/true, cd_magic) || cd_magic != kCsMagicCodeDirectory)
        return false;
    std::uint32_t flags = 0;
    if (!read_u32(sig, cd_off + 12, /*big_endian=*/true, flags))
        return false;
    return (flags & kCsAdhoc) == 0; // an ad-hoc signature is NOT a distributable signature
}

// Does ONE thin Mach-O slice (starting at offset 0 of `slice`) carry an LC_CODE_SIGNATURE with a
// non-empty signature blob? `is_64`/`big_endian` are decided by the caller from the slice's magic. The
// mach_header is magic(4) cputype(4) cpusubtype(4) filetype(4) ncmds(4) sizeofcmds(4) flags(4)
// [reserved(4) — 64-bit only]; the load commands begin right after.
[[nodiscard]] bool thin_macho_signed(std::string_view slice, bool is_64, bool big_endian)
{
    std::uint32_t ncmds = 0;
    if (!read_u32(slice, 16, big_endian, ncmds)) // ncmds sits at header offset 16 in both bit-widths
        return false;
    std::size_t cmd_off = is_64 ? 32 : 28; // load commands begin after the (64/32-bit) mach_header
    for (std::uint32_t i = 0; i < ncmds; ++i)
    {
        std::uint32_t cmd = 0;
        std::uint32_t cmdsize = 0;
        if (!read_u32(slice, cmd_off, big_endian, cmd) ||
            !read_u32(slice, cmd_off + 4, big_endian, cmdsize))
            return false;
        if (cmdsize < 8) // a load_command is at least cmd(4)+cmdsize(4); a smaller size is malformed
            return false;
        if (cmd == kLcCodeSignature)
        {
            // linkedit_data_command: cmd(4) cmdsize(4) dataoff(4) datasize(4). A non-zero datasize ⇒ an
            // embedded __LINKEDIT signature blob is present; it counts as a signature only when its
            // primary CodeDirectory is NOT ad-hoc (arm64 auto-embeds an ad-hoc signature at link time —
            // parse the blob rather than trusting mere presence). There is one LC_CODE_SIGNATURE per
            // image, so this command's verdict is final.
            std::uint32_t dataoff = 0;
            std::uint32_t datasize = 0;
            if (!read_u32(slice, cmd_off + 8, big_endian, dataoff) ||
                !read_u32(slice, cmd_off + 12, big_endian, datasize))
                return false;
            if (datasize != 0)
            {
                // The blob spans [dataoff, dataoff+datasize) — reject an overrun (or the 32-bit-size_t
                // overflow, `end < dataoff`, mirroring this file's `cmd_off + cmdsize < cmd_off` idiom)
                // before viewing it. datasize != 0 here, so `end > dataoff`, making a separate
                // `dataoff > slice.size()` test redundant.
                const std::size_t end = static_cast<std::size_t>(dataoff) + datasize;
                if (end < dataoff || end > slice.size())
                    return false; // the signature blob overruns the slice — fail-closed (unsigned)
                return codesig_is_real_signature(slice.substr(dataoff, datasize));
            }
        }
        if (cmd_off + cmdsize < cmd_off) // overflow guard on a file-controlled cmdsize
            return false;
        cmd_off += cmdsize;
        if (cmd_off > slice.size())
            return false;
    }
    return false;
}

// Interpret a single thin Mach-O slice: decide bit-width + endianness from its magic (a nested fat is
// rejected — a fat cannot contain a fat, so there is NO recursion), then scan its load commands.
[[nodiscard]] bool thin_slice_signed(std::string_view slice)
{
    std::uint32_t magic_be = 0;
    if (!read_u32(slice, 0, /*big_endian=*/true, magic_be))
        return false;
    bool is_64 = false;
    bool big_endian = false;
    if (magic_be == kMachoMagic64) { is_64 = true; big_endian = true; }
    else if (magic_be == kMachoCigam64) { is_64 = true; big_endian = false; }
    else if (magic_be == kMachoMagic32) { is_64 = false; big_endian = true; }
    else if (magic_be == kMachoCigam32) { is_64 = false; big_endian = false; }
    else return false; // not a thin Mach-O
    return thin_macho_signed(slice, is_64, big_endian);
}

} // namespace

bool macho_has_code_signature(std::string_view macho_bytes)
{
    std::uint32_t magic_be = 0;
    if (!read_u32(macho_bytes, 0, /*big_endian=*/true, magic_be))
        return false;

    // Fat/universal binary: a fat_header (magic + nfat_arch), then nfat_arch fat_arch entries — each
    // cputype(4) cpusubtype(4) offset(4) size(4) align(4) = 20 bytes — pointing at a nested thin Mach-O.
    // A universal binary is signed iff a slice carries a signature (`codesign` signs every arch slice).
    if (magic_be == kFatMagic || magic_be == kFatCigam)
    {
        const bool fat_be = (magic_be == kFatMagic);
        std::uint32_t nfat = 0;
        if (!read_u32(macho_bytes, 4, fat_be, nfat) || nfat == 0 || nfat > kFatArchLimit)
            return false;
        for (std::uint32_t i = 0; i < nfat; ++i)
        {
            const std::size_t arch_off = 8 + static_cast<std::size_t>(i) * 20;
            std::uint32_t off = 0;
            std::uint32_t size = 0;
            if (!read_u32(macho_bytes, arch_off + 8, fat_be, off) ||
                !read_u32(macho_bytes, arch_off + 12, fat_be, size))
                return false;
            if (size == 0 || static_cast<std::size_t>(off) + size > macho_bytes.size())
                continue; // a slice that overruns the file is skipped (fail-safe), never an OOB read
            if (thin_slice_signed(macho_bytes.substr(off, size)))
                return true;
        }
        return false;
    }

    // A thin (single-arch) Mach-O.
    return thin_slice_signed(macho_bytes);
}

SigningPlan plan_signing(std::string_view target)
{
    SigningPlan plan;
    plan.target = std::string(target);
    // The v1 code-signing prerequisites: Windows Authenticode (a10) and macOS Developer ID + notarization
    // (a13). Key the required-decision off the SHARED target→requirement enumeration
    // (signing_requirements, doctor.*) so plan_signing and the doctor probe cannot drift — a single
    // source of truth for "which targets sign what". The method-specific method/tool/primary/fallback
    // fields are filled here.
    const std::vector<std::string> reqs = signing_requirements(target);
    if (std::find(reqs.begin(), reqs.end(), kSigningMethodAuthenticode) != reqs.end())
    {
        plan.required = true;
        plan.method = kSigningMethodAuthenticode;
        plan.tool = kSigningTool;
        plan.primary = kSigningPrimaryAzure;
        plan.fallback = kSigningFallbackDevCert;
        plan.timestamp_required = true;
    }
    else if (std::find(reqs.begin(), reqs.end(), kSigningMethodDeveloperId) != reqs.end())
    {
        // macOS (a13): `codesign` with a Developer ID Application identity produces the detectable
        // signature (hardened runtime + secure timestamp), then `notarytool` submits it to Apple's notary
        // service (App-Store-Connect API-key primary path) and the ticket is stapled. No v1 fallback —
        // only the API-key notary path ships. macOS 15+ Gatekeeper hard-blocks an un-notarized build.
        plan.required = true;
        plan.method = kSigningMethodDeveloperId;
        plan.tool = kSigningToolCodesign;
        plan.primary = kSigningPrimaryAppleNotary;
        plan.fallback = ""; // no v1 fallback (developer-id-application-cert IS the identity)
        plan.timestamp_required = true;     // codesign --timestamp (secure timestamp) mandatory
        plan.notarization_required = true;  // notarytool submit + stapler staple to ship
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
    r.notarization_required = plan.notarization_required;

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

    // Method-aware phrasing: Windows Authenticode / SmartScreen vs macOS Developer-ID / Gatekeeper (+ the
    // further notarization requirement). Both echo the machine-branchable primary signing path.
    const bool developer_id = (plan.method == kSigningMethodDeveloperId);
    const std::string sign_kind = developer_id ? "Developer-ID-signed" : "Authenticode-signed";
    const std::string os_gate = developer_id ? "Gatekeeper" : "SmartScreen";
    const std::string ship_reqs =
        developer_id
            ? "code-signed with a Developer ID identity AND notarized + stapled (via " + plan.primary + ")"
            : "signed via " + plan.primary + " (or the " + plan.fallback + " fallback)";

    if (!inputs.binary_available)
    {
        r.warning = "the " + plan.target +
                    " runtime binary could not be read to check its code signature; treat the "
                    "artifact as UNSIGNED (fail-closed) — it must be " +
                    ship_reqs + " before shipping";
    }
    else if (!inputs.requested)
    {
        r.warning = "the " + plan.target + " artifact is NOT " + sign_kind +
                    " and signing was not requested (--sign); this binary will be blocked by " + os_gate +
                    " and must be " + ship_reqs + " before shipping";
    }
    else
    {
        r.warning = "the " + plan.target + " artifact is NOT " + sign_kind +
                    " (no signing identity was available); this binary will be blocked by " + os_gate +
                    " and must be " + ship_reqs +
                    (plan.timestamp_required ? ", with a mandatory secure timestamp," : "") +
                    " before shipping";
    }
    return r;
}

} // namespace context::editor::build

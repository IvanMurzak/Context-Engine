// Binary-sidecar authoring rules (L-33): versioned header codec, $sidecar reference resolution +
// verification, the owner<->sidecar index, and the sidecar-first write / owned-satellite move plans.

#pragma once

#include "context/editor/filesync/intent_log.h"
#include "context/editor/serializer/sidecar_ref.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::filesync
{

class FileStore;

// --- the versioned sidecar header (L-33: "versioned binary sidecars") ---------------------------
//
// Every sidecar file begins with a fixed 12-byte header: an 8-byte magic followed by a 4-byte
// little-endian format version (pinned LE so sidecar bytes are identical across platforms — the
// whole-file raw-byte hash must agree everywhere, R-FILE-001). The magic starts with 'C' and embeds
// a NUL, so a sidecar can NEVER parse as JSON: serializer::canonicalize() therefore takes its
// non-JSON path by construction and the canonical hash EQUALS the raw-byte hash (the R-FILE-001
// sidecar rule — binaries have no canonicalization pass).
inline constexpr char sidecar_magic[8] = {'C', 'T', 'X', 'S', 'I', 'D', 'E', '\0'};
inline constexpr std::uint32_t sidecar_format_version = 1;
inline constexpr std::size_t sidecar_header_size = 12;

// Encode `payload` as sidecar bytes: magic + LE version + payload verbatim.
[[nodiscard]] std::string encode_sidecar(std::string_view payload,
                                         std::uint32_t version = sidecar_format_version);

// The outcome of decoding sidecar bytes. On failure `error_code` is the catalog code
// (sidecar.truncated / sidecar.bad_magic / sidecar.unsupported_version — R-CLI-008).
struct SidecarDecodeResult
{
    bool ok = false;
    std::string error_code;
    std::uint32_t version = 0;      // meaningful when the header was readable
    std::string_view payload;       // view into the input bytes (valid while they live)
};

// Decode + validate the header. Versions 1..sidecar_format_version are readable; anything newer (or
// 0) is sidecar.unsupported_version — never a best-effort parse (L-37 spirit).
[[nodiscard]] SidecarDecodeResult decode_sidecar(std::string_view bytes);

// True when `bytes` begin with the sidecar magic — the cheap classifier the orphan sweep and the
// merge layer use. (R-FILE-012(d): sidecars are NEVER content-merged — whole-file ours/theirs; this
// predicate is how that layer recognizes one.)
[[nodiscard]] bool is_sidecar_bytes(std::string_view bytes);

// The canonical JSON encoding of a sidecar hash: a decimal string (a full-range 64-bit hash exceeds
// 2^53, so a JSON number cannot round-trip it — the rawHash/canonicalHash RPC convention). The
// strict inverse lives in serializer::parse_hash_string.
[[nodiscard]] std::string format_sidecar_hash(std::uint64_t hash);

// --- reference resolution + the R-SEC-008 jail ---------------------------------------------------

// Resolve an authored "$sidecar" relpath against its owner's directory into a normalized logical
// path. nullopt when the ref is not confinable: absolute (leading '/' or a drive letter), or its
// normalized join escapes `root` (R-SEC-008 — absolute paths and `..` escapes are validation
// errors). Purely lexical over the seam; the TOCTOU-safe physical jail (PR #49) is enforced by
// NativeFileStore on every write-side operation these plans route through.
[[nodiscard]] std::optional<std::string>
resolve_sidecar_path(std::string_view root, std::string_view owner_path, std::string_view relpath);

// One machine-readable sidecar finding (R-FILE-003 shape, code from the R-CLI-008 catalog:
// sidecar.* / path.jail_violation / file.parse_error / usage.invalid).
struct SidecarDiagnostic
{
    std::string code;
    std::string owner;   // the referencing JSON file ("" for an ownerless finding, e.g. an orphan)
    std::string sidecar; // the sidecar path (resolved when possible, else as authored)
    std::string message;
};

// One authored ref together with its resolved logical path.
struct ScannedSidecarRef
{
    serializer::SidecarRef ref; // as authored (relpath + declared hash + JSON pointer)
    std::string resolved_path;  // normalized root-relative logical path (jail-checked)
};

// The result of scanning one owner document for sidecar references (the serializer integration
// seam: parse + collect + resolve + jail-check in one pass).
struct SidecarScan
{
    bool owner_parsed = false;           // owner bytes parsed as strict JSON
    std::vector<ScannedSidecarRef> refs; // well-formed refs whose resolution stayed inside the jail
    std::vector<SidecarDiagnostic> diagnostics; // ref_malformed + path.jail_violation findings
};

// Parse `owner_bytes` and scan for refs. Non-JSON owner bytes yield owner_parsed == false with no
// refs and no diagnostics (a binary/TS carve-out file simply has no $sidecar refs; its parse
// diagnostics are the derivation layer's concern).
[[nodiscard]] SidecarScan scan_sidecar_refs(std::string_view root, std::string_view owner_path,
                                            std::string_view owner_bytes);

// Verify each resolved ref against disk (the R-FILE-003 diagnostics surface): a missing sidecar is
// sidecar.dangling_ref; unreadable headers surface their decode code (sidecar.truncated /
// bad_magic / unsupported_version); declared-hash != whole-file raw-byte hash is
// sidecar.hash_mismatch (the L-33 hash covers the ENTIRE file, header included — exactly the bytes
// watch/reconcile and canonicalize() hash).
[[nodiscard]] std::vector<SidecarDiagnostic>
verify_sidecar_refs(const FileStore& fs, std::string_view owner_path,
                    const std::vector<ScannedSidecarRef>& refs);

// --- owner <-> sidecar index (feeds the sidecar-aware reconcile pipeline) ------------------------

// Bidirectional owner<->sidecar map over normalized logical paths. Deterministic iteration
// (std::map) so diagnostics and expansions are stable. Rebuildable at any time from owner scans —
// like the reconcile index, it is derived state, never truth.
class SidecarIndex
{
public:
    // Replace `owner`'s outgoing refs with `sidecar_paths` (normalized; duplicates collapse).
    void set_owner_refs(std::string_view owner, std::vector<std::string> sidecar_paths);
    void remove_owner(std::string_view owner);

    [[nodiscard]] std::vector<std::string> owners_of(std::string_view sidecar_path) const;
    [[nodiscard]] std::vector<std::string> sidecars_of(std::string_view owner) const;
    [[nodiscard]] bool is_sidecar(std::string_view path) const;
    [[nodiscard]] bool has_owner(std::string_view owner) const;
    [[nodiscard]] std::size_t owner_count() const noexcept { return owner_to_sidecars_.size(); }

private:
    std::map<std::string, std::vector<std::string>> owner_to_sidecars_;
    std::map<std::string, std::vector<std::string>> sidecar_to_owners_;
};

// Sweep `root` for sidecar-classified files (magic sniff) that no indexed owner references —
// sidecar.orphaned diagnostics (R-FILE-003). An explicit validation pass (reads each candidate
// file), NOT part of the per-pass reconcile hot path.
[[nodiscard]] std::vector<SidecarDiagnostic>
find_orphaned_sidecars(const FileStore& fs, std::string_view root, const SidecarIndex& index);

// --- write / move planning (L-33 write order + owned satellites, over the R-FILE-004 machinery) --

// A planned multi-file operation, ready for WriteQueue::execute (which stages it through the
// crash-recovery intent log: fsync-before / per-file-atomic / clear-after, resume-or-diagnose on
// the next incarnation — R-FILE-004). `ok == false` means planning REFUSED; `diagnostics` name why.
struct SidecarPlan
{
    bool ok = false;
    std::vector<SidecarDiagnostic> diagnostics;
    std::vector<PlannedWrite> steps; // dependency-safe order; CAS hashes measured at planning time
};

// One staged sidecar for a family write: its resolved logical path + the FULL encoded bytes
// (header included — encode_sidecar output).
struct StagedSidecar
{
    std::string path;
    std::string bytes;
};

// Plan writing an owner document together with its sidecars, sidecar-FIRST (L-33: every sidecar is
// durable before the referencing JSON write). Planning REFUSES (ok == false) when the family is
// incoherent, so the daemon can never durably author a dangling or lying reference:
//   - a staged path escapes the jail (path.jail_violation);
//   - `owner_bytes` does not parse as JSON (file.parse_error — an owner carrying $sidecar refs is
//     authored JSON by definition);
//   - a staged sidecar is not referenced by `owner_bytes`, or is referenced with a hash that does
//     not match the staged bytes (sidecar.hash_mismatch);
//   - a ref resolves to a path that is neither staged nor already on disk (sidecar.dangling_ref) —
//     external files may be temporarily inconsistent (R-FILE-003), but the daemon's OWN write path
//     must never create the inconsistency.
[[nodiscard]] SidecarPlan plan_sidecar_family_write(const FileStore& fs, std::string_view root,
                                                    std::string_view owner_path,
                                                    std::string_view owner_bytes,
                                                    const std::vector<StagedSidecar>& sidecars);

// Plan moving/renaming an owner document, carrying its sidecars with it (L-33 owned satellites).
// Each referenced sidecar keeps its owner-relative relpath at the destination, so the owner bytes
// move VERBATIM (refs stay valid; identity-bearing content never rewrites on move — L-36 spirit).
// Step order is the R-FILE-004 dependency-safe order:
//   write dest sidecars -> write dest owner -> remove src owner -> remove src sidecars
// so no observable mid-state has a referencing JSON whose sidecar is missing (crash mid-plan leaves
// only unreferenced-copy states, which resume completes — R-QA-010 crash windows prove it).
// Planning refuses when src is missing, dest exists, a carried path escapes the jail, or a dest
// sidecar exists with DIFFERENT bytes; a DANGLING ref is skipped with its diagnostic (the move
// neither fixes nor worsens a pre-existing inconsistency). An unparseable owner moves alone,
// flagged file.parse_error (advisory — refs unknowable). A same-path move is a no-op plan.
[[nodiscard]] SidecarPlan plan_owner_move(const FileStore& fs, std::string_view root,
                                          std::string_view owner_src, std::string_view owner_dest);

} // namespace context::editor::filesync

// Binary-sidecar authoring rules implementation (see sidecar.h).

#include "context/editor/filesync/sidecar.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/json_parse.h"

#include <algorithm>
#include <set>
#include <utility>

namespace context::editor::filesync
{
namespace
{

// The raw-byte hash of "no file" — the CAS convention the intent-log recovery uses (a missing file
// reads as the empty string).
[[nodiscard]] std::uint64_t empty_hash() noexcept
{
    return content_hash(std::string_view{});
}

[[nodiscard]] bool is_absolute_like(std::string_view relpath)
{
    if (relpath.empty())
        return true;
    if (relpath.front() == '/' || relpath.front() == '\\')
        return true;
    // A Windows drive-letter prefix ("C:...") is absolute regardless of the separator style.
    if (relpath.size() >= 2 && relpath[1] == ':' &&
        ((relpath[0] >= 'A' && relpath[0] <= 'Z') || (relpath[0] >= 'a' && relpath[0] <= 'z')))
        return true;
    return false;
}

[[nodiscard]] std::string parent_dir(std::string_view normalized_path)
{
    const std::size_t slash = normalized_path.rfind('/');
    if (slash == std::string_view::npos)
        return std::string{};
    return std::string{normalized_path.substr(0, slash)};
}

[[nodiscard]] PlannedWrite make_write(std::string path, std::uint64_t prev_hash, std::string data)
{
    PlannedWrite w;
    w.path = std::move(path);
    w.expected_prev_hash = prev_hash;
    w.target_hash = content_hash(data);
    w.data = std::move(data);
    w.kind = WriteKind::write;
    return w;
}

[[nodiscard]] PlannedWrite make_remove(std::string path, std::uint64_t prev_hash)
{
    PlannedWrite w;
    w.path = std::move(path);
    w.expected_prev_hash = prev_hash;
    w.target_hash = 0; // unused for removals ("applied" == the file is absent)
    w.kind = WriteKind::remove;
    return w;
}

// Refuse any plan that names the same path twice: each entry's planning-time CAS hash is measured
// against the PRE-plan file, so a duplicate would poison crash recovery's re-CAS (the same rule
// EditorKernel::edit_files enforces on its batches).
[[nodiscard]] bool has_duplicate_paths(const std::vector<PlannedWrite>& steps)
{
    std::set<std::string_view> seen;
    for (const PlannedWrite& step : steps)
        if (!seen.insert(step.path).second)
            return true;
    return false;
}

} // namespace

// --- codec ---------------------------------------------------------------------------------------

std::string encode_sidecar(std::string_view payload, std::uint32_t version)
{
    std::string out;
    out.reserve(sidecar_header_size + payload.size());
    out.append(sidecar_magic, sizeof sidecar_magic);
    for (int shift = 0; shift < 32; shift += 8) // little-endian, pinned for cross-platform bytes
        out.push_back(static_cast<char>((version >> shift) & 0xFFU));
    out.append(payload.data(), payload.size());
    return out;
}

SidecarDecodeResult decode_sidecar(std::string_view bytes)
{
    SidecarDecodeResult result;

    const std::string_view magic{sidecar_magic, sizeof sidecar_magic};
    const std::size_t prefix_len = std::min(bytes.size(), magic.size());
    if (bytes.substr(0, prefix_len) != magic.substr(0, prefix_len))
    {
        result.error_code = "sidecar.bad_magic";
        return result;
    }
    if (bytes.size() < sidecar_header_size)
    {
        // The available bytes ARE a proper prefix of a valid header — truncated, not foreign.
        result.error_code = "sidecar.truncated";
        return result;
    }

    std::uint32_t version = 0;
    for (int i = 0; i < 4; ++i)
        version |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[8 + i]))
                   << (8 * i);
    result.version = version;
    if (version == 0 || version > sidecar_format_version)
    {
        // Newer-than-engine (or nonsense) — never a best-effort parse (L-37 spirit).
        result.error_code = "sidecar.unsupported_version";
        return result;
    }

    result.ok = true;
    result.payload = bytes.substr(sidecar_header_size);
    return result;
}

bool is_sidecar_bytes(std::string_view bytes)
{
    const std::string_view magic{sidecar_magic, sizeof sidecar_magic};
    return bytes.size() >= magic.size() && bytes.substr(0, magic.size()) == magic;
}

std::string format_sidecar_hash(std::uint64_t hash)
{
    return std::to_string(hash);
}

// --- reference resolution + jail ------------------------------------------------------------------

std::optional<std::string> resolve_sidecar_path(std::string_view root, std::string_view owner_path,
                                                std::string_view relpath)
{
    if (is_absolute_like(relpath))
        return std::nullopt; // absolute refs are validation errors (R-SEC-008)

    const std::string owner = normalize_path(owner_path);
    const std::string dir = parent_dir(owner);
    const std::string joined =
        normalize_path(dir.empty() ? std::string{relpath} : dir + "/" + std::string{relpath});
    if (!is_inside_jail(root, joined))
        return std::nullopt; // `..` escapes are validation errors (R-SEC-008)
    return joined;
}

SidecarScan scan_sidecar_refs(std::string_view root, std::string_view owner_path,
                              std::string_view owner_bytes)
{
    SidecarScan scan;
    const std::string owner = normalize_path(owner_path);

    const serializer::ParseResult parsed = serializer::parse_json(owner_bytes);
    if (!parsed.ok)
        return scan; // non-JSON owner: no refs here; parse diagnostics are the derivation layer's

    scan.owner_parsed = true;

    std::vector<serializer::Diagnostic> ref_diagnostics;
    const std::vector<serializer::SidecarRef> refs =
        serializer::collect_sidecar_refs(parsed.root, ref_diagnostics);

    for (const serializer::Diagnostic& d : ref_diagnostics)
        scan.diagnostics.push_back(SidecarDiagnostic{d.code, owner, "", d.message});

    for (const serializer::SidecarRef& ref : refs)
    {
        if (std::optional<std::string> resolved = resolve_sidecar_path(root, owner, ref.relpath))
        {
            scan.refs.push_back(ScannedSidecarRef{ref, std::move(*resolved)});
            continue;
        }
        scan.diagnostics.push_back(SidecarDiagnostic{
            "path.jail_violation", owner, ref.relpath,
            ref.json_pointer + ": \"$sidecar\" path escapes the project root (absolute paths and "
                               "`..` escapes are refused, R-SEC-008): " +
                ref.relpath});
    }
    return scan;
}

std::vector<SidecarDiagnostic> verify_sidecar_refs(const FileStore& fs, std::string_view owner_path,
                                                   const std::vector<ScannedSidecarRef>& refs)
{
    std::vector<SidecarDiagnostic> diagnostics;
    const std::string owner = normalize_path(owner_path);

    for (const ScannedSidecarRef& scanned : refs)
    {
        const std::optional<std::string> bytes = fs.read(scanned.resolved_path);
        if (!bytes)
        {
            diagnostics.push_back(SidecarDiagnostic{
                "sidecar.dangling_ref", owner, scanned.resolved_path,
                scanned.ref.json_pointer + ": referenced sidecar does not exist"});
            continue;
        }

        const SidecarDecodeResult decoded = decode_sidecar(*bytes);
        if (!decoded.ok)
        {
            diagnostics.push_back(SidecarDiagnostic{
                decoded.error_code, owner, scanned.resolved_path,
                scanned.ref.json_pointer + ": sidecar header failed to decode"});
            continue;
        }

        // The L-33 hash covers the ENTIRE file (header included) — exactly the bytes the
        // watch/reconcile pipeline hashes and canonicalize() passes through raw (R-FILE-001:
        // raw-byte hash == canonical hash for sidecars).
        const std::uint64_t actual = content_hash(*bytes);
        if (actual != scanned.ref.hash)
        {
            diagnostics.push_back(SidecarDiagnostic{
                "sidecar.hash_mismatch", owner, scanned.resolved_path,
                scanned.ref.json_pointer + ": declared " + format_sidecar_hash(scanned.ref.hash) +
                    " but the sidecar's raw bytes hash to " + format_sidecar_hash(actual)});
        }
    }
    return diagnostics;
}

// --- owner <-> sidecar index ----------------------------------------------------------------------

void SidecarIndex::set_owner_refs(std::string_view owner, std::vector<std::string> sidecar_paths)
{
    const std::string key = normalize_path(owner);
    remove_owner(key);

    std::vector<std::string> normalized;
    normalized.reserve(sidecar_paths.size());
    for (std::string& path : sidecar_paths)
        normalized.push_back(normalize_path(path));
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

    for (const std::string& sidecar : normalized)
    {
        std::vector<std::string>& owners = sidecar_to_owners_[sidecar];
        if (std::find(owners.begin(), owners.end(), key) == owners.end())
            owners.push_back(key);
    }
    if (!normalized.empty())
        owner_to_sidecars_[key] = std::move(normalized);
}

void SidecarIndex::remove_owner(std::string_view owner)
{
    const std::string key = normalize_path(owner);
    const auto it = owner_to_sidecars_.find(key);
    if (it == owner_to_sidecars_.end())
        return;
    for (const std::string& sidecar : it->second)
    {
        const auto rev = sidecar_to_owners_.find(sidecar);
        if (rev == sidecar_to_owners_.end())
            continue;
        rev->second.erase(std::remove(rev->second.begin(), rev->second.end(), key),
                          rev->second.end());
        if (rev->second.empty())
            sidecar_to_owners_.erase(rev);
    }
    owner_to_sidecars_.erase(it);
}

std::vector<std::string> SidecarIndex::owners_of(std::string_view sidecar_path) const
{
    const auto it = sidecar_to_owners_.find(normalize_path(sidecar_path));
    return it == sidecar_to_owners_.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> SidecarIndex::sidecars_of(std::string_view owner) const
{
    const auto it = owner_to_sidecars_.find(normalize_path(owner));
    return it == owner_to_sidecars_.end() ? std::vector<std::string>{} : it->second;
}

bool SidecarIndex::is_sidecar(std::string_view path) const
{
    return sidecar_to_owners_.find(normalize_path(path)) != sidecar_to_owners_.end();
}

bool SidecarIndex::has_owner(std::string_view owner) const
{
    return owner_to_sidecars_.find(normalize_path(owner)) != owner_to_sidecars_.end();
}

std::vector<SidecarDiagnostic> find_orphaned_sidecars(const FileStore& fs, std::string_view root,
                                                      const SidecarIndex& index)
{
    std::vector<SidecarDiagnostic> diagnostics;
    for (const std::string& path : fs.list(root))
    {
        if (is_control_path(path))
            continue; // engine-internal (.editor/, atomic staging residue) — never authored content
        if (index.is_sidecar(path))
            continue; // referenced by at least one indexed owner
        const std::optional<std::string> bytes = fs.read(path);
        if (!bytes || !is_sidecar_bytes(*bytes))
            continue;
        diagnostics.push_back(SidecarDiagnostic{
            "sidecar.orphaned", "", path,
            "sidecar file has no referencing owner (was its owner deleted or its ref removed?)"});
    }
    return diagnostics;
}

// --- write / move planning ------------------------------------------------------------------------

SidecarPlan plan_sidecar_family_write(const FileStore& fs, std::string_view root,
                                      std::string_view owner_path, std::string_view owner_bytes,
                                      const std::vector<StagedSidecar>& sidecars)
{
    SidecarPlan plan;
    const std::string owner = normalize_path(owner_path);

    if (!is_inside_jail(root, owner))
    {
        plan.diagnostics.push_back(SidecarDiagnostic{"path.jail_violation", owner, "",
                                                     "owner path escapes the project root"});
        return plan;
    }

    const SidecarScan scan = scan_sidecar_refs(root, owner, owner_bytes);
    if (!scan.owner_parsed)
    {
        plan.diagnostics.push_back(SidecarDiagnostic{
            "file.parse_error", owner, "",
            "owner bytes are not valid JSON; a $sidecar-referencing owner is authored JSON"});
        return plan;
    }
    if (!scan.diagnostics.empty())
    {
        plan.diagnostics = scan.diagnostics; // malformed / jail-escaping refs — fix the file first
        return plan;
    }

    // Declared refs by resolved path (a path referenced twice with two hashes is itself malformed).
    std::map<std::string, std::uint64_t> declared;
    for (const ScannedSidecarRef& scanned : scan.refs)
    {
        const auto [it, inserted] = declared.emplace(scanned.resolved_path, scanned.ref.hash);
        if (!inserted && it->second != scanned.ref.hash)
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.ref_malformed", owner, scanned.resolved_path,
                "the same sidecar is referenced with two different hashes"});
            return plan;
        }
    }

    // Staged sidecars: jailed, referenced, and byte-coherent with the declared hash.
    std::set<std::string> staged_paths;
    for (const StagedSidecar& staged : sidecars)
    {
        const std::string path = normalize_path(staged.path);
        if (!is_inside_jail(root, path))
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "path.jail_violation", owner, path, "staged sidecar escapes the project root"});
            return plan;
        }
        const auto it = declared.find(path);
        if (it == declared.end())
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.orphaned", owner, path,
                "staged sidecar is not referenced by the staged owner bytes (writing it would "
                "author an orphan)"});
            return plan;
        }
        const std::uint64_t actual = content_hash(staged.bytes);
        if (actual != it->second)
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.hash_mismatch", owner, path,
                "owner declares " + format_sidecar_hash(it->second) +
                    " but the staged sidecar bytes hash to " + format_sidecar_hash(actual)});
            return plan;
        }
        staged_paths.insert(path);
    }

    // Every ref must resolve at commit time: staged now, or already durable on disk with the
    // declared bytes. The daemon's own write path never authors a dangling or lying reference.
    for (const auto& [path, hash] : declared)
    {
        if (staged_paths.count(path) != 0)
            continue;
        const std::optional<std::string> existing = fs.read(path);
        if (!existing)
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.dangling_ref", owner, path,
                "owner references a sidecar that is neither staged nor on disk"});
            return plan;
        }
        const std::uint64_t actual = content_hash(*existing);
        if (actual != hash)
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.hash_mismatch", owner, path,
                "owner declares " + format_sidecar_hash(hash) +
                    " but the on-disk sidecar hashes to " + format_sidecar_hash(actual)});
            return plan;
        }
    }

    // Sidecar-FIRST order (L-33): every sidecar write precedes the referencing owner write.
    for (const StagedSidecar& staged : sidecars)
    {
        const std::string path = normalize_path(staged.path);
        const std::optional<std::string> current = fs.read(path);
        plan.steps.push_back(
            make_write(path, current ? content_hash(*current) : empty_hash(), staged.bytes));
    }
    {
        const std::optional<std::string> current = fs.read(owner);
        plan.steps.push_back(make_write(owner, current ? content_hash(*current) : empty_hash(),
                                        std::string{owner_bytes}));
    }

    if (has_duplicate_paths(plan.steps))
    {
        plan.steps.clear();
        plan.diagnostics.push_back(SidecarDiagnostic{
            "usage.invalid", owner, "",
            "the plan names the same path twice (duplicates poison crash recovery's re-CAS)"});
        return plan;
    }

    plan.ok = true;
    return plan;
}

SidecarPlan plan_owner_move(const FileStore& fs, std::string_view root, std::string_view owner_src,
                            std::string_view owner_dest)
{
    SidecarPlan plan;
    const std::string src = normalize_path(owner_src);
    const std::string dest = normalize_path(owner_dest);

    for (const std::string& path : {src, dest})
    {
        if (!is_inside_jail(root, path))
        {
            plan.diagnostics.push_back(SidecarDiagnostic{"path.jail_violation", src, path,
                                                         "move endpoint escapes the project root"});
            return plan;
        }
    }

    if (src == dest)
    {
        plan.ok = true; // a same-path move is a no-op
        return plan;
    }

    const std::optional<std::string> owner_bytes = fs.read(src);
    if (!owner_bytes)
    {
        plan.diagnostics.push_back(
            SidecarDiagnostic{"file.not_found", src, "", "move source does not exist"});
        return plan;
    }
    if (fs.read(dest))
    {
        plan.diagnostics.push_back(SidecarDiagnostic{
            "usage.invalid", src, dest, "move destination already exists; refusing to clobber"});
        return plan;
    }

    const SidecarScan scan = scan_sidecar_refs(root, src, *owner_bytes);
    if (!scan.owner_parsed)
    {
        // Refs unknowable — move the owner alone, flagged so a caller can see why nothing was
        // carried (the file is already diagnosed as unparseable by the derivation layer).
        plan.diagnostics.push_back(SidecarDiagnostic{
            "file.parse_error", src, "",
            "owner did not parse as JSON; no sidecar refs were carried with the move"});
    }
    else if (!scan.diagnostics.empty())
    {
        // A malformed or jail-escaping ref means the satellite contract cannot be guaranteed —
        // refuse and surface the findings; fix the refs first (R-FILE-003 red-squiggle posture).
        plan.diagnostics = scan.diagnostics;
        return plan;
    }

    std::vector<PlannedWrite> sidecar_writes;
    std::vector<PlannedWrite> sidecar_removes;
    std::set<std::string> carried; // dedupe: two refs to one sidecar carry it once

    for (const ScannedSidecarRef& scanned : scan.refs)
    {
        const std::string& sidecar_src = scanned.resolved_path;
        if (!carried.insert(sidecar_src).second)
            continue;

        const std::optional<std::string> dest_resolved =
            resolve_sidecar_path(root, dest, scanned.ref.relpath);
        if (!dest_resolved)
        {
            plan.diagnostics.push_back(SidecarDiagnostic{
                "path.jail_violation", src, scanned.ref.relpath,
                scanned.ref.json_pointer +
                    ": the carried sidecar would escape the project root at the destination"});
            return plan;
        }
        if (*dest_resolved == sidecar_src)
            continue; // a same-directory rename leaves the satellite in place — still referenced

        const std::optional<std::string> bytes = fs.read(sidecar_src);
        if (!bytes)
        {
            // Pre-existing dangling ref: the move neither fixes nor worsens it (R-FILE-003
            // eventual validity) — skip the satellite and surface the finding.
            plan.diagnostics.push_back(SidecarDiagnostic{
                "sidecar.dangling_ref", src, sidecar_src,
                scanned.ref.json_pointer + ": referenced sidecar does not exist; not carried"});
            continue;
        }

        const std::uint64_t src_hash = content_hash(*bytes);
        if (const std::optional<std::string> dest_existing = fs.read(*dest_resolved))
        {
            if (content_hash(*dest_existing) != src_hash)
            {
                plan.diagnostics.push_back(SidecarDiagnostic{
                    "usage.invalid", src, *dest_resolved,
                    "a different sidecar already exists at the destination; refusing to clobber"});
                return plan;
            }
            // Identical bytes already at the destination — nothing to write, still remove the src.
        }
        else
        {
            sidecar_writes.push_back(make_write(*dest_resolved, empty_hash(), *bytes));
        }
        sidecar_removes.push_back(make_remove(sidecar_src, src_hash));
    }

    // R-FILE-004 dependency-safe order: dest sidecars -> dest owner -> remove src owner -> remove
    // src sidecars. No observable mid-state has a referencing JSON whose sidecar is missing.
    plan.steps = std::move(sidecar_writes);
    plan.steps.push_back(make_write(dest, empty_hash(), *owner_bytes));
    plan.steps.push_back(make_remove(src, content_hash(*owner_bytes)));
    for (PlannedWrite& removal : sidecar_removes)
        plan.steps.push_back(std::move(removal));

    if (has_duplicate_paths(plan.steps))
    {
        plan.steps.clear();
        plan.diagnostics.push_back(SidecarDiagnostic{
            "usage.invalid", src, "",
            "the plan names the same path twice (duplicates poison crash recovery's re-CAS)"});
        return plan;
    }

    plan.ok = true;
    return plan;
}

} // namespace context::editor::filesync

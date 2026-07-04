// The asset database: GUID index, meta lookup, move/rename + raw-move healing (see asset_database.h).

#include "context/editor/assetdb/asset_database.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <algorithm>
#include <map>
#include <utility>

namespace context::editor::assetdb
{

namespace
{

[[nodiscard]] std::string_view basename_of(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

[[nodiscard]] std::string_view dirname_of(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// The kind of a canonical-JSON asset, read from its `$schema` header (*.json only; one transient
// read — nothing is retained, R-FILE-011(e)). Non-JSON assets record "" until the importer
// framework assigns kinds (M2).
[[nodiscard]] std::string sniff_kind(const filesync::FileStore& fs, std::string_view asset_path)
{
    if (!asset_path.ends_with(".json"))
        return "";
    const std::optional<std::string> bytes = fs.read(asset_path);
    if (!bytes.has_value())
        return "";
    // parse_json, not canonicalize: only the header is read — re-serializing + hashing the whole
    // document would be dead work on this one transient read.
    const serializer::ParseResult parsed = serializer::parse_json(*bytes);
    if (!parsed.ok)
        return "";
    std::vector<serializer::Diagnostic> diags;
    const serializer::DocumentHeader header = serializer::read_document_header(parsed.root, diags);
    return header.has_schema ? header.schema : "";
}

[[nodiscard]] AssetDiagnostic make_diag(std::string code, std::string message, std::string path,
                                        std::string other_path = "", std::string guid = "")
{
    AssetDiagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.path = std::move(path);
    d.other_path = std::move(other_path);
    d.guid = std::move(guid);
    return d;
}

// One parsed sidecar observed on disk during a pass.
struct MetaOnDisk
{
    std::string meta_path;
    std::string asset_path;
    AssetMeta meta;
    bool asset_exists = false;
};

// Read every parseable sidecar under `root`; malformed ones surface asset.meta_invalid.
[[nodiscard]] std::vector<MetaOnDisk> read_metas(const filesync::FileStore& fs,
                                                 std::string_view root,
                                                 std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<MetaOnDisk> out;
    for (const std::string& path : fs.list(root))
    {
        if (!is_meta_path(path))
            continue;
        const std::optional<std::string> bytes = fs.read(path);
        if (!bytes.has_value())
            continue; // raced away between list and read; the next pass sees the truth
        std::vector<std::string> problems;
        const std::optional<AssetMeta> meta = parse_meta(*bytes, problems);
        if (!meta.has_value())
        {
            std::string message = "malformed meta sidecar";
            for (const std::string& p : problems)
                message += "; " + p;
            diagnostics.push_back(
                make_diag("asset.meta_invalid", std::move(message), path));
            continue;
        }
        MetaOnDisk entry;
        entry.meta_path = path;
        entry.asset_path = asset_path_for(path);
        entry.meta = *meta;
        entry.asset_exists = fs.exists(entry.asset_path);
        out.push_back(std::move(entry));
    }
    return out;
}

// The meta-less asset candidates under `root`.
[[nodiscard]] std::vector<std::string> find_newcomers(const filesync::FileStore& fs,
                                                      std::string_view root)
{
    std::vector<std::string> out;
    for (const std::string& path : fs.list(root))
        if (is_asset_candidate(path) && !fs.exists(meta_path_for(path)))
            out.push_back(path);
    return out;
}

} // namespace

bool is_asset_candidate(std::string_view path)
{
    if (is_meta_path(path))
        return false;
    if (filesync::is_atomic_temp_name(basename_of(path)))
        return false;
    // No dot-prefixed segment: `.editor/index`, `.git/...`, `.hidden` are engine/tool-internal.
    std::size_t start = 0;
    while (start <= path.size())
    {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment =
            path.substr(start, slash == std::string_view::npos ? path.size() - start
                                                               : slash - start);
        if (!segment.empty() && segment.front() == '.')
            return false;
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return true;
}

std::string AssetDatabase::mint_unique_guid()
{
    std::string guid;
    do
        guid = guids_->next();
    while (find_by_guid(guid) != nullptr); // never alias an indexed identity
    return guid;
}

std::optional<std::string> AssetDatabase::kind_of(std::string_view guid) const
{
    const AssetRecord* record = find_by_guid(guid);
    if (record == nullptr || record->kind.empty())
        return std::nullopt; // unknown = not enforced (the seam's contract)
    return record->kind;
}

const AssetRecord* AssetDatabase::find_by_guid(std::string_view guid) const
{
    const auto it = by_guid_.find(std::string(guid));
    return it == by_guid_.end() ? nullptr : &it->second;
}

const AssetRecord* AssetDatabase::find_by_path(std::string_view asset_path) const
{
    const auto it = path_to_guid_.find(std::string(asset_path));
    return it == path_to_guid_.end() ? nullptr : find_by_guid(it->second);
}

void AssetDatabase::index_record(AssetRecord record)
{
    if (const auto it = by_guid_.find(record.guid); it != by_guid_.end())
        path_to_guid_.erase(it->second.path); // the guid remaps to its new path
    const std::string guid = record.guid;
    path_to_guid_[record.path] = guid;
    by_guid_[guid] = std::move(record);
}

void AssetDatabase::drop_path(std::string_view asset_path)
{
    const auto it = path_to_guid_.find(std::string(asset_path));
    if (it == path_to_guid_.end())
        return;
    by_guid_.erase(it->second);
    path_to_guid_.erase(it);
}

ScanResult AssetDatabase::scan(const filesync::FileStore& fs, std::string_view root)
{
    by_guid_.clear();
    path_to_guid_.clear();

    ScanResult result;
    // fs.list() is sorted, so first-seen == lexicographically-first: duplicate resolution is
    // deterministic across runs and platforms.
    for (MetaOnDisk& entry : read_metas(fs, root, result.diagnostics))
    {
        if (!entry.asset_exists)
        {
            result.diagnostics.push_back(make_diag(
                "asset.meta_orphaned",
                "meta sidecar's asset file is missing (raw move or delete; heal_moves pairs "
                "unique moves, `context validate --fix` cleans deliberate deletes)",
                entry.meta_path, entry.asset_path, entry.meta.guid));
            continue;
        }
        if (const AssetRecord* existing = find_by_guid(entry.meta.guid); existing != nullptr)
        {
            result.diagnostics.push_back(make_diag(
                "asset.guid_duplicate",
                "two live assets claim the same GUID (raw copy?); the lexicographically-first "
                "path keeps it — re-key the duplicate via `context validate --fix`",
                entry.asset_path, existing->path, entry.meta.guid));
            continue;
        }
        index_record({entry.asset_path, entry.meta.guid, entry.meta.kind});
        ++result.assets_indexed;
    }
    return result;
}

HealResult AssetDatabase::heal_moves(filesync::FileStore& fs, std::string_view root)
{
    HealResult result;
    std::vector<MetaOnDisk> metas = read_metas(fs, root, result.diagnostics);

    // The live-GUID view of THIS pass (not the possibly-stale index): guid -> live asset path.
    std::map<std::string, std::string> live_guids;
    for (const MetaOnDisk& m : metas)
        if (m.asset_exists)
            live_guids.emplace(m.meta.guid, m.asset_path);

    std::vector<const MetaOnDisk*> orphans;
    for (const MetaOnDisk& m : metas)
        if (!m.asset_exists)
            orphans.push_back(&m);
    std::vector<std::string> newcomers = find_newcomers(fs, root);

    std::vector<bool> orphan_handled(orphans.size(), false);
    std::vector<bool> newcomer_handled(newcomers.size(), false);

    // Rule 0 — residue of an interrupted move: the orphan's GUID already lives at a live pair.
    // Removing the leftover sidecar COMPLETES the move (GUID move-healing, R-FILE-003).
    for (std::size_t i = 0; i < orphans.size(); ++i)
        if (const auto live = live_guids.find(orphans[i]->meta.guid); live != live_guids.end())
        {
            fs.remove(orphans[i]->meta_path);
            result.actions.push_back({"meta-residue-removed", live->second,
                                      orphans[i]->meta_path, orphans[i]->meta.guid});
            orphan_handled[i] = true;
        }

    // Decide every pairing BEFORE performing writes so decisions never depend on write order.
    std::vector<std::pair<std::size_t, std::size_t>> pairs; // (orphan idx, newcomer idx)
    std::vector<bool> orphan_ambiguous(orphans.size(), false);

    // Rule 1 — unique basename match anywhere (the dominant raw move: directories relocated).
    {
        std::map<std::string, std::vector<std::size_t>> orphan_by_base;
        std::map<std::string, std::vector<std::size_t>> newcomer_by_base;
        for (std::size_t i = 0; i < orphans.size(); ++i)
            if (!orphan_handled[i])
                orphan_by_base[std::string(basename_of(orphans[i]->asset_path))].push_back(i);
        for (std::size_t j = 0; j < newcomers.size(); ++j)
            if (!newcomer_handled[j])
                newcomer_by_base[std::string(basename_of(newcomers[j]))].push_back(j);
        for (const auto& [base, os] : orphan_by_base)
        {
            const auto ns = newcomer_by_base.find(base);
            if (ns == newcomer_by_base.end() || ns->second.empty())
                continue;
            if (os.size() == 1 && ns->second.size() == 1)
            {
                pairs.emplace_back(os.front(), ns->second.front());
                orphan_handled[os.front()] = true;
                newcomer_handled[ns->second.front()] = true;
            }
            else
                for (const std::size_t i : os)
                    orphan_ambiguous[i] = true;
        }
    }

    // Rule 2 — the sole orphan + sole newcomer of ONE directory (rename-in-place without meta).
    {
        std::map<std::string, std::vector<std::size_t>> orphan_by_dir;
        std::map<std::string, std::vector<std::size_t>> newcomer_by_dir;
        for (std::size_t i = 0; i < orphans.size(); ++i)
            if (!orphan_handled[i])
                orphan_by_dir[std::string(dirname_of(orphans[i]->asset_path))].push_back(i);
        for (std::size_t j = 0; j < newcomers.size(); ++j)
            if (!newcomer_handled[j])
                newcomer_by_dir[std::string(dirname_of(newcomers[j]))].push_back(j);
        for (const auto& [dir, os] : orphan_by_dir)
        {
            const auto ns = newcomer_by_dir.find(dir);
            if (ns == newcomer_by_dir.end() || ns->second.empty())
                continue;
            if (os.size() == 1 && ns->second.size() == 1)
            {
                pairs.emplace_back(os.front(), ns->second.front());
                orphan_handled[os.front()] = true;
                newcomer_handled[ns->second.front()] = true;
            }
            else
                for (const std::size_t i : os)
                    orphan_ambiguous[i] = true;
        }
    }

    // Perform the healing writes: meta bytes travel VERBATIM (identity + import settings + any
    // newer-engine members survive), new sidecar first, then the orphan removed — the same
    // meta-first discipline as the move verb.
    for (const auto& [oi, nj] : pairs)
    {
        const MetaOnDisk& orphan = *orphans[oi];
        const std::string& newcomer = newcomers[nj];
        const std::optional<std::string> bytes = fs.read(orphan.meta_path);
        if (!bytes.has_value())
            continue; // raced away; the next pass re-decides
        const std::string new_meta = meta_path_for(newcomer);
        filesync::atomic_write(fs, new_meta, *bytes, "assetdb-heal");
        fs.remove(orphan.meta_path);
        result.actions.push_back({"meta-moved", newcomer, new_meta, orphan.meta.guid});
        drop_path(orphan.asset_path);
        index_record({newcomer, orphan.meta.guid, orphan.meta.kind});
    }

    // What could not be healed is reported, never guessed (R-FILE-003: no unasked "fixes").
    for (std::size_t i = 0; i < orphans.size(); ++i)
    {
        if (orphan_handled[i])
            continue;
        if (orphan_ambiguous[i])
            result.diagnostics.push_back(make_diag(
                "asset.heal_ambiguous",
                "raw-move healing found no UNIQUE orphan/newcomer pairing for this sidecar; "
                "re-run the move via `context asset move` or resolve by hand",
                orphans[i]->meta_path, orphans[i]->asset_path, orphans[i]->meta.guid));
        else
            result.diagnostics.push_back(make_diag(
                "asset.meta_orphaned",
                "meta sidecar's asset file is missing and no meta-less newcomer matches; "
                "`context validate --fix` removes the sidecar if the delete was deliberate",
                orphans[i]->meta_path, orphans[i]->asset_path, orphans[i]->meta.guid));
    }
    return result;
}

HealResult AssetDatabase::ensure_metas(filesync::FileStore& fs, std::string_view root)
{
    HealResult result;
    for (const std::string& path : find_newcomers(fs, root))
    {
        AssetMeta meta;
        meta.kind = sniff_kind(fs, path);
        meta.guid = mint_unique_guid();
        const std::string meta_path = meta_path_for(path);
        filesync::atomic_write(fs, meta_path, serialize_meta(meta), "assetdb-create");
        result.actions.push_back({"meta-created", path, meta_path, meta.guid});
        index_record({path, meta.guid, meta.kind});
    }
    return result;
}

MoveResult AssetDatabase::move_asset(filesync::FileStore& fs, std::string_view from,
                                     std::string_view to)
{
    MoveResult result;
    const std::string from_s(from);
    const std::string to_s(to);
    const std::string from_meta = meta_path_for(from);
    const std::string to_meta = meta_path_for(to);

    if (is_meta_path(from) || is_meta_path(to) || from.empty() || to.empty())
    {
        result.diagnostics.push_back(make_diag(
            "asset.move_invalid",
            "move/rename operates on ASSET paths; sidecars travel with their asset", from_s,
            to_s));
        return result;
    }
    if (from == to)
    {
        // A no-op request converges trivially (idempotence over the degenerate window).
        if (const AssetRecord* rec = find_by_path(from); rec != nullptr)
            result.guid = rec->guid;
        result.ok = fs.exists(from_s);
        if (!result.ok)
            result.diagnostics.push_back(make_diag("asset.move_source_missing",
                                                   "the asset does not exist", from_s));
        return result;
    }

    const bool from_exists = fs.exists(from_s);
    const bool to_exists = fs.exists(to_s);
    std::vector<std::string> problems;

    std::optional<AssetMeta> from_meta_parsed;
    std::optional<std::string> from_meta_bytes = fs.read(from_meta);
    if (from_meta_bytes.has_value())
        from_meta_parsed = parse_meta(*from_meta_bytes, problems);
    std::optional<AssetMeta> to_meta_parsed;
    if (const std::optional<std::string> bytes = fs.read(to_meta); bytes.has_value())
        to_meta_parsed = parse_meta(*bytes, problems);

    // --- convergence / resume detection (R-FILE-004: idempotent + re-runnable under partial
    // apply — every crash window of the write order below re-enters through one of these arms).
    if (!from_exists)
    {
        if (!to_exists)
        {
            result.diagnostics.push_back(make_diag("asset.move_source_missing",
                                                   "the asset does not exist", from_s, to_s));
            return result;
        }
        // Destination present, source gone: the move applied. Clear identity residue.
        if (from_meta_parsed.has_value())
        {
            if (to_meta_parsed.has_value() &&
                to_meta_parsed->guid != from_meta_parsed->guid)
            {
                result.diagnostics.push_back(make_diag(
                    "asset.move_source_missing",
                    "the source is gone and the destination holds a DIFFERENT asset; the "
                    "leftover source sidecar needs `context validate --fix`",
                    from_s, to_s, from_meta_parsed->guid));
                return result;
            }
            if (!to_meta_parsed.has_value())
                filesync::atomic_write(fs, to_meta, *from_meta_bytes, "assetdb-move");
            fs.remove(from_meta);
        }
        if (to_meta_parsed.has_value())
            result.guid = to_meta_parsed->guid;
        else if (from_meta_parsed.has_value())
            result.guid = from_meta_parsed->guid;
        drop_path(from);
        if (!result.guid.empty())
            index_record({to_s, result.guid,
                          to_meta_parsed.has_value() ? to_meta_parsed->kind
                                                     : from_meta_parsed->kind});
        result.ok = true;
        return result;
    }
    if (from_meta_bytes.has_value() && !from_meta_parsed.has_value())
    {
        // A malformed-but-PRESENT source sidecar is not a meta-less source: minting fresh identity
        // would silently re-key the asset (references to the old GUID dangle) and the write order
        // below would then discard the unparseable bytes — identity + import settings that may be
        // recoverable by hand or from git. Refuse instead (R-FILE-003: no unasked destructive
        // fixes); scan() reports the same asset.meta_invalid and the repair is the user's call.
        result.diagnostics.push_back(make_diag(
            "asset.meta_invalid",
            "the source asset's meta sidecar is malformed; repair it (or remove it to mint fresh "
            "identity) before moving",
            from_meta, from_s));
        return result;
    }
    if (to_exists)
    {
        const bool same_identity = from_meta_parsed.has_value() && to_meta_parsed.has_value() &&
                                   from_meta_parsed->guid == to_meta_parsed->guid;
        const bool same_bytes_pre_meta =
            !to_meta_parsed.has_value() && fs.read(from_s) == fs.read(to_s);
        if (!same_identity && !same_bytes_pre_meta)
        {
            // CAS-honesty: a DIFFERENT asset occupies the destination — refuse, never overwrite.
            result.diagnostics.push_back(make_diag(
                "asset.move_destination_exists",
                "the destination is occupied by a different asset; move/rename never overwrites",
                to_s, from_s));
            return result;
        }
        // Otherwise this is our own interrupted move — fall through and re-run the write order
        // (each step re-applies or no-ops; atomic_write is idempotent for identical bytes).
    }

    // --- identity for the destination sidecar: the existing bytes travel VERBATIM (import
    // settings + unknown members survive); a meta-less source mints identity at move time.
    std::string guid;
    std::string kind;
    std::string meta_bytes;
    if (from_meta_parsed.has_value())
    {
        guid = from_meta_parsed->guid;
        kind = from_meta_parsed->kind;
        meta_bytes = *from_meta_bytes;
    }
    else
    {
        AssetMeta fresh;
        fresh.kind = sniff_kind(fs, from_s);
        fresh.guid = mint_unique_guid();
        guid = fresh.guid;
        kind = fresh.kind;
        meta_bytes = serialize_meta(fresh);
    }

    const std::optional<std::string> asset_bytes = fs.read(from_s);
    if (!asset_bytes.has_value())
    {
        result.diagnostics.push_back(make_diag("asset.move_source_missing",
                                               "the asset raced away mid-move", from_s, to_s));
        return result;
    }

    // --- the R-FILE-004 dependency-safe write order: destination file, destination meta (GUID
    // identity is at the destination from here on), THEN source removal — per-file atomic, no
    // cross-file transaction (L-25). Referencing files are untouched (hints heal lazily, L-34).
    filesync::atomic_write(fs, to_s, *asset_bytes, "assetdb-move");
    filesync::atomic_write(fs, to_meta, meta_bytes, "assetdb-move");
    fs.remove(from_s);
    if (from_meta_bytes.has_value())
        fs.remove(from_meta);

    drop_path(from);
    index_record({to_s, guid, kind});
    result.ok = true;
    result.guid = guid;
    return result;
}

} // namespace context::editor::assetdb

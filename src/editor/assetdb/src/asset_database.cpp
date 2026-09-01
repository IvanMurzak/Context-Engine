// The asset database: GUID index, meta lookup, move/rename + raw-move healing, and the M9 e2
// quarantine-backed delete/restore pair (see asset_database.h).

#include "context/editor/assetdb/asset_database.h"

#include "context/editor/assetdb/ref_heal.h"
#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

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

// --- M9 e2: the quarantine ENTRY record --------------------------------------------------------
//
// The one thing `restore_asset` cannot re-derive from the quarantined bytes is WHERE they came from
// (and whether the asset had a sidecar at all -- restoring one that never existed would not be a
// byte-identical restore). This tiny canonical-JSON record carries exactly that, beside the bytes,
// so the handle in an undo checkpoint stays 32 hex characters.
inline constexpr std::string_view kTrashEntryKind = "ctx:trash-entry";

struct TrashEntry
{
    std::string asset_path;
    std::string guid;
    bool has_meta = false;
};

[[nodiscard]] std::string serialize_trash_entry(const TrashEntry& entry)
{
    serializer::JsonValue root;
    root.type = serializer::JsonValue::Type::object;
    auto add_string = [&root](std::string_view key, std::string_view value)
    {
        serializer::JsonMember m;
        m.key = std::string(key);
        m.value.type = serializer::JsonValue::Type::string;
        m.value.string_value = std::string(value);
        root.members.push_back(std::move(m));
    };
    add_string("$schema", kTrashEntryKind);
    add_string("assetPath", entry.asset_path);
    add_string("guid", entry.guid);
    serializer::JsonMember has_meta;
    has_meta.key = "hasMeta";
    has_meta.value.type = serializer::JsonValue::Type::boolean;
    has_meta.value.boolean_value = entry.has_meta;
    root.members.push_back(std::move(has_meta));

    std::string out;
    const bool ok = serializer::serialize_canonical(root, out);
    (void)ok; // strings + booleans only -- always serializable
    return out;
}

[[nodiscard]] std::optional<TrashEntry> parse_trash_entry(std::string_view bytes)
{
    const serializer::ParseResult parsed = serializer::parse_json(bytes);
    if (!parsed.ok || parsed.root.type != serializer::JsonValue::Type::object)
        return std::nullopt;
    const serializer::JsonValue* asset_path = schema::find_member(parsed.root, "assetPath");
    const serializer::JsonValue* guid = schema::find_member(parsed.root, "guid");
    if (asset_path == nullptr || asset_path->type != serializer::JsonValue::Type::string ||
        asset_path->string_value.empty() || guid == nullptr ||
        guid->type != serializer::JsonValue::Type::string)
        return std::nullopt;
    TrashEntry entry;
    entry.asset_path = asset_path->string_value;
    entry.guid = guid->string_value;
    if (const serializer::JsonValue* has_meta = schema::find_member(parsed.root, "hasMeta");
        has_meta != nullptr)
        entry.has_meta = has_meta->type == serializer::JsonValue::Type::boolean &&
                         has_meta->boolean_value;
    return entry;
}

// One parsed sidecar observed on disk during a pass.
struct MetaOnDisk
{
    std::string meta_path;
    std::string asset_path;
    AssetMeta meta;
    bool asset_exists = false;
};

// Read every parseable sidecar named in `paths`; malformed ones surface asset.meta_invalid. Takes
// an already-fetched listing so a caller that also needs the raw listing itself (the `editor.files`
// daemon read) can share ONE `fs.list()` walk instead of paying for a second.
[[nodiscard]] std::vector<MetaOnDisk> read_metas_over(const filesync::FileStore& fs,
                                                      const std::vector<std::string>& paths,
                                                      std::vector<AssetDiagnostic>& diagnostics)
{
    std::vector<MetaOnDisk> out;
    for (const std::string& path : paths)
    {
        if (!is_meta_path(path))
            continue;
        std::string asset_path = asset_path_for(path);
        // Sidecars OUTSIDE the asset domain — under a dot-segment tree (`.editor/`, `.git/`),
        // beside atomic-write residue, or a sidecar-of-a-sidecar — are not asset metas: scan must
        // never index them (a hand-made `x.json.meta.json.meta.json` must not turn a sidecar into
        // an asset) and heal_moves must never pair their identity onto a genuine asset
        // (find_newcomers applies the same filter on the newcomer side). Out of domain means out
        // of diagnostics too: they are engine/tool territory, not scan findings.
        if (!is_asset_candidate(asset_path))
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
        entry.asset_path = std::move(asset_path);
        entry.meta = *meta;
        entry.asset_exists = fs.exists(entry.asset_path);
        out.push_back(std::move(entry));
    }
    return out;
}

// Read every parseable sidecar under `root`; malformed ones surface asset.meta_invalid.
[[nodiscard]] std::vector<MetaOnDisk> read_metas(const filesync::FileStore& fs,
                                                 std::string_view root,
                                                 std::vector<AssetDiagnostic>& diagnostics)
{
    return read_metas_over(fs, fs.list(root), diagnostics);
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
    return scan(fs, fs.list(root));
}

ScanResult AssetDatabase::scan(const filesync::FileStore& fs,
                               const std::vector<std::string>& listed_paths)
{
    by_guid_.clear();
    path_to_guid_.clear();

    ScanResult result;
    // listed_paths is expected sorted (fs.list()'s contract), so first-seen == lexicographically-
    // first: duplicate resolution is deterministic across runs and platforms.
    for (MetaOnDisk& entry : read_metas_over(fs, listed_paths, result.diagnostics))
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

    // is_asset_candidate refuses sidecar paths, atomic-temp shapes, and dot-segment trees in one
    // check — endpoints outside the asset domain would produce a pair the index can never see
    // (silent identity loss) or collide with atomic-write residue cleanup. Empty stays explicit
    // (the candidate walk vacuously accepts "").
    if (from.empty() || to.empty() || !is_asset_candidate(from) || !is_asset_candidate(to))
    {
        result.diagnostics.push_back(make_diag(
            "asset.move_invalid",
            "move/rename operates on ASSET paths (not sidecars, dot-tree internals, or temp "
            "residue); sidecars travel with their asset",
            from_s, to_s));
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
    const std::optional<std::string> to_meta_bytes = fs.read(to_meta);
    if (to_meta_bytes.has_value())
        to_meta_parsed = parse_meta(*to_meta_bytes, problems);

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
            if (to_meta_bytes.has_value() && !to_meta_parsed.has_value())
            {
                // A malformed-but-PRESENT destination sidecar gets the same honesty as the
                // malformed SOURCE below: its bytes may hold identity/import settings recoverable
                // by hand or from git, and our own write order never leaves a torn sidecar
                // (atomic_write is all-or-nothing) — this is foreign state, never our residue.
                // Refuse rather than overwrite (R-FILE-003: no unasked destructive fixes).
                result.diagnostics.push_back(make_diag(
                    "asset.meta_invalid",
                    "the destination's meta sidecar is malformed; repair it (or remove it) "
                    "before re-running the move",
                    to_meta, to_s));
                return result;
            }
            if (!to_meta_bytes.has_value())
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
    if (to_meta_bytes.has_value() && !to_meta_parsed.has_value())
    {
        // The malformed-DESTINATION twin of the refusal above: the resume arms below must never
        // treat unparseable destination-sidecar bytes as "no sidecar" and clobber them — the
        // same-bytes resume window is defined by a MISSING destination sidecar (our atomic writes
        // never leave a torn one, so a malformed one is foreign state).
        result.diagnostics.push_back(make_diag(
            "asset.meta_invalid",
            "the destination's meta sidecar is malformed; repair it (or remove it) before moving",
            to_meta, to_s));
        return result;
    }
    if (!to_exists && to_meta_parsed.has_value())
    {
        // An orphaned sidecar squats at the destination (its asset file is gone). No write order
        // of ours produces this state — the destination FILE lands first and is never removed —
        // so it is foreign residue holding some asset's identity. Refuse rather than overwrite;
        // `context validate --fix` cleans deliberate deletes.
        result.diagnostics.push_back(make_diag(
            "asset.move_destination_exists",
            "an orphaned meta sidecar occupies the destination; `context validate --fix` cleans "
            "deliberate deletes",
            to_s, from_s, to_meta_parsed->guid));
        return result;
    }
    if (to_exists)
    {
        const bool same_identity = from_meta_parsed.has_value() && to_meta_parsed.has_value() &&
                                   from_meta_parsed->guid == to_meta_parsed->guid;
        // KNOWN AMBIGUITY (accepted): bytes alone cannot tell our window-B residue (destination
        // file landed, crash before its sidecar) from a coincidentally byte-identical, meta-less
        // file that legitimately occupies the destination — pre-meta files carry no identity to
        // compare. In that edge the documented occupied-destination refusal does not fire; the
        // move completes (source removed, fresh identity minted at `to`). Accepted: the bytes at
        // `to` are unchanged either way and neither endpoint holds a GUID yet, so no content or
        // identity is destroyed — only the refusal diagnostic is missed. Distinguishing the two
        // would need a persisted move-intent record; the window closes once assets are meta-adopted.
        const bool same_bytes_pre_meta =
            !to_meta_bytes.has_value() && fs.read(from_s) == fs.read(to_s);
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


std::string trash_asset_path(std::string_view token)
{
    return std::string(kTrashRoot) + "/" + std::string(token) + "/asset";
}

std::string trash_meta_path(std::string_view token)
{
    return meta_path_for(trash_asset_path(token));
}

std::string trash_entry_path(std::string_view token)
{
    return std::string(kTrashRoot) + "/" + std::string(token) + "/entry.json";
}

DeleteResult AssetDatabase::delete_asset(filesync::FileStore& fs, std::string_view root,
                                         std::string_view path, const schema::SchemaSet& schemas)
{
    DeleteResult result;
    const std::string path_s(path);
    const std::string meta = meta_path_for(path);

    // The move path's endpoint check, verbatim in spirit: a sidecar, a dot-tree internal or
    // atomic-write residue is not an ASSET, and letting one through would either strand identity
    // (deleting a sidecar out from under a live asset) or race the temp-residue sweep.
    if (path.empty() || !is_asset_candidate(path))
    {
        result.diagnostics.push_back(make_diag(
            "asset.delete_invalid",
            "delete operates on ASSET paths (not sidecars, dot-tree internals, or temp residue); "
            "a sidecar is deleted with its asset, never on its own",
            path_s));
        return result;
    }

    const bool asset_exists = fs.exists(path_s);
    std::vector<std::string> problems;
    const std::optional<std::string> meta_bytes = fs.read(meta);
    std::optional<AssetMeta> meta_parsed;
    if (meta_bytes.has_value())
        meta_parsed = parse_meta(*meta_bytes, problems);

    if (meta_bytes.has_value() && !meta_parsed.has_value())
    {
        // The malformed-sidecar refusal move_asset already makes, for the same reason and with the
        // same code: those bytes may hold identity + import settings recoverable by hand or from
        // git, and quarantining a sidecar we cannot even read the GUID out of would file it under a
        // token nothing can name. Repair (or remove) it first -- R-FILE-003: no unasked destructive
        // fixes.
        result.diagnostics.push_back(make_diag(
            "asset.meta_invalid",
            "the asset's meta sidecar is malformed; repair it (or remove it) before deleting",
            meta, path_s));
        return result;
    }

    // --- convergence / resume detection (R-FILE-004) ---------------------------------------------
    if (!asset_exists)
    {
        if (!meta_bytes.has_value())
        {
            // Fully converged. Deliberately ok=true with both removed_* false, so a caller can tell
            // "this call deleted something" from "the requested end state already held" -- and so a
            // re-run after a COMPLETED delete answers the same as a re-run after a partial one.
            // KNOWN AMBIGUITY (accepted, the same shape move_asset documents): bytes alone cannot
            // tell our own completed delete from a path that never existed; distinguishing them
            // would need a persisted intent record.
            drop_path(path);
            result.ok = true;
            return result;
        }
        // An ORPHANED SIDECAR is the residue of an interrupted delete (asset removed, sidecar not
        // yet) -- exactly the state scan() reports as asset.meta_orphaned and heal_moves() treats as
        // interrupted-move residue. COMPLETE it.
        result.guid = meta_parsed->guid;
        // The restore handle is offered only when the quarantine entry actually EXISTS. When the
        // orphan came from a hand-deleted file rather than from us there is nothing quarantined, and
        // synthesizing a half entry (a sidecar with no asset bytes) would restore something that was
        // never deleted.
        if (fs.exists(trash_entry_path(meta_parsed->guid)))
            result.restore_token = meta_parsed->guid;
        fs.remove(meta);
        drop_path(path);
        result.removed_meta = true;
        result.ok = true;
        return result;
    }

    // --- REFUSE on visible referrers, before a single byte moves ---------------------------------
    const std::string existing_guid = meta_parsed.has_value() ? meta_parsed->guid : std::string();
    const std::vector<Referrer> referrers =
        find_referrers(fs, fs.list(root), schemas, existing_guid, path_s);
    if (!referrers.empty())
    {
        for (const Referrer& referrer : referrers)
            result.diagnostics.push_back(make_diag(
                "asset.delete_referenced",
                "`" + referrer.path + "` references this asset at `" +
                    (referrer.pointer.empty() ? std::string("/") : referrer.pointer) +
                    "`; delete refuses rather than leaving a dangling reference",
                path_s, referrer.path, existing_guid));
        return result;
    }

    // --- identity: the quarantine token ----------------------------------------------------------
    // A sidecar-bearing asset files under its OWN GUID, which makes the token deterministic: a
    // crashed delete's re-run lands in the SAME entry rather than leaking a second copy. A meta-less
    // asset has no identity to file under, so one is minted purely as a handle -- it is never
    // written as a sidecar, so restoring brings back exactly the meta-less file that was deleted.
    const std::string token = meta_parsed.has_value() ? meta_parsed->guid : mint_unique_guid();

    const std::optional<std::string> asset_bytes = fs.read(path_s);
    if (!asset_bytes.has_value())
    {
        result.diagnostics.push_back(
            make_diag("asset.delete_source_missing", "the asset raced away mid-delete", path_s));
        return result;
    }

    // --- the R-FILE-004 dependency-safe write order ----------------------------------------------
    // Quarantine FIRST (the bytes exist in two places before they exist in one), then the source
    // asset, then the identity-bearing sidecar LAST -- see the header for why the reverse order
    // would let ensure_metas() re-key a half-deleted asset.
    //
    // AND THE QUARANTINE WRITES ARE CHECKED, unlike move_asset's destination writes -- the asymmetry
    // is the point. A failed destination write on a MOVE leaves the source intact, so ignoring it
    // costs a confusing result at worst; a failed quarantine write on a DELETE would remove the only
    // copy while reporting a `restore_token` that resolves to nothing. Refuse before the removal:
    // nothing has left the project yet, so the honest answer is "the delete did not happen".
    const bool quarantined_asset =
        filesync::atomic_write(fs, trash_asset_path(token), *asset_bytes, "assetdb-delete");
    const bool quarantined_meta =
        !meta_parsed.has_value() ||
        filesync::atomic_write(fs, trash_meta_path(token), *meta_bytes, "assetdb-delete");
    const bool quarantined_entry = filesync::atomic_write(
        fs, trash_entry_path(token), serialize_trash_entry({path_s, token, meta_parsed.has_value()}),
        "assetdb-delete");
    if (!quarantined_asset || !quarantined_meta || !quarantined_entry)
    {
        result.diagnostics.push_back(make_diag(
            "internal.error",
            "the asset could not be quarantined under `" + std::string(kTrashRoot) +
                "`, so nothing was deleted -- an irreversible delete is never the fallback",
            path_s, trash_entry_path(token), existing_guid));
        return result;
    }

    fs.remove(path_s);
    result.removed_asset = true;
    if (meta_bytes.has_value())
    {
        fs.remove(meta);
        result.removed_meta = true;
    }

    drop_path(path);
    result.guid = token;
    result.restore_token = token;
    result.ok = true;
    return result;
}

RestoreResult AssetDatabase::restore_asset(filesync::FileStore& fs, std::string_view restore_token)
{
    RestoreResult result;
    const std::string token(restore_token);
    if (token.empty())
    {
        result.diagnostics.push_back(
            make_diag("asset.restore_missing", "no restore token was supplied", ""));
        return result;
    }

    // The token is interpolated straight into a filesystem path, so it must be ONE directory name.
    // A token carrying a separator (or a dot segment) would address `.editor/trash/` relative to
    // somewhere else entirely -- and the READ side of the FileStore seam is lexical
    // (native_file_store.h: only write-side ops carry the R-SEC-008 physical jail), so an escaping
    // token would read an `entry.json` from outside the project root. Refused here rather than only
    // at the daemon boundary, so every caller of the engine operation gets the same guarantee.
    if (token.front() == '.' || token.find('/') != std::string::npos ||
        token.find('\\') != std::string::npos)
    {
        result.diagnostics.push_back(
            make_diag("asset.restore_invalid",
                      "a restore token names ONE quarantine directory; this one is a path", token));
        return result;
    }

    const std::string entry_path = trash_entry_path(token);
    const std::optional<std::string> entry_bytes = fs.read(entry_path);
    const std::optional<TrashEntry> entry =
        entry_bytes.has_value() ? parse_trash_entry(*entry_bytes) : std::nullopt;
    const std::optional<std::string> asset_bytes = fs.read(trash_asset_path(token));
    if (!entry.has_value() || !asset_bytes.has_value())
    {
        // The honest answer for BOTH "a human cleared .editor/trash/" and "this restore already
        // completed": there is nothing quarantined under this token. Never a silent success --
        // reporting ok here would tell the human their file came back when it did not.
        result.diagnostics.push_back(make_diag(
            "asset.restore_missing",
            "no quarantined asset is filed under this restore token; `.editor/trash/` may have been "
            "cleared, or the restore already completed",
            entry_path));
        return result;
    }
    if (!is_asset_candidate(entry->asset_path))
    {
        result.diagnostics.push_back(make_diag(
            "asset.restore_invalid",
            "the quarantine entry names a path outside the asset domain; refusing to write there",
            entry->asset_path));
        return result;
    }

    const std::string dest = entry->asset_path;
    const std::string dest_meta = meta_path_for(dest);
    const std::optional<std::string> quarantined_meta =
        entry->has_meta ? fs.read(trash_meta_path(token)) : std::nullopt;
    if (entry->has_meta && !quarantined_meta.has_value())
    {
        // The entry says this asset HAD a sidecar and the sidecar is not there (a partially cleared
        // `.editor/trash/`, an I/O failure). Restoring the payload alone would put back a META-LESS
        // asset -- which the very next ensure_metas() pass re-keys with a FRESH GUID, resurrecting it
        // under a new identity and dangling every reference to the old one. That is the exact failure
        // delete_asset orders its removals to avoid, so restore refuses it rather than producing it.
        result.diagnostics.push_back(make_diag(
            "asset.restore_missing",
            "the quarantined sidecar for this restore token is missing, so the asset cannot come "
            "back with its identity; nothing was written",
            trash_meta_path(token), entry->asset_path, entry->guid));
        return result;
    }

    // CAS-honesty, the occupied-destination rule move_asset states: something else may have taken
    // this path since the delete. Identical bytes are our own interrupted restore (fall through and
    // re-run the order); anything else refuses rather than overwrites.
    if (fs.exists(dest) && fs.read(dest) != asset_bytes)
    {
        result.diagnostics.push_back(make_diag(
            "asset.restore_destination_exists",
            "a different file now occupies the deleted asset's path; restore never overwrites",
            dest));
        return result;
    }
    if (const std::optional<std::string> occupying_meta = fs.read(dest_meta);
        occupying_meta.has_value())
    {
        std::vector<std::string> problems;
        const std::optional<AssetMeta> parsed = parse_meta(*occupying_meta, problems);
        if (!parsed.has_value() || parsed->guid != entry->guid)
        {
            result.diagnostics.push_back(make_diag(
                "asset.restore_destination_exists",
                "a sidecar holding a different identity occupies the deleted asset's path; restore "
                "never overwrites",
                dest_meta, dest, entry->guid));
            return result;
        }
    }

    // The inverse dependency-safe order: the asset file lands first, then the sidecar re-attaches
    // identity to it, then the quarantine entry is cleared. A crash anywhere re-runs to the same
    // result; a crash before the entry is cleared simply leaves a re-runnable restore.
    filesync::atomic_write(fs, dest, *asset_bytes, "assetdb-restore");
    result.restored_asset = true;
    std::string kind;
    if (quarantined_meta.has_value())
    {
        filesync::atomic_write(fs, dest_meta, *quarantined_meta, "assetdb-restore");
        result.restored_meta = true;
        std::vector<std::string> problems;
        if (const std::optional<AssetMeta> parsed = parse_meta(*quarantined_meta, problems);
            parsed.has_value())
            kind = parsed->kind;
    }

    fs.remove(trash_asset_path(token));
    if (entry->has_meta)
        fs.remove(trash_meta_path(token));
    fs.remove(entry_path);

    result.guid = entry->guid;
    result.path = dest;
    result.ok = true;
    if (result.restored_meta)
        index_record({dest, entry->guid, kind});
    return result;
}

} // namespace context::editor::assetdb

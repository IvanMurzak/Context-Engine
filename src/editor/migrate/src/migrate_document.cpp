// Parse-time document migration implementation (see migrate_document.h).

#include "context/editor/migrate/migrate_document.h"

#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <string>
#include <utility>

namespace context::editor::migrate
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// JSON-pointer segment escaping (RFC 6901): "~" -> "~0", "/" -> "~1".
std::string escape_segment(std::string_view segment)
{
    std::string out;
    out.reserve(segment.size());
    for (const char c : segment)
    {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out += c;
    }
    return out;
}

JsonValue* find_member(JsonValue& object, std::string_view key)
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The identity of one id/guid member: (document-relative pointer within the payload, canonical
// serialization of the value). The exact multiset of these must survive every step (L-37: a
// migration never alters, moves, adds, or removes entity/instance ids or GUIDs).
void collect_ids(const JsonValue& value, const std::string& pointer,
                 std::vector<std::pair<std::string, std::string>>& out)
{
    if (value.type == JsonValue::Type::object)
    {
        for (const JsonMember& m : value.members)
        {
            const std::string child = pointer + "/" + escape_segment(m.key);
            if (m.key == "id" || m.key == "guid")
            {
                std::string bytes;
                // The payload was pre-checked canonically serializable, so this cannot fail here.
                (void)serializer::serialize_canonical(m.value, bytes);
                out.emplace_back(child, std::move(bytes));
            }
            collect_ids(m.value, child, out);
        }
    }
    else if (value.type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < value.elements.size(); ++i)
            collect_ids(value.elements[i], pointer + "/" + std::to_string(i), out);
    }
}

// One planned per-type migration: the stamped (from) and registered (to) versions plus the
// resolved step chain from -> from+1 -> ... -> to.
struct TypePlan
{
    std::string type;
    std::int64_t from = 0;
    std::int64_t to = 0;
    std::vector<const MigrationStep*> chain;
};

// A discovered payload site: the document pointer of the payload value + the value itself.
struct PayloadSite
{
    std::string pointer;
    JsonValue* payload = nullptr;
};

// The ONE payload-opacity rule every document traversal below shares (site discovery, the
// unstamped-site scan, override rewriting): an object member keyed by a header-stamped OR
// registered "<ns>:<type>" id IS a payload site, and payload interiors are opaque — no traversal
// descends into one (payload-internal data that happens to use a namespaced key, an "overrides"-
// shaped member, etc. is the payload's private business).
bool is_payload_boundary(std::string_view key, const MigrationSet& set,
                         const std::vector<std::string>& stamped)
{
    if (key.find(':') == std::string_view::npos)
        return false;
    if (set.current_version(key) > 0)
        return true;
    return std::find(stamped.begin(), stamped.end(), key) != stamped.end();
}

// Recursively discover payload sites for `type`: any object member whose key equals the type id
// and whose value is an object. The root "componentVersions" header member is exempt, and
// EVERY payload subtree — this type's or another stamped/registered type's — is opaque
// (is_payload_boundary): a nested namespaced key inside some other payload is that payload's
// private data, never a site of this scan.
void find_sites(JsonValue& value, const std::string& pointer, std::string_view type, bool is_root,
                const MigrationSet& set, const std::vector<std::string>& stamped,
                std::vector<PayloadSite>& out)
{
    if (value.type == JsonValue::Type::object)
    {
        for (JsonMember& m : value.members)
        {
            if (is_root && m.key == "componentVersions")
                continue;
            const std::string child = pointer + "/" + escape_segment(m.key);
            if (m.value.type == JsonValue::Type::object)
            {
                if (m.key == type)
                {
                    out.push_back({child, &m.value});
                    continue; // opaque: no nested sites inside a payload
                }
                if (is_payload_boundary(m.key, set, stamped))
                    continue; // another type's payload: equally opaque to this scan
            }
            find_sites(m.value, child, type, /*is_root=*/false, set, stamped, out);
        }
    }
    else if (value.type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < value.elements.size(); ++i)
            find_sites(value.elements[i], pointer + "/" + std::to_string(i), type,
                       /*is_root=*/false, set, stamped, out);
    }
}

// Discover registered-type payload sites with NO componentVersions stamp (the
// stamp_registered_sites tool-save rule). Same traversal contract as find_sites: every
// stamped/registered payload subtree is opaque, whether or not it contributes to `out`.
void find_unstamped_types(const JsonValue& value, bool is_root, const MigrationSet& set,
                          const std::vector<std::string>& stamped, std::vector<std::string>& out)
{
    if (value.type == JsonValue::Type::object)
    {
        for (const JsonMember& m : value.members)
        {
            if (is_root && m.key == "componentVersions")
                continue;
            if (m.value.type == JsonValue::Type::object && is_payload_boundary(m.key, set, stamped))
            {
                if (set.current_version(m.key) > 0 &&
                    std::find(stamped.begin(), stamped.end(), m.key) == stamped.end() &&
                    std::find(out.begin(), out.end(), m.key) == out.end())
                    out.push_back(m.key);
                continue; // a payload site is opaque even when already stamped/collected
            }
            find_unstamped_types(m.value, /*is_root=*/false, set, stamped, out);
        }
    }
    else if (value.type == JsonValue::Type::array)
    {
        for (const JsonValue& e : value.elements)
            find_unstamped_types(e, /*is_root=*/false, set, stamped, out);
    }
}

// Apply one step to one payload under the L-37 execution contract. Appends a diagnostic and
// returns false on any violation (the caller rolls the whole document back).
bool apply_step(const MigrationStep& step, JsonValue& payload, const std::string& site_pointer,
                const MigrationBudget& budget, std::vector<MigrationDiagnostic>& diagnostics)
{
    // Tier gating: package-shipped migrations run ONLY in the sandboxed WASM tier (L-37). The VM
    // component is not stood up in v1, so the tier boundary REFUSES in-process execution — the
    // contract is registered; execution is deliberately stubbed, never silently run unsandboxed.
    // The sandboxed WASM runner is the tracked follow-up (issue #71).
    if (step.tier != MigrationTier::engine_native)
    {
        diagnostics.push_back(
            {"migration.runner_unavailable",
             "package-shipped migration \"" + step.component_type + "\" v" +
                 std::to_string(step.from_version) + "->v" + std::to_string(step.from_version + 1) +
                 " executes only in the sandboxed WASM tier (L-37); the VM component is not stood "
                 "up yet, and package steps are never run unsandboxed",
             site_pointer, /*blocking=*/true});
        return false;
    }

    // Budget, input side (deterministic node metric — the sandboxed tier maps the same budget to
    // VM instruction/fuel metering when it lands).
    if (node_count(payload) > budget.max_nodes)
    {
        diagnostics.push_back({"migration.budget_exceeded",
                               "payload exceeds the migration budget (" +
                                   std::to_string(budget.max_nodes) + " nodes) before step v" +
                                   std::to_string(step.from_version),
                               site_pointer, /*blocking=*/true});
        return false;
    }

    // Pre-state for the structural checks.
    std::vector<std::pair<std::string, std::string>> ids_before;
    collect_ids(payload, "", ids_before);
    std::sort(ids_before.begin(), ids_before.end());

    if (!step.transform(payload))
    {
        diagnostics.push_back({"migration.step_failed",
                               "migration step \"" + step.component_type + "\" v" +
                                   std::to_string(step.from_version) + "->v" +
                                   std::to_string(step.from_version + 1) + " reported failure",
                               site_pointer, /*blocking=*/true});
        return false;
    }

    // The output must remain canonically serializable (bans non-finite numbers — R-FILE-001) and
    // within budget.
    std::string canonical_check;
    if (!serializer::serialize_canonical(payload, canonical_check))
    {
        diagnostics.push_back({"migration.step_failed",
                               "migration step \"" + step.component_type + "\" v" +
                                   std::to_string(step.from_version) + "->v" +
                                   std::to_string(step.from_version + 1) +
                                   " produced a non-canonical payload (non-finite number)",
                               site_pointer, /*blocking=*/true});
        return false;
    }
    if (node_count(payload) > budget.max_nodes)
    {
        diagnostics.push_back({"migration.budget_exceeded",
                               "payload exceeds the migration budget (" +
                                   std::to_string(budget.max_nodes) + " nodes) after step v" +
                                   std::to_string(step.from_version),
                               site_pointer, /*blocking=*/true});
        return false;
    }

    // Id immutability (L-37): the exact multiset of ("id"/"guid" pointer, canonical value) must
    // survive — no mutation, move, addition, or removal. Composed identity survives upgrade.
    std::vector<std::pair<std::string, std::string>> ids_after;
    collect_ids(payload, "", ids_after);
    std::sort(ids_after.begin(), ids_after.end());
    if (ids_before != ids_after)
    {
        diagnostics.push_back({"migration.id_mutated",
                               "migration step \"" + step.component_type + "\" v" +
                                   std::to_string(step.from_version) + "->v" +
                                   std::to_string(step.from_version + 1) +
                                   " altered, moved, added, or removed an id/guid member — "
                                   "migrations must never touch identity (L-37)",
                               site_pointer, /*blocking=*/true});
        return false;
    }
    return true;
}

// Rewrite the override "path" strings of every override site through the migrated types' chained
// path maps. An override site is any object member named "overrides" holding an array of objects
// with a string "path". Payload sites are opaque (not scanned — the shared is_payload_boundary
// rule); the root componentVersions header is exempt. Unmappable paths yield NON-blocking
// migration.orphan_override findings; the entry is preserved verbatim (parse-time migration never
// destroys authored data — flatten excludes the orphan by consulting the same rule, L-37).
void rewrite_overrides(JsonValue& value, const std::string& pointer, bool is_root,
                       const std::vector<TypePlan>& plans, const MigrationSet& set,
                       const std::vector<std::string>& stamped, bool& changed,
                       std::vector<MigrationDiagnostic>& diagnostics)
{
    if (value.type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < value.elements.size(); ++i)
            rewrite_overrides(value.elements[i], pointer + "/" + std::to_string(i),
                              /*is_root=*/false, plans, set, stamped, changed, diagnostics);
        return;
    }
    if (value.type != JsonValue::Type::object)
        return;

    for (JsonMember& m : value.members)
    {
        if (is_root && m.key == "componentVersions")
            continue;
        // Opaque payload sites: a member keyed by a stamped/registered type is a payload, not
        // override data (an "overrides"-shaped member INSIDE one is the payload's private data).
        if (m.value.type == JsonValue::Type::object && is_payload_boundary(m.key, set, stamped))
            continue;

        const std::string child = pointer + "/" + escape_segment(m.key);
        if (m.key == "overrides" && m.value.type == JsonValue::Type::array)
        {
            for (std::size_t i = 0; i < m.value.elements.size(); ++i)
            {
                JsonValue& entry = m.value.elements[i];
                JsonValue* path = find_member(entry, "path");
                if (path == nullptr || path->type != JsonValue::Type::string)
                    continue;
                const std::string entry_pointer = child + "/" + std::to_string(i) + "/path";

                // Split the authored path on '/': the LAST segment equal to ANY migrated type splits
                // it into "<prefix>/<type>/<payload-relative-pointer>", and the tail is fed through
                // THAT type's chain (L-37). Selecting by segment POSITION — not plans-iteration
                // order — is what makes a path that names two DIFFERENT migrated types split at the
                // DEEPEST one, exactly the last-segment rule the single-type case already follows.
                // (A prior implementation matched the first plan in iteration order whose type
                // appeared anywhere, silently mis-splitting such a path at the wrong — outer — type.)
                const std::string& authored = path->string_value;
                const TypePlan* selected = nullptr;
                std::size_t type_at = std::string::npos;
                {
                    std::size_t seg_start = 0;
                    for (std::size_t pos = 0; pos <= authored.size(); ++pos)
                    {
                        if (pos == authored.size() || authored[pos] == '/')
                        {
                            for (const TypePlan& plan : plans)
                                if (authored.compare(seg_start, pos - seg_start, plan.type) == 0)
                                {
                                    selected = &plan;    // a later segment overwrites — LAST wins
                                    type_at = seg_start;
                                    break; // types are unique — at most one plan matches a segment
                                }
                            seg_start = pos + 1;
                        }
                    }
                }
                if (selected == nullptr)
                    continue; // no migrated-type segment in this path — nothing to rewrite

                const std::size_t tail_at = type_at + selected->type.size();
                // The payload-relative pointer: "/a/b" for ".../<type>/a/b", "" for a path
                // that addresses the whole payload.
                const std::string tail(authored, std::min(tail_at, authored.size()));
                std::optional<std::string> mapped = tail;
                for (const MigrationStep* step : selected->chain)
                {
                    if (step->map_path && mapped.has_value())
                        mapped = step->map_path(*mapped);
                    if (!mapped.has_value())
                        break;
                }
                if (!mapped.has_value())
                {
                    diagnostics.push_back(
                        {"migration.orphan_override",
                         "override path \"" + authored + "\" has no destination after the \"" +
                             selected->type + "\" v" + std::to_string(selected->from) + "->v" +
                             std::to_string(selected->to) +
                             " migration; the entry is preserved but excluded from flatten "
                             "(L-37 orphan override)",
                         entry_pointer, /*blocking=*/false});
                }
                else if (*mapped != tail)
                {
                    path->string_value = authored.substr(0, type_at) + selected->type + *mapped;
                    changed = true;
                }
            }
            continue;
        }
        rewrite_overrides(m.value, child, /*is_root=*/false, plans, set, stamped, changed,
                          diagnostics);
    }
}

} // namespace

std::uint64_t node_count(const JsonValue& value) noexcept
{
    std::uint64_t n = 1;
    for (const JsonValue& e : value.elements)
        n += node_count(e);
    for (const JsonMember& m : value.members)
        n += 1 + node_count(m.value);
    return n;
}

std::optional<std::string> transform_payload_path(const MigrationSet& set,
                                                  std::string_view component_type,
                                                  std::int64_t from_version,
                                                  std::string_view pointer)
{
    const std::int64_t current = set.current_version(component_type);
    if (current == 0 || from_version < 1 || from_version > current)
        return std::nullopt;
    std::optional<std::string> mapped{std::string(pointer)};
    for (std::int64_t v = from_version; v < current; ++v)
    {
        const MigrationStep* step = set.find_step(component_type, v);
        if (step == nullptr)
            return std::nullopt; // a gap in the chain
        if (step->map_path && mapped.has_value())
            mapped = step->map_path(*mapped);
        if (!mapped.has_value())
            return std::nullopt; // unmapped: the orphan case
    }
    return mapped;
}

bool migrate_payload(const MigrationSet& set, std::string_view component_type,
                     std::int64_t from_version, JsonValue& payload, const MigrationBudget& budget,
                     std::string_view site_pointer, std::vector<MigrationDiagnostic>& diagnostics)
{
    const std::int64_t current = set.current_version(component_type);
    if (current == 0)
        return true; // unregistered: not ours to migrate (the caller decides unknown-type policy)
    if (from_version == current)
        return true; // already current

    const std::string type(component_type);
    const std::string pointer(site_pointer);

    if (from_version > current)
    {
        // The L-37 downgrade rule (R-PKG-005): a payload stamped NEWER than the installed schema is
        // never best-effort migrated — blocking, last-good retained.
        const bool engine_kind = type.rfind("ctx:", 0) == 0;
        diagnostics.push_back(
            {engine_kind ? "schema.newer_than_engine" : "schema.newer_than_package",
             "\"" + type + "\" payload is stamped version " + std::to_string(from_version) +
                 " but the installed schema is version " + std::to_string(current) + "; upgrade the " +
                 (engine_kind ? "engine" : "package") + " that provides it",
             pointer, /*blocking=*/true});
        return false;
    }
    if (from_version < 1)
    {
        diagnostics.push_back({"migration.step_missing",
                               "\"" + type + "\" payload carries an invalid from-version " +
                                   std::to_string(from_version) + " (versions start at 1)",
                               pointer, /*blocking=*/true});
        return false;
    }

    // Pre-check canonical serializability so every apply_step id-snapshot / output serialize is
    // total (mirrors the per-site pre-check in migrate_document).
    std::string pre_check;
    if (!serializer::serialize_canonical(payload, pre_check))
    {
        diagnostics.push_back({"migration.step_failed",
                               "\"" + type +
                                   "\" payload is not canonically serializable (non-finite "
                                   "number) — cannot migrate",
                               pointer, /*blocking=*/true});
        return false;
    }

    // All-or-nothing per payload: snapshot, apply the chain, roll back on any blocking finding.
    const JsonValue snapshot = payload;
    for (std::int64_t v = from_version; v < current; ++v)
    {
        const MigrationStep* step = set.find_step(component_type, v);
        if (step == nullptr)
        {
            diagnostics.push_back({"migration.step_missing",
                                   "no registered migration step takes \"" + type +
                                       "\" from version " + std::to_string(v) + " toward " +
                                       std::to_string(current) + " (a gap in the chain)",
                                   pointer, /*blocking=*/true});
            payload = snapshot;
            return false;
        }
        if (!apply_step(*step, payload, pointer, budget, diagnostics))
        {
            payload = snapshot;
            return false;
        }
    }
    return true;
}

DocumentMigrationResult migrate_document(JsonValue& root, const MigrationSet& set,
                                         const MigrateOptions& options)
{
    DocumentMigrationResult result;
    if (root.type != JsonValue::Type::object || set.empty())
        return result;

    // Read the header stamps through the ONE header reader (L-32). A malformed componentVersions
    // shape is the VALIDATOR's finding (header.* codes, blocking for schema-bound documents) —
    // migration must not half-apply against a header it cannot trust, so it no-ops here.
    std::vector<serializer::Diagnostic> header_diagnostics;
    const serializer::DocumentHeader header =
        serializer::read_document_header(root, header_diagnostics);
    for (const serializer::Diagnostic& d : header_diagnostics)
        if (d.code == "header.component_versions_not_object" ||
            d.code == "header.component_version_not_integer")
            return result;

    // --- selection (per componentVersions entry, authored order — deterministic) ---------------
    std::vector<TypePlan> plans;      // stamped OLDER than registered: migrate
    std::vector<std::string> stamped; // every stamped type (for the unstamped-site scan)
    if (header.has_component_versions)
    {
        for (const auto& [type, version] : header.component_versions)
        {
            stamped.push_back(type);
            const std::int64_t current = set.current_version(type);
            if (current == 0 || version == current)
                continue; // unregistered (not ours) or already current
            if (version > current)
            {
                // The L-37 downgrade rule (R-PKG-005): a payload stamped NEWER than the installed
                // schema is never best-effort parsed — blocking, last-good retained.
                const bool engine_kind = type.rfind("ctx:", 0) == 0;
                result.diagnostics.push_back(
                    {engine_kind ? "schema.newer_than_engine" : "schema.newer_than_package",
                     "\"" + type + "\" payloads are stamped version " + std::to_string(version) +
                         " but the installed schema is version " + std::to_string(current) +
                         "; upgrade the " + (engine_kind ? "engine" : "package") +
                         " or run `context migrate` under the newer one",
                     "/componentVersions/" + escape_segment(type), /*blocking=*/true});
                result.ok = false;
                continue;
            }
            TypePlan plan;
            plan.type = type;
            plan.from = version;
            plan.to = current;
            bool gap = false;
            for (std::int64_t v = version; v < current; ++v)
            {
                const MigrationStep* step = set.find_step(type, v);
                if (step == nullptr)
                {
                    result.diagnostics.push_back(
                        {"migration.step_missing",
                         "no registered migration step takes \"" + type + "\" from version " +
                             std::to_string(v) + " toward " + std::to_string(current) +
                             " (a gap in the chain)",
                         "/componentVersions/" + escape_segment(type), /*blocking=*/true});
                    result.ok = false;
                    gap = true;
                    break;
                }
                plan.chain.push_back(step);
            }
            if (!gap)
                plans.push_back(std::move(plan));
        }
    }

    // Tool-save stamping (L-37): registered types present as payload sites but carrying no stamp
    // are authored at the current version by definition — the stamp affirms it.
    std::vector<std::string> unstamped;
    if (options.stamp_registered_sites)
        find_unstamped_types(root, /*is_root=*/true, set, stamped, unstamped);

    if (!result.ok)
    {
        // Blocking selection findings (newer-than / gaps): all-or-nothing per document — nothing
        // was applied yet, so the tree is already its pre-call self.
        return result;
    }
    if (plans.empty() && unstamped.empty())
        return result; // nothing to do

    // --- application (all-or-nothing per document: snapshot, apply, roll back on blocking) ------
    const JsonValue snapshot = root;

    for (const TypePlan& plan : plans)
    {
        std::vector<PayloadSite> sites;
        find_sites(root, "", plan.type, /*is_root=*/true, set, stamped, sites);
        for (const PayloadSite& site : sites)
        {
            // Pre-check: the payload must be canonically serializable BEFORE the chain, so every
            // later serialize (id snapshots, output checks) is total.
            std::string pre_check;
            if (!serializer::serialize_canonical(*site.payload, pre_check))
            {
                result.diagnostics.push_back({"migration.step_failed",
                                              "payload is not canonically serializable "
                                              "(non-finite number) — cannot migrate",
                                              site.pointer, /*blocking=*/true});
                result.ok = false;
                break;
            }
            for (const MigrationStep* step : plan.chain)
            {
                if (!apply_step(*step, *site.payload, site.pointer, options.budget,
                                result.diagnostics))
                {
                    result.ok = false;
                    break;
                }
            }
            if (!result.ok)
                break;
            result.changed = true;
        }
        if (!result.ok)
            break;
    }

    if (result.ok && !plans.empty())
    {
        // Override/reference path transforms through the same chains (L-37: migrations transform
        // paths as well as payloads). Non-blocking orphan findings may be appended here.
        rewrite_overrides(root, "", /*is_root=*/true, plans, set, stamped, result.changed,
                          result.diagnostics);
    }

    if (!result.ok)
    {
        root = snapshot; // all-or-nothing: blocking finding => the document is untouched
        result.changed = false;
        return result;
    }

    // --- stamping (structural header edits LAST — they may reallocate root.members) -------------
    if (!plans.empty() || !unstamped.empty())
    {
        JsonValue* versions = find_member(root, "componentVersions");
        if (versions == nullptr)
        {
            JsonMember member;
            member.key = "componentVersions";
            member.value.type = JsonValue::Type::object;
            root.members.push_back(std::move(member));
            versions = &root.members.back().value;
        }
        for (const TypePlan& plan : plans)
        {
            JsonValue* stamp = find_member(*versions, plan.type);
            if (stamp != nullptr)
            {
                stamp->type = JsonValue::Type::integer;
                stamp->int_value = plan.to;
                result.changed = true;
            }
        }
        for (const std::string& type : unstamped)
        {
            JsonMember member;
            member.key = type;
            member.value.type = JsonValue::Type::integer;
            member.value.int_value = set.current_version(type);
            versions->members.push_back(std::move(member));
            result.changed = true;
        }
    }

    return result;
}

} // namespace context::editor::migrate

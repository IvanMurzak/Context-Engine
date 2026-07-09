// The `context` merge family implementation (see merge_command.h).

#include "context/cli/merge_command.h"

#include "context/editor/compose/stable_id.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "context/editor/merge/conflict.h"
#include "context/editor/merge/rekey.h"
#include "context/editor/merge/resolve.h"
#include "context/editor/merge/three_way_merge.h"
#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include "json_walk.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
using editor::contract::Registry;
namespace merge = editor::merge;
namespace schema = editor::schema;
namespace compose = editor::compose;
namespace serializer = editor::serializer;
namespace fs = std::filesystem;

namespace
{

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// Atomic-enough in-place write for a one-shot CLI (mirrors migrate_command): temp sibling + rename.
bool write_file_atomically(const fs::path& path, const std::string& bytes)
{
    const fs::path temp = path.string() + ".ctx-merge-tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec)
    {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

// serializer JsonValue -> contract Json (through canonical bytes). Null on a non-serializable tree.
Json to_contract(const serializer::JsonValue& v)
{
    std::string bytes;
    if (!serializer::serialize_canonical(v, bytes))
        return Json();
    return Json::parse(bytes);
}

// contract Json -> serializer JsonValue (through compact bytes). Used for a sidecar-sourced value.
serializer::JsonValue to_serializer(const Json& j)
{
    serializer::ParseResult r = serializer::parse_json(j.dump(0));
    return std::move(r.root);
}

// One conflict as the R-CLI-008 envelope entry {path, class, id?, base?, ours?, theirs?}. Absent
// sides are OMITTED (so absence is unambiguous — a field added on both sides has no `base`).
Json conflict_to_json(const merge::Conflict& c)
{
    Json entry = Json::object();
    entry.set("path", Json(c.path));
    entry.set("class", Json(merge::to_string(c.klass)));
    entry.set("code", Json(merge::catalog_code(c.klass)));
    if (!c.id.empty())
        entry.set("id", Json(c.id));
    if (c.base.has_value())
        entry.set("base", to_contract(*c.base));
    if (c.ours.has_value())
        entry.set("ours", to_contract(*c.ours));
    if (c.theirs.has_value())
        entry.set("theirs", to_contract(*c.theirs));
    return entry;
}

Json conflicts_to_json(const std::vector<merge::Conflict>& conflicts)
{
    Json out = Json::array();
    for (const merge::Conflict& c : conflicts)
        out.push_back(conflict_to_json(c));
    return out;
}

// The installed-schema floor from the live registry (kind id -> version). Drives L-37 newer-stamped
// whole-file detection; an unregistered kind is never judged.
merge::SchemaFloor registry_floor()
{
    merge::SchemaFloor floor;
    for (const editor::contract::FileKindSpec& kind : Registry::instance().file_kinds())
        floor.kind_versions[kind.id] = kind.version;
    return floor;
}

// Write / clear the conflict sidecar next to the merged file (the resolve loop's input).
void write_sidecar(const fs::path& sidecar, const std::string& pathname,
                   const std::vector<merge::Conflict>& conflicts)
{
    Json doc = Json::object();
    doc.set("path", Json(pathname));
    doc.set("conflicts", conflicts_to_json(conflicts));
    std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
    if (out)
        out << doc.dump(2) << "\n";
}

void remove_sidecar(const fs::path& sidecar)
{
    std::error_code ec;
    fs::remove(sidecar, ec);
}

// Whole-file 3-way over raw bytes (binary sidecars are NEVER content-merged — L-33). Returns the
// merged bytes; `conflicted` is set when both sides changed the file differently.
std::string merge_binary(const std::string& base, const std::string& ours, const std::string& theirs,
                         bool base_present, bool& conflicted)
{
    conflicted = false;
    if (ours == theirs)
        return ours;
    if (base_present && base == ours)
        return theirs; // only theirs changed
    if (base_present && base == theirs)
        return ours; // only ours changed
    conflicted = true; // both changed differently => whole-file ours/theirs
    return ours;
}

// A canonical RFC 6901 array-index token: plain digits, no leading zero beyond "0", length-capped so
// a pathological digit-run never reaches std::stoull (which would throw). Mirrors the merge module's
// internal detail::parse_array_index — duplicated here rather than reaching across into that module's
// PRIVATE src/pointer_format.h from the CLI layer.
bool parse_index_token(const std::string& token, std::size_t& out)
{
    if (token.empty() || token.size() > 10 ||
        token.find_first_not_of("0123456789") != std::string::npos ||
        (token.size() > 1 && token[0] == '0'))
        return false;
    out = static_cast<std::size_t>(std::stoull(token));
    return true;
}

// After a resolution DELETED the array element at `removed_parent`[`removed_index`], a surviving
// conflict entry addressing a LATER element of the SAME array carries a now-stale index (the erase
// shifted every later element down one). Return `path` with that index decremented so a subsequent
// `resolve-conflict --path` still targets the intended element — never a silently mis-targeted
// neighbour. A path into a different container, or an object key that merely shares the prefix, is
// returned unchanged (RFC 6901 array indices are plain digits, so a non-index token is left alone).
std::string reindex_after_array_removal(const std::string& path, const std::string& removed_parent,
                                        std::size_t removed_index)
{
    const std::string prefix = removed_parent + "/";
    if (path.size() < prefix.size() || path.compare(0, prefix.size(), prefix) != 0)
        return path; // a different container (or the container pointer itself)
    const std::string rest = path.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    std::size_t index = 0;
    if (!parse_index_token(rest.substr(0, slash), index) || index <= removed_index)
        return path; // an object key, or an earlier/equal element — unaffected by the shift
    const std::string tail = slash == std::string::npos ? std::string() : rest.substr(slash);
    return prefix + std::to_string(index - 1) + tail;
}

// One stable-id FORMAT finding (L-33): an object carries a string `id` member whose value is not the
// pinned stable-id form (16..32 lowercase hex). Distinct from the duplicate-id gate — a MALFORMED id
// slips past uniqueness entirely (the dup walk only groups ids that already have stable form), so a
// non-hex id shipped silent until this check (issue #108 Gap 1).
struct InvalidStableId
{
    std::string pointer; // RFC 6901 pointer to the offending object's /id
    std::string id;      // the malformed id value (verbatim)
};

// Escape one object key for embedding in an RFC 6901 pointer ('~' -> "~0", '/' -> "~1").
std::string pointer_escape(const std::string& key)
{
    std::string out;
    out.reserve(key.size());
    for (const char ch : key)
    {
        if (ch == '~')
            out += "~0";
        else if (ch == '/')
            out += "~1";
        else
            out.push_back(ch);
    }
    return out;
}

// Depth-first walk collecting every object with a string `id` member of NON-stable-id form. Scoped by
// the caller to schema-bound authored documents (scene/tilemap kinds declare `id` as a stable id under
// additionalProperties:false), so a non-authored file that merely carries an `id` string — a CI/bench
// manifest, a test vector — is never format-checked and cannot be false-rejected (issue #108).
void collect_invalid_stable_ids(const serializer::JsonValue& node, const std::string& pointer,
                                std::vector<InvalidStableId>& out)
{
    if (node.type == serializer::JsonValue::Type::object)
    {
        for (const serializer::JsonMember& m : node.members)
            if (m.key == "id" && m.value.type == serializer::JsonValue::Type::string &&
                !compose::is_stable_id(m.value.string_value))
                out.push_back({pointer + "/id", m.value.string_value});
        for (const serializer::JsonMember& m : node.members)
            collect_invalid_stable_ids(m.value, pointer + "/" + pointer_escape(m.key), out);
    }
    else if (node.type == serializer::JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < node.elements.size(); ++i)
            collect_invalid_stable_ids(node.elements[i], pointer + "/" + std::to_string(i), out);
    }
}

// A schema ValidationDiagnostic that is INFORMATIONAL (kinds/versions register incrementally, so an
// unrecognized binding must never fail validation). Mirrors validator.cpp's internal is_blocking_code.
bool is_blocking_schema_code(const std::string& code) noexcept
{
    return code != "schema.unknown_kind" && code != "schema.version_unregistered";
}

} // namespace

Envelope run_merge_file(const std::map<std::string, std::string>& params,
                        const std::map<std::string, std::string>& flags)
{
    auto param = [&](const char* k) { auto it = params.find(k); return it != params.end() ? it->second : std::string(); };
    auto flag = [&](const char* k) { auto it = flags.find(k); return it != flags.end() ? it->second : std::string(); };

    const std::string base_path = param("base");
    const std::string ours_path = param("ours");
    const std::string theirs_path = param("theirs");
    const bool driver = flags.find("driver") != flags.end();
    const bool dry_run = flags.find("dry-run") != flags.end(); // core flag: report without writing
    const std::string output = !flag("output").empty() ? flag("output") : ours_path;
    const std::string pathname = !param("pathname").empty() ? param("pathname") : output;
    const fs::path sidecar = pathname + ".ctxconflicts.json";

    std::string ours_text;
    std::string theirs_text;
    if (!read_file(ours_path, ours_text))
        return Envelope::failure("file.not_found", "merge input not found: " + ours_path);
    if (!read_file(theirs_path, theirs_text))
        return Envelope::failure("file.not_found", "merge input not found: " + theirs_path);
    std::string base_text;
    const bool base_present = read_file(base_path, base_text) && !base_text.empty();

    const serializer::CanonicalizeResult ours_c = serializer::canonicalize(ours_text);
    const serializer::CanonicalizeResult theirs_c = serializer::canonicalize(theirs_text);

    // --- binary / non-JSON inputs: whole-file ours/theirs (never content-merged) ----------------
    if (!ours_c.is_json || !theirs_c.is_json)
    {
        bool conflicted = false;
        const std::string merged = merge_binary(base_text, ours_text, theirs_text, base_present, conflicted);
        if (!dry_run && !write_file_atomically(output, merged))
            return Envelope::failure("internal.error", "could not write merged output: " + output);

        if (!conflicted)
        {
            if (!dry_run)
                remove_sidecar(sidecar);
            Json data = Json::object();
            data.set("merged", Json(output));
            data.set("clean", Json(true));
            data.set("wholeFile", Json(false));
            data.set("conflicts", Json::array());
            data.set("dryRun", Json(dry_run));
            return Envelope::success(std::move(data));
        }

        merge::Conflict c;
        c.path = "";
        c.klass = merge::ConflictClass::binary_sidecar; // binary bytes are not embedded as JSON
        std::vector<merge::Conflict> conflicts{c};
        if (!dry_run)
            write_sidecar(sidecar, pathname, conflicts);
        if (driver)
            return Envelope::failure(merge::catalog_code(c.klass),
                                     "binary sidecar differs on both sides — resolve whole-file "
                                     "ours/theirs (context resolve-conflict).");
        Json data = Json::object();
        data.set("merged", Json(output));
        data.set("clean", Json(false));
        data.set("wholeFile", Json(true));
        data.set("conflicts", conflicts_to_json(conflicts));
        data.set("dryRun", Json(dry_run));
        return Envelope::success(std::move(data));
    }

    // --- JSON inputs: structural three-way merge ------------------------------------------------
    serializer::JsonValue base_tree;
    const serializer::CanonicalizeResult base_c = serializer::canonicalize(base_text);
    if (base_c.is_json)
        base_tree = base_c.root;
    else
        base_tree.type = serializer::JsonValue::Type::object; // absent/empty ancestor (add/add)

    merge::MergeOptions opts;
    opts.floor = registry_floor();
    const merge::MergeResult result =
        merge::merge_documents(base_tree, ours_c.root, theirs_c.root, opts);

    std::string merged_bytes;
    if (!serializer::serialize_canonical(result.merged, merged_bytes))
        return Envelope::failure("internal.error", "the merged tree did not serialize");
    if (!dry_run && !write_file_atomically(output, merged_bytes))
        return Envelope::failure("internal.error", "could not write merged output: " + output);

    if (result.clean)
    {
        if (!dry_run)
            remove_sidecar(sidecar);
        Json data = Json::object();
        data.set("merged", Json(output));
        data.set("clean", Json(true));
        data.set("wholeFile", Json(false));
        data.set("conflicts", Json::array());
        data.set("dryRun", Json(dry_run));
        return Envelope::success(std::move(data));
    }

    if (!dry_run)
        write_sidecar(sidecar, pathname, result.conflicts);

    if (driver)
    {
        const char* code = merge::catalog_code(result.conflicts.front().klass);
        return Envelope::failure(code, std::to_string(result.conflicts.size()) +
                                           " unresolved conflict(s); wrote " + sidecar.string() +
                                           " for `context resolve-conflict`.");
    }

    Json data = Json::object();
    data.set("merged", Json(output));
    data.set("clean", Json(false));
    data.set("wholeFile", Json(result.whole_file));
    data.set("conflicts", conflicts_to_json(result.conflicts));
    data.set("sidecar", Json(sidecar.string()));
    data.set("dryRun", Json(dry_run));
    return Envelope::success(std::move(data));
}

Envelope run_resolve_conflict(const std::map<std::string, std::string>& params,
                              const std::map<std::string, std::string>& flags)
{
    auto flag = [&](const char* k) { auto it = flags.find(k); return it != flags.end() ? it->second : std::string(); };

    const std::string file = params.count("file") ? params.at("file") : std::string();
    const std::string path = flag("path");
    const std::string take = flag("take");
    const bool has_value = flags.find("value") != flags.end();
    const bool dry_run = flags.find("dry-run") != flags.end(); // core flag: report without writing

    if (file.empty())
        return Envelope::failure("usage.missing_argument", "a target <file> is required");
    if (flags.find("path") == flags.end())
        return Envelope::failure("usage.missing_argument",
                                 "--path is required (pass --path \"\" for a whole-file conflict)");
    if (take.empty() && !has_value)
        return Envelope::failure("usage.missing_argument", "one of --take ours|theirs or --value is required");
    if (!take.empty() && has_value)
        return Envelope::failure("usage.invalid",
                                 "--take and --value are mutually exclusive; pass exactly one");
    if (!take.empty() && take != "ours" && take != "theirs")
        return Envelope::failure("usage.invalid", "--take must be 'ours' or 'theirs'");

    std::string text;
    if (!read_file(file, text))
        return Envelope::failure("file.not_found", "file not found: " + file);
    serializer::ParseResult parsed = serializer::parse_json(text);
    if (!parsed.ok)
        return Envelope::failure("file.parse_error", "the target file is not well-formed JSON: " + file);
    serializer::JsonValue root = std::move(parsed.root);

    const fs::path sidecar = file + ".ctxconflicts.json";
    Json sidecar_doc = Json::object();
    bool have_sidecar = false;
    if (std::string sc; read_file(sidecar, sc))
    {
        try
        {
            sidecar_doc = Json::parse(sc);
        }
        catch (const std::exception&)
        {
            return Envelope::failure("file.parse_error",
                                     "the conflict sidecar is not well-formed JSON: " + sidecar.string());
        }
        have_sidecar = true;
    }

    // Resolve the value to apply: --value is self-contained; --take reads the conflict sidecar.
    std::optional<serializer::JsonValue> value;
    if (has_value)
    {
        serializer::ParseResult v = serializer::parse_json(flag("value"));
        if (!v.ok)
            return Envelope::failure("usage.invalid", "--value is not well-formed JSON");
        value = std::move(v.root);
    }
    else
    {
        if (!have_sidecar)
            return Envelope::failure("merge.no_conflict_at_path",
                                     "no conflict sidecar next to " + file + " (nothing to --take)");
        const Json& conflicts = sidecar_doc.at("conflicts");
        const Json* entry = nullptr;
        for (std::size_t i = 0; i < conflicts.size(); ++i)
            if (conflicts.at(i).at("path").as_string() == path)
                entry = &conflicts.at(i);
        if (entry == nullptr)
            return Envelope::failure("merge.no_conflict_at_path",
                                     "no open conflict at --path " + path);
        if (entry->contains(take))
            value = to_serializer(entry->at(take)); // the chosen side's value
        // else: the chosen side ABSENT (a delete) => value stays nullopt (a removal)
    }

    const merge::ApplyResult applied = merge::apply_resolution(root, path, value);
    if (!applied.ok)
        return Envelope::failure("usage.invalid", applied.error);

    std::string out_bytes;
    if (!serializer::serialize_canonical(root, out_bytes))
        return Envelope::failure("internal.error", "the resolved tree did not serialize");
    if (!dry_run && !write_file_atomically(file, out_bytes))
        return Envelope::failure("internal.error", "could not write " + file);

    // Drop the resolved entry from the sidecar; reindex any survivor addressing a LATER element of an
    // array this resolution shrank (else its recorded index now points one slot too high); delete the
    // sidecar when it empties.
    std::uint64_t remaining = 0;
    if (have_sidecar)
    {
        Json kept = Json::array();
        const Json& conflicts = sidecar_doc.at("conflicts");
        for (std::size_t i = 0; i < conflicts.size(); ++i)
        {
            if (conflicts.at(i).at("path").as_string() == path)
                continue; // the just-resolved entry
            Json entry = conflicts.at(i);
            if (applied.removed_array_element)
            {
                const std::string entry_path = entry.at("path").as_string();
                const std::string reindexed = reindex_after_array_removal(
                    entry_path, applied.removed_array_pointer, applied.removed_index);
                if (reindexed != entry_path)
                    entry.set("path", Json(reindexed));
            }
            kept.push_back(std::move(entry));
        }
        remaining = kept.size();
        if (!dry_run)
        {
            if (remaining == 0)
            {
                remove_sidecar(sidecar);
            }
            else
            {
                Json doc = Json::object();
                doc.set("path", sidecar_doc.contains("path") ? sidecar_doc.at("path") : Json(file));
                doc.set("conflicts", std::move(kept));
                std::ofstream o(sidecar, std::ios::binary | std::ios::trunc);
                if (o)
                    o << doc.dump(2) << "\n";
            }
        }
    }

    Json data = Json::object();
    data.set("file", Json(file));
    data.set("path", Json(path));
    data.set("resolution", Json(has_value ? std::string("value") : take));
    data.set("remainingConflicts", Json(remaining));
    data.set("dryRun", Json(dry_run));
    return Envelope::success(std::move(data));
}

Envelope run_rekey(const std::map<std::string, std::string>& params,
                   const std::map<std::string, std::string>& flags)
{
    auto flag = [&](const char* k) { auto it = flags.find(k); return it != flags.end() ? it->second : std::string(); };

    const std::string file = params.count("file") ? params.at("file") : std::string();
    const std::string at = flag("at");
    const std::string id = flag("id");
    const bool dry_run = flags.find("dry-run") != flags.end(); // core flag: report without writing
    if (file.empty())
        return Envelope::failure("usage.missing_argument", "a target <file> is required");
    if (at.empty() && id.empty())
        return Envelope::failure("usage.missing_argument", "one of --at <pointer> or --id <id> is required");
    if (!at.empty() && !id.empty())
        return Envelope::failure("usage.invalid",
                                 "--at and --id are mutually exclusive; pass exactly one");

    std::string text;
    if (!read_file(file, text))
        return Envelope::failure("file.not_found", "file not found: " + file);
    serializer::ParseResult parsed = serializer::parse_json(text);
    if (!parsed.ok)
        return Envelope::failure("file.parse_error", "the target file is not well-formed JSON: " + file);
    serializer::JsonValue root = std::move(parsed.root);

    std::string pointer = at;
    if (pointer.empty())
    {
        // --id: re-key the LAST holder of a duplicated id (the split-a-collision remedy).
        const std::vector<merge::DuplicateId> dups = merge::find_duplicate_ids(root);
        const merge::DuplicateId* group = nullptr;
        for (const merge::DuplicateId& d : dups)
            if (d.id == id)
                group = &d;
        if (group == nullptr)
            return Envelope::failure("merge.rekey_target_invalid",
                                     "'" + id + "' is not a duplicated intra-file id; use --at to "
                                     "re-key a specific object");
        pointer = group->pointers.back();
    }

    const merge::RekeyResult r = merge::rekey_entity(root, pointer);
    if (!r.ok)
        return Envelope::failure("merge.rekey_target_invalid", r.error);

    std::string out_bytes;
    if (!serializer::serialize_canonical(root, out_bytes))
        return Envelope::failure("internal.error", "the re-keyed tree did not serialize");
    if (!dry_run && !write_file_atomically(file, out_bytes))
        return Envelope::failure("internal.error", "could not write " + file);

    Json data = Json::object();
    data.set("file", Json(file));
    data.set("oldId", Json(r.old_id));
    data.set("newId", Json(r.new_id));
    data.set("referencesRewritten", Json(r.references_rewritten));
    data.set("dryRun", Json(dry_run));
    return Envelope::success(std::move(data));
}

Envelope run_validate(const std::map<std::string, std::string>& params,
                      const std::map<std::string, std::string>& flags)
{
    const std::string target = params.count("path") ? params.at("path")
                               : (flags.count("project") ? flags.at("project") : std::string("."));
    const fs::path target_path(target);

    std::error_code ec;
    if (!fs::exists(target_path, ec) || ec)
        return Envelope::failure("file.not_found", "validate target does not exist: " + target);

    // Candidate files: the target file, or every *.json under it (skip dot-dirs) — the ONE shared
    // migrate/validate rule (json_walk.h), so the two verbs' file selection cannot drift.
    const std::vector<fs::path> files = collect_json_candidates(target_path);

    Json diagnostics = Json::array();
    std::uint64_t scanned = 0;
    std::uint64_t duplicate_ids = 0;
    std::uint64_t invalid_stable_ids = 0; // Gap 1: stable-id FORMAT (L-33)
    std::uint64_t schema_violations = 0;  // Gap 2: schema-SHAPE via editor::schema Validator
    const schema::SchemaSet& schemas = schema::engine_schemas();
    for (const fs::path& file : files)
    {
        std::string text;
        if (!read_file(file, text))
            continue;
        serializer::ParseResult parsed = serializer::parse_json(text);
        if (!parsed.ok)
            continue; // non-JSON / malformed files are out of scope for the validate gates
        ++scanned;
        // Gate 1 — duplicate intra-file ids (the post-merge convergence gate).
        for (const merge::DuplicateId& dup : merge::find_duplicate_ids(parsed.root))
        {
            ++duplicate_ids;
            Json entry = Json::object();
            entry.set("code", Json("merge.duplicate_id"));
            entry.set("file", Json(file.generic_string()));
            entry.set("id", Json(dup.id));
            Json pointers = Json::array();
            for (const std::string& p : dup.pointers)
                pointers.push_back(Json(p));
            entry.set("pointers", std::move(pointers));
            entry.set("message", Json("intra-file id '" + dup.id + "' is carried by " +
                                      std::to_string(dup.pointers.size()) +
                                      " objects — re-key one via `context re-key`"));
            diagnostics.push_back(std::move(entry));
        }

        // The schema report drives BOTH remaining gates: schema_bound gates the stable-id FORMAT walk
        // (so only authored kinds are format-checked — never a foreign JSON that merely carries an
        // `id`), and its blocking diagnostics are the schema-SHAPE findings (issue #108).
        const schema::ValidationReport report =
            schema::validate_document(parsed.root, text, schemas);

        // Gate 2 — stable-id FORMAT (L-33): 16..32 lowercase hex. Scoped to schema-bound authored
        // documents; a malformed id escapes the duplicate-id gate entirely (that gate only groups ids
        // ALREADY of stable form), so this is the check that catches a non-hex authored id.
        if (report.schema_bound)
        {
            std::vector<InvalidStableId> bad;
            collect_invalid_stable_ids(parsed.root, "", bad);
            for (const InvalidStableId& b : bad)
            {
                ++invalid_stable_ids;
                Json entry = Json::object();
                entry.set("code", Json("merge.invalid_stable_id"));
                entry.set("file", Json(file.generic_string()));
                entry.set("id", Json(b.id));
                entry.set("pointer", Json(b.pointer));
                entry.set("message",
                          Json("stable intra-file id '" + b.id +
                               "' is not the L-33 form (16..32 lowercase hex chars)"));
                diagnostics.push_back(std::move(entry));
            }
        }

        // Gate 3 — schema SHAPE via the editor::schema Validator (R-DATA-006): a payload that fails
        // its kind schema (a wrong-typed / missing / undeclared field) is a validate error, so a
        // shape bug can no longer pass the gate green. Only BLOCKING findings are reported; an
        // unregistered kind/version is informational and never fails validation.
        if (!report.ok)
            for (const schema::ValidationDiagnostic& d : report.diagnostics)
            {
                if (!is_blocking_schema_code(d.code))
                    continue;
                ++schema_violations;
                Json entry = Json::object();
                entry.set("code", Json(d.code));
                entry.set("file", Json(file.generic_string()));
                entry.set("pointer", Json(d.pointer));
                entry.set("line", Json(static_cast<std::int64_t>(d.line)));
                entry.set("column", Json(static_cast<std::int64_t>(d.column)));
                entry.set("message", Json(d.message));
                diagnostics.push_back(std::move(entry));
            }
    }

    const bool valid = duplicate_ids == 0 && invalid_stable_ids == 0 && schema_violations == 0;
    Json data = Json::object();
    data.set("target", Json(target_path.generic_string()));
    data.set("scanned", Json(scanned));
    data.set("valid", Json(valid));
    data.set("duplicateIds", Json(duplicate_ids));
    data.set("invalidStableIds", Json(invalid_stable_ids));
    data.set("schemaViolations", Json(schema_violations));
    data.set("diagnostics", std::move(diagnostics));
    return Envelope::success(std::move(data));
}

} // namespace context::cli

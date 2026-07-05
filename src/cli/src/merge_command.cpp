// The `context` merge family implementation (see merge_command.h).

#include "context/cli/merge_command.h"

#include "context/editor/contract/json.h"
#include "context/editor/contract/registry.h"
#include "context/editor/merge/conflict.h"
#include "context/editor/merge/rekey.h"
#include "context/editor/merge/resolve.h"
#include "context/editor/merge/three_way_merge.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/json_tree.h"

#include <algorithm>
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
    std::error_code ec;
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
        if (!write_file_atomically(output, merged))
            return Envelope::failure("internal.error", "could not write merged output: " + output);

        if (!conflicted)
        {
            remove_sidecar(sidecar);
            Json data = Json::object();
            data.set("merged", Json(output));
            data.set("clean", Json(true));
            data.set("wholeFile", Json(false));
            data.set("conflicts", Json::array());
            return Envelope::success(std::move(data));
        }

        merge::Conflict c;
        c.path = "";
        c.klass = merge::ConflictClass::binary_sidecar; // binary bytes are not embedded as JSON
        std::vector<merge::Conflict> conflicts{c};
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
    if (!write_file_atomically(output, merged_bytes))
        return Envelope::failure("internal.error", "could not write merged output: " + output);

    if (result.clean)
    {
        remove_sidecar(sidecar);
        Json data = Json::object();
        data.set("merged", Json(output));
        data.set("clean", Json(true));
        data.set("wholeFile", Json(false));
        data.set("conflicts", Json::array());
        return Envelope::success(std::move(data));
    }

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

    if (file.empty())
        return Envelope::failure("usage.missing_argument", "a target <file> is required");
    if (path.empty())
        return Envelope::failure("usage.missing_argument", "--path is required");
    if (take.empty() && !has_value)
        return Envelope::failure("usage.missing_argument", "one of --take ours|theirs or --value is required");
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
        sidecar_doc = Json::parse(sc);
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
    if (!write_file_atomically(file, out_bytes))
        return Envelope::failure("internal.error", "could not write " + file);

    // Drop the resolved entry from the sidecar; delete the sidecar when it empties.
    std::uint64_t remaining = 0;
    if (have_sidecar)
    {
        Json kept = Json::array();
        const Json& conflicts = sidecar_doc.at("conflicts");
        for (std::size_t i = 0; i < conflicts.size(); ++i)
            if (conflicts.at(i).at("path").as_string() != path)
                kept.push_back(conflicts.at(i));
        remaining = kept.size();
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

    Json data = Json::object();
    data.set("file", Json(file));
    data.set("path", Json(path));
    data.set("resolution", Json(has_value ? std::string("value") : take));
    data.set("remainingConflicts", Json(remaining));
    return Envelope::success(std::move(data));
}

Envelope run_rekey(const std::map<std::string, std::string>& params,
                   const std::map<std::string, std::string>& flags)
{
    auto flag = [&](const char* k) { auto it = flags.find(k); return it != flags.end() ? it->second : std::string(); };

    const std::string file = params.count("file") ? params.at("file") : std::string();
    const std::string at = flag("at");
    const std::string id = flag("id");
    if (file.empty())
        return Envelope::failure("usage.missing_argument", "a target <file> is required");
    if (at.empty() && id.empty())
        return Envelope::failure("usage.missing_argument", "one of --at <pointer> or --id <id> is required");

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
    if (!write_file_atomically(file, out_bytes))
        return Envelope::failure("internal.error", "could not write " + file);

    Json data = Json::object();
    data.set("file", Json(file));
    data.set("oldId", Json(r.old_id));
    data.set("newId", Json(r.new_id));
    data.set("referencesRewritten", Json(r.references_rewritten));
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

    // Candidate files: the target file, or every *.json under it (skip dot-dirs) — the migrate rule.
    std::vector<fs::path> files;
    if (fs::is_regular_file(target_path))
    {
        files.push_back(target_path);
    }
    else
    {
        fs::recursive_directory_iterator it(target_path, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        while (!ec && it != end)
        {
            const fs::directory_entry& entry = *it;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(ec))
            {
                if (!name.empty() && name[0] == '.')
                    it.disable_recursion_pending();
            }
            else if (entry.is_regular_file(ec) && name.size() > 5 &&
                     name.compare(name.size() - 5, 5, ".json") == 0)
            {
                files.push_back(entry.path());
            }
            it.increment(ec);
        }
        std::sort(files.begin(), files.end());
    }

    Json diagnostics = Json::array();
    std::uint64_t scanned = 0;
    std::uint64_t duplicate_ids = 0;
    for (const fs::path& file : files)
    {
        std::string text;
        if (!read_file(file, text))
            continue;
        serializer::ParseResult parsed = serializer::parse_json(text);
        if (!parsed.ok)
            continue; // non-JSON / malformed files are out of scope for the dup-id gate
        ++scanned;
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
    }

    Json data = Json::object();
    data.set("target", Json(target_path.generic_string()));
    data.set("scanned", Json(scanned));
    data.set("valid", Json(duplicate_ids == 0));
    data.set("duplicateIds", Json(duplicate_ids));
    data.set("diagnostics", std::move(diagnostics));
    return Envelope::success(std::move(data));
}

} // namespace context::cli

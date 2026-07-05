// `context set` (composed write) + `context query --overrides` (advisory hygiene) — see set_command.h.

#include "context/cli/set_command.h"

#include "context/editor/compose/compose_write.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <cstdint>
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
namespace compose = editor::compose;
namespace filesync = editor::filesync;
namespace serializer = editor::serializer;
namespace fs = std::filesystem;

namespace
{

bool read_file(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// Split a slash-separated id-path token ("a/b/c") into its segments. Rejects empty segments (a
// leading/trailing/doubled '/'), so a malformed id-path is a clean usage error rather than an
// id-path with an empty segment that could never resolve.
[[nodiscard]] bool split_id_path(const std::string& text, std::vector<std::string>& out)
{
    out.clear();
    if (text.empty())
        return false;
    std::string segment;
    std::istringstream stream(text);
    while (std::getline(stream, segment, '/'))
    {
        if (segment.empty())
            return false;
        out.push_back(segment);
    }
    // A trailing '/' leaves getline without a final empty read; guard it explicitly.
    return !out.empty() && text.back() != '/';
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out)
{
    if (text.empty())
        return false;
    std::uint64_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

// The lazy project scene resolver: reads + canonicalizes + composition-parses scene files under the
// project root on demand, caching the raw parsed tree (for the write splice) and the composition
// view (for the id-path walk). The R-SEC-008 jail is enforced lexically here (a scene / instance
// path escaping the project root simply does not resolve); the native FileStore re-enforces it
// TOCTOU-safely on the actual write.
class ProjectResolver final : public compose::WriteResolver
{
public:
    explicit ProjectResolver(fs::path root) : root_(std::move(root)) {}

    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        const Entry& e = load(std::string(path));
        return e.has_doc ? &e.doc : nullptr;
    }
    [[nodiscard]] const serializer::JsonValue* tree(std::string_view path) const override
    {
        const Entry& e = load(std::string(path));
        return e.has_tree ? &e.tree : nullptr;
    }

private:
    struct Entry
    {
        bool has_tree = false;
        serializer::JsonValue tree;
        bool has_doc = false;
        compose::SceneDoc doc;
    };

    [[nodiscard]] const Entry& load(const std::string& path) const
    {
        if (const auto it = cache_.find(path); it != cache_.end())
            return it->second;
        Entry entry;
        // The path must stay within the project root (R-SEC-008); an escaping instance/scene path
        // resolves to nothing (the write path then reports write_target_not_found / file.not_found).
        if (filesync::is_inside_jail(".", path))
        {
            std::string bytes;
            if (read_file(root_ / filesync::normalize_path(path), bytes))
            {
                serializer::CanonicalizeResult canonical = serializer::canonicalize(bytes);
                if (canonical.is_json)
                {
                    if (std::optional<compose::SceneDoc> doc =
                            compose::build_scene_doc(path, canonical.root))
                    {
                        entry.has_doc = true;
                        entry.doc = std::move(*doc);
                    }
                    entry.has_tree = true;
                    entry.tree = std::move(canonical.root);
                }
            }
        }
        const auto [inserted, _] = cache_.emplace(path, std::move(entry));
        return inserted->second;
    }

    fs::path root_;
    mutable std::map<std::string, Entry, std::less<>> cache_;
};

[[nodiscard]] const char* target_name(compose::WriteTarget target)
{
    switch (target)
    {
    case compose::WriteTarget::outermost:
        return "outermost";
    case compose::WriteTarget::defining_template:
        return "template";
    case compose::WriteTarget::at_instance:
        return "at-instance";
    }
    return "outermost";
}

} // namespace

Envelope run_set(const std::vector<std::string>& positionals,
                 const std::map<std::string, std::string>& flags)
{
    if (positionals.size() < 2)
        return Envelope::failure("usage.missing_argument",
                                 "usage: context set <scene> <value> --pointer <jsonPointer> "
                                 "--id-path <a/b/c> [--edit-template | --at-instance <a/b>]");
    const std::string& scene = positionals[0];
    const std::string& value_text = positionals[1];

    const std::optional<std::string> pointer = flag(flags, "pointer");
    if (!pointer || pointer->empty())
        return Envelope::failure("usage.missing_argument",
                                 "context set requires --pointer <jsonPointer> (the field to write "
                                 "inside the addressed entity)");
    const std::optional<std::string> id_path_text = flag(flags, "id-path");
    if (!id_path_text)
        return Envelope::failure("usage.missing_argument",
                                 "context set requires --id-path <a/b/c> (the L-35 id-path to the "
                                 "composed entity)");

    const bool edit_template = flags.find("edit-template") != flags.end();
    const std::optional<std::string> at_instance_text = flag(flags, "at-instance");
    if (edit_template && at_instance_text)
        return Envelope::failure("usage.invalid",
                                 "--edit-template and --at-instance are mutually exclusive (the "
                                 "first writes the defining template, the second a mid-level "
                                 "override)");

    std::vector<std::string> id_path;
    if (!split_id_path(*id_path_text, id_path))
        return Envelope::failure("usage.invalid",
                                 "--id-path segments are slash-separated stable ids (no empty "
                                 "segments): got `" + *id_path_text + "`");

    compose::WriteRequest request;
    request.root_scene = scene;
    request.id_path = std::move(id_path);
    request.pointer = *pointer;
    if (at_instance_text)
    {
        if (!split_id_path(*at_instance_text, request.at_instance))
            return Envelope::failure("usage.invalid",
                                     "--at-instance segments are slash-separated stable ids (no "
                                     "empty segments): got `" + *at_instance_text + "`");
        request.target = compose::WriteTarget::at_instance;
    }
    else if (edit_template)
    {
        request.target = compose::WriteTarget::defining_template;
    }
    else
    {
        request.target = compose::WriteTarget::outermost;
    }

    // Parse the <value> as a JSON document (a scalar / array / object literal).
    serializer::ParseResult parsed = serializer::parse_json(value_text);
    if (!parsed.ok)
    {
        std::string detail = parsed.diagnostics.empty() ? std::string("malformed JSON")
                                                        : parsed.diagnostics.front().code + ": " +
                                                              parsed.diagnostics.front().message;
        return Envelope::failure("file.parse_error",
                                 "the <value> argument is not valid JSON (" + detail +
                                     ") — pass a JSON literal, e.g. '[1, 2, 3]' or '\"text\"'");
    }
    request.value = std::move(parsed.root);

    if (!filesync::is_inside_jail(".", scene))
        return Envelope::failure("path.jail_violation",
                                 "the scene path `" + scene + "` escapes the project root (R-SEC-008)");

    const std::string project = flag(flags, "project").value_or(".");
    ProjectResolver resolver(project);

    const compose::WritePlan plan = compose::plan_write(request, resolver);
    if (!plan.ok)
        return Envelope::failure(plan.error_code, plan.error_message, plan.error_pointer);

    std::string new_bytes;
    if (!serializer::serialize_canonical(plan.document, new_bytes))
        return Envelope::failure("internal.error",
                                 "the mutated scene document could not be canonically serialized");

    const std::uint64_t raw_hash = filesync::content_hash(new_bytes);
    const std::uint64_t canonical_hash = serializer::canonical_hash_of(new_bytes);

    const bool dry_run = flags.find("dry-run") != flags.end();
    const fs::path target_abs = fs::path(project) / filesync::normalize_path(plan.file);

    // --if-match CAS: guard the target file's CURRENT raw bytes (R-FILE-004 / R-CLI-006 — the raw-byte
    // hash guards the exact bytes on disk).
    if (const std::optional<std::string> if_match = flag(flags, "if-match"))
    {
        std::uint64_t expected = 0;
        if (!parse_u64(*if_match, expected))
            return Envelope::failure("usage.invalid",
                                     "--if-match takes a decimal raw-byte content hash; got `" +
                                         *if_match + "`");
        std::string current;
        const bool present = read_file(target_abs, current);
        const std::uint64_t actual = filesync::content_hash(current);
        if (!present || actual != expected)
            return Envelope::failure(
                "cas.mismatch",
                "the --if-match raw-byte hash (" + std::to_string(expected) +
                    ") does not match the target file's current bytes (" +
                    (present ? std::to_string(actual) : std::string("<file absent>")) +
                    ") — re-read and retry");
    }

    Json data = Json::object();
    data.set("file", Json(plan.file));
    data.set("pointer", Json(plan.pointer));
    data.set("target", Json(std::string(target_name(plan.target))));
    data.set("baseRecorded", Json(plan.base_recorded));
    // 64-bit hashes as decimal STRINGS: a full-range hash exceeds 2^53 and would lose precision as a
    // JSON number (R-CLI-006 barrier keys must round-trip losslessly) — mirrors editor smoke output.
    data.set("rawHash", Json(std::to_string(raw_hash)));
    data.set("canonicalHash", Json(std::to_string(canonical_hash)));
    data.set("dryRun", Json(dry_run));

    if (dry_run)
    {
        data.set("applied", Json(false));
        data.set("note", Json(std::string("dry-run: the write target + resulting hashes are "
                                          "computed; no bytes were written")));
        return Envelope::success(std::move(data));
    }

    filesync::NativeFileStore store(project);
    if (!filesync::atomic_write(store, plan.file, new_bytes))
        return Envelope::failure("internal.error",
                                 "the atomic write to `" + plan.file +
                                     "` failed (path jail refusal or an IO error)");
    data.set("applied", Json(true));
    return Envelope::success(std::move(data));
}

Envelope run_override_query(const std::vector<std::string>& positionals,
                            const std::map<std::string, std::string>& flags)
{
    const std::string mode = flag(flags, "overrides").value_or("");
    compose::HygieneKind kind;
    if (mode == "diverged")
        kind = compose::HygieneKind::diverged;
    else if (mode == "redundant")
        kind = compose::HygieneKind::redundant;
    else
        return Envelope::failure("usage.invalid",
                                 "usage: context query --overrides <diverged|redundant> <scene> — "
                                 "got --overrides `" + mode + "`");

    if (positionals.empty())
        return Envelope::failure("usage.missing_argument",
                                 "context query --overrides requires a <scene> to inspect");
    const std::string& scene = positionals[0];
    if (!filesync::is_inside_jail(".", scene))
        return Envelope::failure("path.jail_violation",
                                 "the scene path `" + scene + "` escapes the project root (R-SEC-008)");

    const std::string project = flag(flags, "project").value_or(".");
    ProjectResolver resolver(project);
    if (resolver.resolve(scene) == nullptr)
        return Envelope::failure("file.not_found",
                                 "the scene `" + scene +
                                     "` does not resolve to a known scene under project `" +
                                     project + "`");

    const std::vector<compose::OverrideFinding> findings =
        compose::override_hygiene(scene, resolver, kind);

    Json entries = Json::array();
    for (const compose::OverrideFinding& f : findings)
    {
        Json entry = Json::object();
        entry.set("file", Json(f.file));
        entry.set("entryPointer", Json(f.entry_pointer));
        Json path = Json::array();
        for (const std::string& seg : f.path)
            path.push_back(Json(seg));
        entry.set("path", std::move(path));
        entry.set("fieldPointer", Json(f.field_pointer));
        entry.set("reason", Json(f.reason));
        entries.push_back(std::move(entry));
    }

    Json data = Json::object();
    data.set("rootScene", Json(scene));
    data.set("mode", Json(mode));
    data.set("advisory", Json(true)); // R-CLI-006: hygiene findings are never auto-pruned
    data.set("count", Json(static_cast<std::uint64_t>(findings.size())));
    data.set("overrides", std::move(entries));
    return Envelope::success(std::move(data));
}

} // namespace context::cli

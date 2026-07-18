// `context set` (composed write) + `context query --overrides` (advisory hygiene) — see set_command.h.

#include "context/cli/set_command.h"

#include "context/cli/wire_client.h"
#include "context/editor/compose/compose_write.h"
#include "context/editor/compose/project_resolver.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/native_file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <cstdint>
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

namespace
{

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

[[nodiscard]] std::optional<std::string> flag(const std::map<std::string, std::string>& flags,
                                              const std::string& name)
{
    const auto it = flags.find(name);
    if (it == flags.end())
        return std::nullopt;
    return it->second;
}

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
    compose::ProjectSceneResolver resolver(project);

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
    filesync::NativeFileStore store(project);

    // --if-match CAS: guard the target file's CURRENT raw bytes (R-FILE-004 / R-CLI-006 — the raw-byte
    // hash guards the exact bytes on disk).
    if (const std::optional<std::string> if_match = flag(flags, "if-match"))
    {
        const std::optional<std::uint64_t> expected = parse_u64(*if_match);
        if (!expected)
            return Envelope::failure("usage.invalid",
                                     "--if-match takes a decimal raw-byte content hash; got `" +
                                         *if_match + "`");
        const std::optional<std::string> current = store.read(plan.file);
        const std::uint64_t actual = current ? filesync::content_hash(*current) : 0;
        if (!current || actual != *expected)
            return Envelope::failure(
                "cas.mismatch",
                "the --if-match raw-byte hash (" + std::to_string(*expected) +
                    ") does not match the target file's current bytes (" +
                    (current ? std::to_string(actual) : std::string("<file absent>")) +
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
    compose::ProjectSceneResolver resolver(project);
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

// <asset>.meta.json parsing + fresh-sidecar serialization (see meta.h).

#include "context/editor/assetdb/meta.h"

#include "context/editor/assetdb/guid.h"
#include "context/editor/schema/json_access.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

namespace context::editor::assetdb
{

using serializer::JsonMember;
using serializer::JsonValue;

std::string meta_path_for(std::string_view asset_path)
{
    std::string out(asset_path);
    out += kMetaSuffix;
    return out;
}

bool is_meta_path(std::string_view path) noexcept
{
    return path.size() > kMetaSuffix.size() &&
           path.substr(path.size() - kMetaSuffix.size()) == kMetaSuffix;
}

std::string asset_path_for(std::string_view meta_path)
{
    return std::string(meta_path.substr(0, meta_path.size() - kMetaSuffix.size()));
}

std::optional<AssetMeta> parse_meta(std::string_view bytes, std::vector<std::string>& problems)
{
    // parse_json, not canonicalize: only the tree is needed, and parse_meta runs per sidecar on
    // every scan (the R-FILE-011 100k-file hot path) — canonicalize would re-serialize + hash for
    // nothing. Tolerance is the point: hand-edited / newer-engine sidecars need not be canonical.
    const serializer::ParseResult parsed = serializer::parse_json(bytes);
    if (!parsed.ok)
    {
        problems.emplace_back("meta sidecar is not well-formed JSON");
        return std::nullopt;
    }
    if (parsed.root.type != JsonValue::Type::object)
    {
        problems.emplace_back("meta sidecar root must be an object");
        return std::nullopt;
    }

    const JsonValue* guid = schema::find_member(parsed.root, "guid");
    if (guid == nullptr || guid->type != JsonValue::Type::string ||
        !is_guid(guid->string_value))
    {
        problems.emplace_back("meta sidecar carries no valid \"guid\" (32 lowercase hex chars)");
        return std::nullopt;
    }

    AssetMeta meta;
    meta.guid = guid->string_value;

    if (const JsonValue* kind = schema::find_member(parsed.root, "kind"); kind != nullptr)
    {
        if (kind->type == JsonValue::Type::string)
            meta.kind = kind->string_value;
        else
            problems.emplace_back("meta \"kind\" is not a string; treated as unknown");
    }

    // Header oddities are notes, not identity failures: identity must survive a sidecar written by
    // a newer engine (extra members) or a hand-edit that dropped the header (R-FILE-003 honesty).
    const JsonValue* schema_id = schema::find_member(parsed.root, "$schema");
    if (schema_id == nullptr || schema_id->type != JsonValue::Type::string ||
        schema_id->string_value != kMetaKindId)
        problems.emplace_back("meta \"$schema\" is not \"" + std::string(kMetaKindId) + "\"");

    return meta;
}

std::string serialize_meta(const AssetMeta& meta)
{
    JsonValue root;
    root.type = JsonValue::Type::object;

    auto add_string = [&root](std::string_view key, std::string_view value)
    {
        JsonMember m;
        m.key = std::string(key);
        m.value.type = JsonValue::Type::string;
        m.value.string_value = std::string(value);
        root.members.push_back(std::move(m));
    };
    auto add_object = [&root](std::string_view key)
    {
        JsonMember m;
        m.key = std::string(key);
        m.value.type = JsonValue::Type::object;
        root.members.push_back(std::move(m));
    };

    add_string("$schema", kMetaKindId);
    JsonMember version;
    version.key = "version";
    version.value.type = JsonValue::Type::integer;
    version.value.int_value = kMetaSchemaVersion;
    root.members.push_back(std::move(version));

    add_string("guid", meta.guid);
    if (!meta.kind.empty())
        add_string("kind", meta.kind);
    add_object("importSettings"); // import settings live beside the asset (L-36)
    add_object("platforms");      // RESERVED: per-platform import-setting overrides (M2 meta schema)

    std::string out;
    const bool ok = serializer::serialize_canonical(root, out);
    (void)ok; // the tree above contains no numbers beyond an integer literal — always serializable
    return out;
}

} // namespace context::editor::assetdb

// Player save document serialize/parse (see save_document.h).

#include "context/runtime/save/save_document.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <string>
#include <utility>

namespace context::runtime::save
{

using serializer::Diagnostic;
using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

[[nodiscard]] const JsonValue* find_member(const JsonValue& object, std::string_view key)
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

[[nodiscard]] bool is_integer(const JsonValue& v) noexcept
{
    return v.type == JsonValue::Type::integer || v.type == JsonValue::Type::unsigned_integer;
}

[[nodiscard]] std::int64_t as_integer(const JsonValue& v) noexcept
{
    return v.type == JsonValue::Type::integer ? v.int_value
                                              : static_cast<std::int64_t>(v.uint_value);
}

void push_diag(std::vector<Diagnostic>& diagnostics, std::string code, std::string message)
{
    Diagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    // Byte position is the caller's concern (the derivation locate layer resolves it); a save is
    // machine-written, so a shape finding points at the whole document.
    d.line = 1;
    d.column = 1;
    diagnostics.push_back(std::move(d));
}

// Canonical-JSON emitter helpers (mirrors the sibling modules; the writer sorts object keys).
[[nodiscard]] JsonValue json_string(std::string_view v)
{
    JsonValue out;
    out.type = JsonValue::Type::string;
    out.string_value = std::string(v);
    return out;
}

[[nodiscard]] JsonValue json_int(std::int64_t v)
{
    JsonValue out;
    out.type = JsonValue::Type::integer;
    out.int_value = v;
    return out;
}

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    JsonMember m;
    m.key = std::string(key);
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}

} // namespace

const std::int64_t* SaveDocument::saved_version(std::string_view type) const
{
    for (const auto& [t, v] : component_versions)
        if (t == type)
            return &v;
    return nullptr;
}

std::string format_identity(std::uint64_t identity)
{
    static const char* const kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        out[static_cast<std::size_t>(i)] = kHex[identity & 0xFU];
        identity >>= 4;
    }
    return out;
}

bool parse_identity(std::string_view text, std::uint64_t& out) noexcept
{
    if (text.size() != 16)
        return false;
    std::uint64_t value = 0;
    for (const char c : text)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= static_cast<std::uint64_t>(c - 'a' + 10);
        else
            return false; // canonical form is lowercase hex only
    }
    out = value;
    return true;
}

std::string serialize_save(const SaveDocument& save)
{
    JsonValue root;
    root.type = JsonValue::Type::object;
    set_member(root, "$save", json_string(kSaveKind));
    set_member(root, "saveFormatVersion", json_int(save.format_version));
    set_member(root, "engineVersion", json_string(save.engine_version));
    set_member(root, "backCompatScope", json_int(save.back_compat_scope));

    JsonValue versions;
    versions.type = JsonValue::Type::object;
    for (const auto& [type, version] : save.component_versions)
        set_member(versions, type, json_int(version));
    set_member(root, "componentVersions", std::move(versions));

    JsonValue entities;
    entities.type = JsonValue::Type::array;
    for (const SaveEntity& entity : save.entities)
    {
        JsonValue obj;
        obj.type = JsonValue::Type::object;
        set_member(obj, "identity", json_string(format_identity(entity.identity)));
        set_member(obj, "components", entity.components);
        entities.elements.push_back(std::move(obj));
    }
    set_member(root, "entities", std::move(entities));

    std::string out;
    // A save is built from valid-UTF-8 strings and integers; the only unrepresentable input would be
    // a non-finite double inside a component payload, which a runtime writer never produces. Fall
    // back to empty on the impossible case rather than emit half a document.
    if (!serializer::serialize_canonical(root, out))
        out.clear();
    return out;
}

SaveParseResult parse_save(std::string_view bytes)
{
    SaveParseResult result;

    serializer::ParseResult parsed = serializer::parse_json(bytes);
    // Carry the parser's findings through in BOTH directions: the fatal json.* finding on failure,
    // or the non-fatal encoding.bom/encoding.crlf findings the parser still reports on a SUCCESSFUL
    // parse (json_parse.h). The SaveParseResult contract exposes the json.* family, so dropping them
    // on the ok path would silently hide a BOM/CRLF a hand-edited save carried.
    result.diagnostics = std::move(parsed.diagnostics);
    if (!parsed.ok)
        return result;

    const JsonValue& root = parsed.root;
    if (root.type != JsonValue::Type::object)
    {
        push_diag(result.diagnostics, "save.malformed", "the save root is not a JSON object");
        return result;
    }

    const JsonValue* marker = find_member(root, "$save");
    if (marker == nullptr || marker->type != JsonValue::Type::string ||
        marker->string_value != kSaveKind)
    {
        push_diag(result.diagnostics, "save.malformed",
                  "not a save envelope (missing or wrong \"$save\" marker)");
        return result;
    }

    const JsonValue* format_version = find_member(root, "saveFormatVersion");
    if (format_version == nullptr || !is_integer(*format_version))
    {
        push_diag(result.diagnostics, "save.malformed",
                  "missing or non-integer \"saveFormatVersion\"");
        return result;
    }
    result.save.format_version = as_integer(*format_version);
    if (result.save.format_version < 1)
    {
        push_diag(result.diagnostics, "save.malformed",
                  "\"saveFormatVersion\" is " + std::to_string(result.save.format_version) +
                      " but save envelope versions start at 1");
        return result;
    }
    if (result.save.format_version > kSaveFormatVersion)
    {
        push_diag(result.diagnostics, "save.format_unsupported",
                  "the save envelope is version " + std::to_string(result.save.format_version) +
                      " but this build reads up to version " + std::to_string(kSaveFormatVersion));
        return result;
    }

    if (const JsonValue* engine = find_member(root, "engineVersion"); engine != nullptr)
    {
        if (engine->type != JsonValue::Type::string)
        {
            push_diag(result.diagnostics, "save.malformed", "\"engineVersion\" is not a string");
            return result;
        }
        result.save.engine_version = engine->string_value;
    }

    if (const JsonValue* scope = find_member(root, "backCompatScope"); scope != nullptr)
    {
        if (!is_integer(*scope))
        {
            push_diag(result.diagnostics, "save.malformed", "\"backCompatScope\" is not an integer");
            return result;
        }
        result.save.back_compat_scope = as_integer(*scope);
    }

    if (const JsonValue* versions = find_member(root, "componentVersions"); versions != nullptr)
    {
        if (versions->type != JsonValue::Type::object)
        {
            push_diag(result.diagnostics, "save.malformed", "\"componentVersions\" is not an object");
            return result;
        }
        for (const JsonMember& m : versions->members)
        {
            if (!is_integer(m.value))
            {
                push_diag(result.diagnostics, "save.malformed",
                          "componentVersions[\"" + m.key + "\"] is not an integer");
                return result;
            }
            const std::int64_t version = as_integer(m.value);
            // Mirror the saveFormatVersion floor: a per-component schemaVersion also starts at 1. A
            // 0/negative stamp with no live payload of that type would otherwise never reach
            // migrate_payload's floor check and get silently re-stamped to the current version.
            if (version < 1)
            {
                push_diag(result.diagnostics, "save.malformed",
                          "componentVersions[\"" + m.key + "\"] is " + std::to_string(version) +
                              " but component schema versions start at 1");
                return result;
            }
            result.save.component_versions.emplace_back(m.key, version);
        }
    }

    const JsonValue* entities = find_member(root, "entities");
    if (entities == nullptr || entities->type != JsonValue::Type::array)
    {
        push_diag(result.diagnostics, "save.malformed", "missing or non-array \"entities\"");
        return result;
    }
    for (const JsonValue& element : entities->elements)
    {
        if (element.type != JsonValue::Type::object)
        {
            push_diag(result.diagnostics, "save.malformed", "an entities entry is not an object");
            return result;
        }
        const JsonValue* identity = find_member(element, "identity");
        if (identity == nullptr || identity->type != JsonValue::Type::string)
        {
            push_diag(result.diagnostics, "save.malformed",
                      "an entity is missing a string \"identity\"");
            return result;
        }
        SaveEntity entity;
        if (!parse_identity(identity->string_value, entity.identity))
        {
            push_diag(result.diagnostics, "save.malformed",
                      "entity identity \"" + identity->string_value +
                          "\" is not a 16-char lowercase-hex composed identity");
            return result;
        }
        const JsonValue* components = find_member(element, "components");
        if (components == nullptr || components->type != JsonValue::Type::object)
        {
            push_diag(result.diagnostics, "save.malformed",
                      "an entity is missing an object \"components\" map");
            return result;
        }
        // Every component payload must itself be an object. The migration runner and the editor's
        // parse-time payload discovery (migrate::find_sites) both treat ONLY object values as real
        // payload sites, so a scalar/array payload would silently no-op through migration and still
        // be re-stamped to the current version — surface it as malformed here (never best-effort).
        for (const JsonMember& component : components->members)
        {
            if (component.value.type != JsonValue::Type::object)
            {
                push_diag(result.diagnostics, "save.malformed",
                          "component \"" + component.key + "\" is not an object payload");
                return result;
            }
        }
        entity.components = *components;
        result.save.entities.push_back(std::move(entity));
    }

    result.ok = true;
    return result;
}

} // namespace context::runtime::save

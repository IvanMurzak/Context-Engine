// Dual-form reference checking + healing (see ref_heal.h).

#include "context/editor/assetdb/ref_heal.h"

#include "context/editor/schema/json_access.h"
#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"

#include <string>
#include <utility>

namespace context::editor::assetdb
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// JSON-pointer token escaping (RFC 6901): "~" -> "~0", "/" -> "~1".
[[nodiscard]] std::string escape_pointer_token(std::string_view token)
{
    std::string out;
    out.reserve(token.size());
    for (const char c : token)
    {
        if (c == '~')
            out += "~0";
        else if (c == '/')
            out += "~1";
        else
            out.push_back(c);
    }
    return out;
}

// Walk `value` against schema `node`, invoking `visit(ref_value, declared_kind, pointer)` on every
// x-ctx-ref field's value. Mirrors the validator's binding walk over the same schema dialect
// (properties / items / x-ctx-union) but visits ONLY reference fields — validation itself stays the
// schema module's job. Templated on constness so the checking and healing passes share one walk.
template <typename V, typename Visit>
void walk_refs(V& value, const JsonValue& node, const std::string& pointer, const Visit& visit)
{
    if (const JsonValue* ref_kind = schema::find_member(node, schema::kKeyRef);
        ref_kind != nullptr && ref_kind->type == JsonValue::Type::string)
    {
        visit(value, ref_kind->string_value, pointer);
        return; // a reference field's value IS the reference object — nothing nests deeper
    }
    if (const JsonValue* union_spec = schema::find_member(node, schema::kKeyUnion);
        union_spec != nullptr && value.type == JsonValue::Type::object)
    {
        const JsonValue* tag = schema::find_member(value, "type");
        if (tag != nullptr && tag->type == JsonValue::Type::string)
            if (const JsonValue* variant = schema::find_member(*union_spec, tag->string_value);
                variant != nullptr)
                walk_refs(value, *variant, pointer, visit);
        return;
    }
    if (value.type == JsonValue::Type::object)
    {
        if (const JsonValue* props = schema::find_member(node, "properties"); props != nullptr)
            for (auto& member : value.members)
                if (const JsonValue* sub = schema::find_member(*props, member.key); sub != nullptr)
                    walk_refs(member.value, *sub,
                              pointer + "/" + escape_pointer_token(member.key), visit);
        return;
    }
    if (value.type == JsonValue::Type::array)
        if (const JsonValue* items = schema::find_member(node, "items"); items != nullptr)
            for (std::size_t i = 0; i < value.elements.size(); ++i)
                walk_refs(value.elements[i], *items, pointer + "/" + std::to_string(i), visit);
}

// Select the kind schema the document binds to (its $schema/version header; the latest registered
// version when the exact one is absent). nullptr for unbound/unregistered documents — reference
// work is schema-driven, so those are a no-op here (the validator reports unknown kinds).
[[nodiscard]] const schema::KindSchema* schema_for(const JsonValue& root,
                                                   const schema::SchemaSet& schemas)
{
    std::vector<serializer::Diagnostic> diags;
    const serializer::DocumentHeader header = serializer::read_document_header(root, diags);
    if (!header.has_schema)
        return nullptr;
    if (header.has_version)
        if (const schema::KindSchema* exact = schemas.find(header.schema, header.version);
            exact != nullptr)
            return exact;
    return schemas.latest(header.schema);
}

// The shape triage of one x-ctx-ref field value. Malformed shapes are the validator's findings —
// this pass touches only the well-understood cross-file forms.
struct RefShape
{
    const JsonValue* guid = nullptr;   // "$ref" member (string)
    const JsonValue* path = nullptr;   // "path" member (string)
    bool entity = false;               // {"$entity": ...} — same-file, out of scope here
    bool cross_file_like = false;      // an object shaped like a cross-file reference
};

[[nodiscard]] RefShape triage(const JsonValue& value)
{
    RefShape shape;
    if (value.type != JsonValue::Type::object)
        return shape;
    if (schema::find_member(value, schema::kRefEntityField) != nullptr)
    {
        shape.entity = true;
        return shape;
    }
    const JsonValue* guid = schema::find_member(value, schema::kRefGuidField);
    const JsonValue* path = schema::find_member(value, schema::kRefPathField);
    if (guid != nullptr && guid->type == JsonValue::Type::string && !guid->string_value.empty())
        shape.guid = guid;
    if (path != nullptr && path->type == JsonValue::Type::string && !path->string_value.empty())
        shape.path = path;
    shape.cross_file_like = shape.guid != nullptr || shape.path != nullptr;
    return shape;
}

void set_member(JsonValue& object, std::string_view key, std::string_view value)
{
    for (JsonMember& m : object.members)
        if (m.key == key)
        {
            m.value.type = JsonValue::Type::string;
            m.value.string_value = std::string(value);
            return;
        }
    JsonMember m;
    m.key = std::string(key);
    m.value.type = JsonValue::Type::string;
    m.value.string_value = std::string(value);
    object.members.push_back(std::move(m)); // the canonical writer sorts keys on serialization
}

} // namespace

std::vector<RefFinding> check_document_refs(const JsonValue& root,
                                            const schema::SchemaSet& schemas,
                                            const AssetDatabase& db)
{
    std::vector<RefFinding> findings;
    const schema::KindSchema* kind = schema_for(root, schemas);
    if (kind == nullptr)
        return findings;

    walk_refs(root, kind->doc, "",
              [&](const JsonValue& value, const std::string& /*declared_kind*/,
                  const std::string& pointer)
              {
                  const RefShape shape = triage(value);
                  if (shape.entity || !shape.cross_file_like)
                      return;
                  if (shape.guid == nullptr)
                  {
                      // Path-only: ACCEPTED (L-34) — awaiting resolution by --fix / tool save.
                      if (db.find_by_path(shape.path->string_value) != nullptr)
                          findings.push_back({"asset.ref_path_only", pointer,
                                              "path-only reference; `context validate --fix` or "
                                              "the next tool save resolves the $ref GUID"});
                      else
                          findings.push_back({"asset.ref_dangling", pointer,
                                              "path-only reference resolves to no indexed asset: " +
                                                  shape.path->string_value});
                      return;
                  }
                  const AssetRecord* record = db.find_by_guid(shape.guid->string_value);
                  if (record == nullptr)
                  {
                      findings.push_back({"asset.ref_dangling", pointer,
                                          "$ref GUID resolves to no indexed asset: " +
                                              shape.guid->string_value});
                      return;
                  }
                  if (shape.path != nullptr && shape.path->string_value != record->path)
                      findings.push_back({"asset.ref_hint_stale", pointer,
                                          "path hint \"" + shape.path->string_value +
                                              "\" is stale; the asset lives at \"" +
                                              record->path + "\""});
              });
    return findings;
}

RefHealResult heal_document_refs(JsonValue& root, const schema::SchemaSet& schemas,
                                 const AssetDatabase& db)
{
    RefHealResult result;
    const schema::KindSchema* kind = schema_for(root, schemas);
    if (kind == nullptr)
        return result;

    walk_refs(root, kind->doc, "",
              [&](JsonValue& value, const std::string& /*declared_kind*/,
                  const std::string& pointer)
              {
                  const RefShape shape = triage(value);
                  if (shape.entity || !shape.cross_file_like)
                      return;
                  if (shape.guid == nullptr)
                  {
                      // Resolve a path-only reference to the dual form (L-34: the GUID becomes
                      // authoritative; the path stays as the now-correct hint).
                      const AssetRecord* record = db.find_by_path(shape.path->string_value);
                      if (record == nullptr)
                      {
                          result.findings.push_back(
                              {"asset.ref_dangling", pointer,
                               "path-only reference resolves to no indexed asset: " +
                                   shape.path->string_value});
                          return;
                      }
                      set_member(value, schema::kRefGuidField, record->guid);
                      result.actions.push_back(
                          {"guid-resolved", pointer, record->guid, record->path});
                      return;
                  }
                  const AssetRecord* record = db.find_by_guid(shape.guid->string_value);
                  if (record == nullptr)
                  {
                      result.findings.push_back({"asset.ref_dangling", pointer,
                                                 "$ref GUID resolves to no indexed asset: " +
                                                     shape.guid->string_value});
                      return;
                  }
                  if (shape.path == nullptr)
                  {
                      set_member(value, schema::kRefPathField, record->path);
                      result.actions.push_back(
                          {"hint-added", pointer, record->guid, record->path});
                  }
                  else if (shape.path->string_value != record->path)
                  {
                      set_member(value, schema::kRefPathField, record->path);
                      result.actions.push_back(
                          {"hint-updated", pointer, record->guid, record->path});
                  }
              });
    return result;
}

bool is_entity_ref(const JsonValue& value) noexcept
{
    if (value.type != JsonValue::Type::object || value.members.size() != 1)
        return false;
    const JsonValue* entity = schema::find_member(value, schema::kRefEntityField);
    if (entity == nullptr)
        return false;
    if (entity->type == JsonValue::Type::string)
        return !entity->string_value.empty();
    return is_entity_id_path(*entity);
}

bool is_entity_id_path(const JsonValue& value) noexcept
{
    // The id-path form implies at least one instance hop: [instanceId, ..., entityId], >= 2
    // non-empty strings (a lone [entityId] is the plain string form's job).
    if (value.type != JsonValue::Type::array || value.elements.size() < 2)
        return false;
    for (const JsonValue& element : value.elements)
        if (element.type != JsonValue::Type::string || element.string_value.empty())
            return false;
    return true;
}

} // namespace context::editor::assetdb

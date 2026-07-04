// Kind-schema compilation (the vocabulary-law gate), the versioned registration set, the engine
// kinds, and the introspection projection — see kind_schema.h.

#include "context/editor/schema/kind_schema.h"

#include "context/editor/schema/json_access.h"
#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::schema
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

constexpr std::array<std::string_view, 6> kTypeNames = {"object", "array",   "string",
                                                        "number", "integer", "boolean"};

// The pinned dialect keywords a schema node may carry (kind_schema.h). Anything else is rejected
// so the dialect cannot drift per package.
constexpr std::array<std::string_view, 12> kSchemaKeywords = {
    "$id",   "version",     "type", "properties", "required",    "additionalProperties",
    "items", "description", "enum", "x-ctx-type", "x-ctx-units", "x-ctx-storage"};

[[nodiscard]] bool is_schema_keyword(std::string_view key) noexcept
{
    return std::find(kSchemaKeywords.begin(), kSchemaKeywords.end(), key) !=
               kSchemaKeywords.end() ||
           key == kKeyRef || key == kKeyUnion;
}

void add_problem(std::vector<std::string>& problems, const std::string& pointer,
                 std::string_view message)
{
    problems.push_back((pointer.empty() ? std::string("<root>") : pointer) + ": " +
                       std::string(message));
}

// Recursively law-check one schema node. `pointer` addresses the node inside the schema DOCUMENT.
void check_schema_node(const JsonValue& node, const std::string& pointer, bool is_root,
                       std::vector<std::string>& problems)
{
    if (node.type != JsonValue::Type::object)
    {
        add_problem(problems, pointer, "a schema node must be a JSON object");
        return;
    }

    for (const JsonMember& m : node.members)
        if (!is_schema_keyword(m.key))
            add_problem(problems, pointer + "/" + m.key, "unknown schema keyword");

    const JsonValue* type = find_member(node, "type");
    if (type != nullptr &&
        (type->type != JsonValue::Type::string ||
         std::find(kTypeNames.begin(), kTypeNames.end(), type->string_value) == kTypeNames.end()))
        add_problem(problems, pointer + "/type",
                    "type must be one of object/array/string/number/integer/boolean");

    if (const JsonValue* description = find_member(node, "description");
        description != nullptr && description->type != JsonValue::Type::string)
        add_problem(problems, pointer + "/description", "description must be a string");

    if (const JsonValue* additional = find_member(node, "additionalProperties");
        additional != nullptr && additional->type != JsonValue::Type::boolean)
        add_problem(problems, pointer + "/additionalProperties",
                    "additionalProperties must be a boolean");

    if (const JsonValue* enumeration = find_member(node, "enum"); enumeration != nullptr)
    {
        const bool strings = enumeration->type == JsonValue::Type::array &&
                             !enumeration->elements.empty() &&
                             std::all_of(enumeration->elements.begin(),
                                         enumeration->elements.end(), [](const JsonValue& e) {
                                             return e.type == JsonValue::Type::string;
                                         });
        if (!strings)
            add_problem(problems, pointer + "/enum", "enum must be a non-empty array of strings");
    }

    // --- the vocabulary LAW (R-DATA-006) --------------------------------------------------------
    const JsonValue* semantic = find_member(node, kKeySemanticType);
    if (semantic != nullptr && (semantic->type != JsonValue::Type::string ||
                                !is_semantic_type_id(semantic->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeySemanticType),
                    "unknown engine semantic type (pinned: quaternion, color, curve, gradient, "
                    "bit-flags)");

    const JsonValue* units = find_member(node, kKeyUnits);
    if (units != nullptr &&
        (units->type != JsonValue::Type::string || !is_si_unit(units->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeyUnits),
                    "the units law is SI + radians everywhere — non-SI unit declarations are "
                    "rejected (R-DATA-006)");

    if (const JsonValue* storage = find_member(node, kKeyStorage);
        storage != nullptr &&
        (storage->type != JsonValue::Type::string || !is_storage_layout(storage->string_value)))
        add_problem(problems, pointer + "/" + std::string(kKeyStorage),
                    "malformed storage layout (grammar: <base> or <base>x<lanes>, base in "
                    "f32/f64/i8..i64/u8..u64, lanes in 2/3/4/9/16)");

    const JsonValue* ref = find_member(node, kKeyRef);
    if (ref != nullptr && (ref->type != JsonValue::Type::string || ref->string_value.empty()))
        add_problem(problems, pointer + "/" + std::string(kKeyRef),
                    "x-ctx-ref must name the required target kind");

    const JsonValue* union_spec = find_member(node, kKeyUnion);
    if (union_spec != nullptr)
    {
        if (union_spec->type != JsonValue::Type::object || union_spec->members.empty())
            add_problem(problems, pointer + "/" + std::string(kKeyUnion),
                        "x-ctx-union must be a non-empty object of tag -> variant schema");
        else
            for (const JsonMember& variant : union_spec->members)
            {
                const std::string variant_pointer =
                    pointer + "/" + std::string(kKeyUnion) + "/" + variant.key;
                if (!is_union_tag(variant.key))
                    add_problem(problems, variant_pointer,
                                "union tags follow the ONE pinned convention \"<ns>:<shape>\" "
                                "(R-DATA-006 — never ad-hoc encodings)");
                check_schema_node(variant.value, variant_pointer, /*is_root=*/false, problems);
            }
    }

    // A field is a ref, a union, or a semantic value — never two at once.
    if ((semantic != nullptr) + (ref != nullptr) + (union_spec != nullptr) > 1)
        add_problem(problems, pointer,
                    "x-ctx-type, x-ctx-ref, and x-ctx-union are mutually exclusive on one field");

    const JsonValue* properties = find_member(node, "properties");
    if (properties != nullptr)
    {
        if (properties->type != JsonValue::Type::object)
            add_problem(problems, pointer + "/properties", "properties must be an object");
        else
            for (const JsonMember& property : properties->members)
                check_schema_node(property.value, pointer + "/properties/" + property.key,
                                  /*is_root=*/false, problems);
    }

    if (const JsonValue* required = find_member(node, "required"); required != nullptr)
    {
        if (required->type != JsonValue::Type::array)
            add_problem(problems, pointer + "/required", "required must be an array of strings");
        else
            for (const JsonValue& name : required->elements)
                if (name.type != JsonValue::Type::string || properties == nullptr ||
                    find_member(*properties, name.string_value) == nullptr)
                    add_problem(problems, pointer + "/required",
                                "every required name must be a declared property");
    }

    if (const JsonValue* items = find_member(node, "items"); items != nullptr)
        check_schema_node(*items, pointer + "/items", /*is_root=*/false, problems);

    if (is_root)
    {
        const JsonValue* id = find_member(node, "$id");
        if (id == nullptr || id->type != JsonValue::Type::string || id->string_value.empty())
            add_problem(problems, pointer + "/$id", "the root must carry the kind id ($id)");
        const JsonValue* version = find_member(node, "version");
        if (version == nullptr || version->type != JsonValue::Type::integer ||
            version->int_value < 1)
            add_problem(problems, pointer + "/version",
                        "the root must carry an integer schema version >= 1");
        if (type == nullptr || type->string_value != "object")
            add_problem(problems, pointer + "/type", "an authored kind's root type is object");
        if (properties == nullptr || find_member(*properties, kNotesField) == nullptr)
            add_problem(problems, pointer + "/properties",
                        "every authored kind exposes the schema-blessed `notes` field (L-32)");
    }
}

// --- the engine kind schemas (the M1 scene placeholder, migrated onto this mechanism) -----------

constexpr std::string_view kSceneSchemaJson = R"({
  "$id": "ctx:scene",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["entities"],
  "description": "An authored scene (L-32 canonical JSON; L-33 one scene per file).",
  "properties": {
    "kind": {"type": "string", "enum": ["scene"], "description": "The M1 placeholder kind marker; the $schema header is authoritative."},
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."},
    "entities": {
      "type": "array",
      "description": "The scene's entities (id-keyed stable collections land with the full M2 schema).",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "components"],
        "properties": {
          "name": {"type": "string"},
          "notes": {"description": "Schema-blessed annotations at the entity level."},
          "components": {
            "type": "object",
            "additionalProperties": false,
            "properties": {
              "notes": {"description": "Schema-blessed annotations at the component-map level."},
              "transform": {
                "type": "object",
                "additionalProperties": false,
                "required": ["position"],
                "properties": {
                  "notes": {"description": "Schema-blessed annotations."},
                  "position": {
                    "type": "array",
                    "items": {"type": "number"},
                    "x-ctx-units": "m",
                    "x-ctx-storage": "f32x3",
                    "description": "World position, meters (SI units law)."
                  }
                }
              },
              "camera": {
                "type": "object",
                "additionalProperties": false,
                "required": ["fov", "near", "far"],
                "properties": {
                  "notes": {"description": "Schema-blessed annotations."},
                  "fov": {"type": "number", "x-ctx-units": "rad", "x-ctx-storage": "f32", "description": "Vertical field of view, RADIANS (the units law: no degrees in authored data)."},
                  "near": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32"},
                  "far": {"type": "number", "x-ctx-units": "m", "x-ctx-storage": "f32"}
                }
              }
            }
          }
        }
      }
    }
  }
})";

constexpr std::string_view kProjectSchemaJson = R"({
  "$id": "ctx:project",
  "version": 1,
  "type": "object",
  "additionalProperties": false,
  "required": ["engine", "name", "scene"],
  "description": "The project manifest `context new` scaffolds (R-QA-006 runnable template).",
  "properties": {
    "engine": {"type": "string", "enum": ["context"]},
    "name": {"type": "string"},
    "scene": {"type": "string", "description": "Project-relative path of the startup scene (becomes a typed x-ctx-ref when the asset database lands, L-34/L-36)."},
    "notes": {"description": "Schema-blessed human/AI annotations — string or array of strings (L-32 bans JSON comments)."}
  }
})";

[[nodiscard]] KindSchema compile_engine_schema(std::string_view schema_json)
{
    std::vector<std::string> problems;
    std::optional<KindSchema> compiled = compile_kind_schema(schema_json, problems);
    if (!compiled.has_value())
    {
        // An engine schema that violates its own vocabulary law is a programmer error caught by
        // the schema tests; fail loudly rather than publish a lawless schema.
        for (const std::string& p : problems)
            std::fprintf(stderr, "engine kind schema invalid: %s\n", p.c_str());
        std::abort();
    }
    return std::move(*compiled);
}

// --- introspection projection ---------------------------------------------------------------------

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    object.type = JsonValue::Type::object;
    object.members.push_back(JsonMember{std::string(key), std::move(value)});
}

[[nodiscard]] JsonValue make_string(std::string_view s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::string(s);
    return v;
}

// Flatten one schema node into per-field index entries (DATA-space pointers: "/<prop>" for
// properties, "<base>/[]" for array items, "<base>/(<tag>)" for union variants).
void collect_fields(const JsonValue& node, const std::string& data_pointer, JsonValue& fields)
{
    if (const JsonValue* properties = find_member(node, "properties"); properties != nullptr)
        for (const JsonMember& property : properties->members)
        {
            const std::string child_pointer = data_pointer + "/" + property.key;
            const JsonValue* union_spec = find_member(property.value, kKeyUnion);
            JsonValue entry;
            set_member(entry, "pointer", make_string(child_pointer));
            if (property.key == kNotesField)
                set_member(entry, "type", make_string("notes")); // the blessed annotation field
            else if (const JsonValue* type = find_member(property.value, "type"); type != nullptr)
                set_member(entry, "type", make_string(type->string_value));
            if (const JsonValue* semantic = find_member(property.value, kKeySemanticType);
                semantic != nullptr)
                set_member(entry, "semantic", make_string(semantic->string_value));
            if (const JsonValue* units = find_member(property.value, kKeyUnits); units != nullptr)
                set_member(entry, "units", make_string(units->string_value));
            if (const JsonValue* storage = find_member(property.value, kKeyStorage);
                storage != nullptr)
                set_member(entry, "storage", make_string(storage->string_value));
            if (const JsonValue* ref = find_member(property.value, kKeyRef); ref != nullptr)
                set_member(entry, "ref", make_string(ref->string_value));
            if (union_spec != nullptr)
            {
                JsonValue tags;
                tags.type = JsonValue::Type::array;
                for (const JsonMember& variant : union_spec->members)
                    tags.elements.push_back(make_string(variant.key));
                set_member(entry, "unionTags", std::move(tags));
            }
            if (const JsonValue* description = find_member(property.value, "description");
                description != nullptr)
                set_member(entry, "description", make_string(description->string_value));
            fields.elements.push_back(std::move(entry));

            collect_fields(property.value, child_pointer, fields);
            if (union_spec != nullptr)
                for (const JsonMember& variant : union_spec->members)
                    collect_fields(variant.value, child_pointer + "/(" + variant.key + ")",
                                   fields);
        }
    if (const JsonValue* items = find_member(node, "items"); items != nullptr)
        collect_fields(*items, data_pointer + "/[]", fields);
}

} // namespace

std::optional<KindSchema> compile_kind_schema(std::string_view schema_json,
                                              std::vector<std::string>& problems)
{
    serializer::ParseResult parsed = serializer::parse_json(schema_json);
    if (!parsed.ok)
    {
        for (const serializer::Diagnostic& d : parsed.diagnostics)
            add_problem(problems, "", d.code + ": " + d.message);
        return std::nullopt;
    }

    check_schema_node(parsed.root, "", /*is_root=*/true, problems);
    if (!problems.empty())
        return std::nullopt;

    KindSchema schema;
    schema.id = find_member(parsed.root, "$id")->string_value;
    schema.version = find_member(parsed.root, "version")->int_value;
    if (!serializer::serialize_canonical(parsed.root, schema.canonical_doc))
    {
        add_problem(problems, "", "schema document is not canonically serializable");
        return std::nullopt;
    }
    schema.doc = std::move(parsed.root);
    return schema;
}

void SchemaSet::add(KindSchema schema)
{
    for (KindSchema& existing : schemas_)
        if (existing.id == schema.id && existing.version == schema.version)
        {
            existing = std::move(schema);
            return;
        }
    schemas_.push_back(std::move(schema));
}

const KindSchema* SchemaSet::find(std::string_view id, std::int64_t version) const noexcept
{
    for (const KindSchema& s : schemas_)
        if (s.id == id && s.version == version)
            return &s;
    return nullptr;
}

const KindSchema* SchemaSet::latest(std::string_view id) const noexcept
{
    const KindSchema* best = nullptr;
    for (const KindSchema& s : schemas_)
        if (s.id == id && (best == nullptr || s.version > best->version))
            best = &s;
    return best;
}

const SchemaSet& engine_schemas()
{
    static const SchemaSet set = [] {
        SchemaSet s;
        s.add(compile_engine_schema(kSceneSchemaJson));
        s.add(compile_engine_schema(kProjectSchemaJson));
        return s;
    }();
    return set;
}

std::string introspection_json(const KindSchema& schema)
{
    JsonValue entry;
    set_member(entry, "id", make_string(schema.id));
    JsonValue version;
    version.type = JsonValue::Type::integer;
    version.int_value = schema.version;
    set_member(entry, "version", std::move(version));

    JsonValue fields;
    fields.type = JsonValue::Type::array;
    collect_fields(schema.doc, "", fields);
    set_member(entry, "fields", std::move(fields));

    // The full published schema document rides along, so `describe` consumers get the authoritative
    // versioned JSON Schema (R-DATA-006) — the fields index above is a derived convenience view.
    set_member(entry, "schema", schema.doc);

    std::string out;
    if (!serializer::serialize_canonical(entry, out))
        out = "{}"; // unreachable: introspection trees carry no non-finite numbers
    return out;
}

} // namespace context::editor::schema

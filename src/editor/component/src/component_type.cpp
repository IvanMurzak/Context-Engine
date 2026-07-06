// Declarative component-type compiler (R-LANG-010): parse + validate a declarative definition, derive
// the storage layout, and publish the schema. Shares the M2 x-ctx-* vocabulary + canonical serializer.

#include "context/editor/component/component_type.h"

#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace context::editor::component
{
namespace
{

using serializer::JsonValue;

// FNV-1a 64-bit constants (the engine's canonical-hash family — serializer::canonical_hash_of, the
// runtime state hash; reused here so the layout hash is the same platform-independent digest family).
constexpr std::uint64_t kFnvOffset = 1469598103934664385ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fnv1a_bytes(std::uint64_t& h, const void* data, std::size_t n) noexcept
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

void fnv1a_str(std::uint64_t& h, std::string_view s) noexcept { fnv1a_bytes(h, s.data(), s.size()); }

// Round `n` up to the next multiple of `align` (align is a power of two: 1/2/4/8).
[[nodiscard]] std::size_t align_up(std::size_t n, std::size_t align) noexcept
{
    return (n + align - 1) & ~(align - 1);
}

const JsonValue* find_member(const JsonValue& object, std::string_view key) noexcept
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

void add_problem(std::vector<std::string>& problems, std::string_view pointer, std::string_view msg)
{
    problems.push_back((pointer.empty() ? std::string("<root>") : std::string(pointer)) + ": " +
                       std::string(msg));
}

// A component-type id is "<ns>:<type>" with each segment [a-z][a-z0-9_-]* (the kind-id / union-tag
// segment grammar). Keeps declared component ids in the same namespace shape as file kinds.
[[nodiscard]] bool is_id_segment(std::string_view s) noexcept
{
    if (s.empty() || !(s.front() >= 'a' && s.front() <= 'z'))
        return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

[[nodiscard]] bool is_valid_type_id(std::string_view id) noexcept
{
    const std::size_t colon = id.find(':');
    if (colon == std::string_view::npos)
        return false;
    return is_id_segment(id.substr(0, colon)) && is_id_segment(id.substr(colon + 1));
}

// A field name is [a-z][a-z0-9_]* — a stable JSON member key + a well-behaved identifier for the
// derived TS/WASM accessors (a follow-up task) with no namespacing needed inside one component.
[[nodiscard]] bool is_valid_field_name(std::string_view name) noexcept
{
    if (name.empty() || !(name.front() >= 'a' && name.front() <= 'z'))
        return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

[[nodiscard]] bool is_known_key(std::string_view key, const std::vector<std::string_view>& allowed)
{
    for (const std::string_view a : allowed)
        if (a == key)
            return true;
    return false;
}

constexpr std::array<std::pair<std::string_view, ScalarKind>, 10> kScalarBases{{
    {"f32", ScalarKind::f32},
    {"f64", ScalarKind::f64},
    {"i8", ScalarKind::i8},
    {"i16", ScalarKind::i16},
    {"i32", ScalarKind::i32},
    {"i64", ScalarKind::i64},
    {"u8", ScalarKind::u8},
    {"u16", ScalarKind::u16},
    {"u32", ScalarKind::u32},
    {"u64", ScalarKind::u64},
}};

} // namespace

std::string_view scalar_kind_id(ScalarKind kind) noexcept
{
    for (const auto& [id, k] : kScalarBases)
        if (k == kind)
            return id;
    return "f32"; // unreachable: kScalarBases covers every enumerator
}

std::size_t scalar_byte_width(ScalarKind kind) noexcept
{
    switch (kind)
    {
    case ScalarKind::i8:
    case ScalarKind::u8:
        return 1;
    case ScalarKind::i16:
    case ScalarKind::u16:
        return 2;
    case ScalarKind::f32:
    case ScalarKind::i32:
    case ScalarKind::u32:
        return 4;
    case ScalarKind::f64:
    case ScalarKind::i64:
    case ScalarKind::u64:
        return 8;
    }
    return 4; // unreachable: the switch is exhaustive over ScalarKind
}

std::optional<StorageLayout> parse_storage_layout(std::string_view layout) noexcept
{
    // Mirror schema::is_storage_layout exactly: rfind('x'); a valid lane suffix splits the base,
    // otherwise the whole string is the base (a bare scalar, lanes == 1). The two stay in lockstep.
    std::string_view base = layout;
    unsigned lanes = 1;
    if (const std::size_t x = layout.rfind('x'); x != std::string_view::npos)
    {
        const std::string_view lane_str = layout.substr(x + 1);
        if (lane_str == "2" || lane_str == "3" || lane_str == "4" || lane_str == "9" ||
            lane_str == "16")
        {
            base = layout.substr(0, x);
            lanes = static_cast<unsigned>(std::stoul(std::string(lane_str)));
        }
    }
    for (const auto& [id, kind] : kScalarBases)
        if (id == base)
            return StorageLayout{kind, lanes};
    return std::nullopt;
}

const ComponentField* ComponentTypeSchema::field(std::string_view name) const noexcept
{
    for (const ComponentField& f : fields)
        if (f.name == name)
            return &f;
    return nullptr;
}

std::uint64_t layout_hash_of(const std::vector<ComponentField>& fields) noexcept
{
    std::uint64_t h = kFnvOffset;
    for (const ComponentField& f : fields)
    {
        fnv1a_str(h, f.name);
        fnv1a_bytes(h, "\0", 1); // name/base separator so ("ab","c") != ("a","bc")
        fnv1a_str(h, scalar_kind_id(f.storage.base));
        const std::uint32_t lanes = f.storage.lanes;
        fnv1a_bytes(h, &lanes, sizeof(lanes));
    }
    return h;
}

std::optional<ComponentTypeSchema> compile_component_type(std::string_view schema_json,
                                                          std::vector<std::string>& problems)
{
    const serializer::ParseResult parsed = serializer::parse_json(schema_json);
    if (!parsed.ok)
    {
        for (const serializer::Diagnostic& d : parsed.diagnostics)
            problems.push_back("json parse (" + std::to_string(d.line) + ":" +
                               std::to_string(d.column) + "): " + d.code + " — " + d.message);
        if (parsed.diagnostics.empty())
            add_problem(problems, "", "not valid JSON");
        return std::nullopt;
    }

    const JsonValue& root = parsed.root;
    if (root.type != JsonValue::Type::object)
    {
        add_problem(problems, "", "the component definition must be a JSON object");
        return std::nullopt;
    }

    const std::size_t start = problems.size();
    ComponentTypeSchema type;

    // Reject unknown root keys (the declarative dialect is closed — a typo is an error, not silently
    // ignored). "$schema" is tolerated as an editor-tool convenience header.
    const std::vector<std::string_view> root_keys{"$id",   "$schema",     "version",
                                                  "fields", "notes", "description"};
    for (const serializer::JsonMember& m : root.members)
        if (!is_known_key(m.key, root_keys))
            add_problem(problems, "/" + m.key, "unknown key in a component definition");

    const JsonValue* id = find_member(root, "$id");
    if (id == nullptr || id->type != JsonValue::Type::string || !is_valid_type_id(id->string_value))
        add_problem(problems, "/$id", "missing or malformed \"$id\" (want \"<ns>:<type>\")");
    else
        type.id = id->string_value;

    const JsonValue* version = find_member(root, "version");
    if (version == nullptr || version->type != JsonValue::Type::integer || version->int_value < 1)
        add_problem(problems, "/version", "missing or invalid \"version\" (want an integer >= 1)");
    else
        type.version = version->int_value;

    if (const JsonValue* notes = find_member(root, "notes");
        notes != nullptr && !schema::is_valid_notes(*notes))
        add_problem(problems, "/notes", "\"notes\" must be a string or an array of strings");

    if (const JsonValue* desc = find_member(root, "description");
        desc != nullptr && desc->type != JsonValue::Type::string)
        add_problem(problems, "/description", "\"description\" must be a string");

    const JsonValue* fields = find_member(root, "fields");
    if (fields == nullptr || fields->type != JsonValue::Type::array || fields->elements.empty())
    {
        add_problem(problems, "/fields", "missing or empty \"fields\" (want a non-empty array)");
        return std::nullopt;
    }

    const std::vector<std::string_view> field_keys{"name",        "x-ctx-storage", "x-ctx-units",
                                                   "x-ctx-type", "description",   "notes"};
    std::size_t cursor = 0;
    std::size_t max_align = 1;
    for (std::size_t i = 0; i < fields->elements.size(); ++i)
    {
        const JsonValue& fnode = fields->elements[i];
        const std::string ptr = "/fields/" + std::to_string(i);
        if (fnode.type != JsonValue::Type::object)
        {
            add_problem(problems, ptr, "each field must be a JSON object");
            continue;
        }
        for (const serializer::JsonMember& m : fnode.members)
            if (!is_known_key(m.key, field_keys))
                add_problem(problems, ptr + "/" + m.key, "unknown key in a field definition");

        ComponentField field;

        const JsonValue* name = find_member(fnode, "name");
        if (name == nullptr || name->type != JsonValue::Type::string ||
            !is_valid_field_name(name->string_value))
            add_problem(problems, ptr + "/name",
                        "missing or malformed \"name\" (want [a-z][a-z0-9_]*)");
        else if (type.field(name->string_value) != nullptr)
            add_problem(problems, ptr + "/name", "duplicate field name \"" + name->string_value +
                                                     "\"");
        else
            field.name = name->string_value;

        const JsonValue* storage = find_member(fnode, "x-ctx-storage");
        if (storage == nullptr || storage->type != JsonValue::Type::string)
            add_problem(problems, ptr + "/x-ctx-storage", "missing \"x-ctx-storage\" layout string");
        else if (!schema::is_storage_layout(storage->string_value))
            add_problem(problems, ptr + "/x-ctx-storage",
                        "invalid storage layout \"" + storage->string_value + "\"");
        else if (const std::optional<StorageLayout> parsed_layout =
                     parse_storage_layout(storage->string_value);
                 parsed_layout.has_value())
            field.storage = *parsed_layout;

        if (const JsonValue* units = find_member(fnode, "x-ctx-units");
            units != nullptr &&
            (units->type != JsonValue::Type::string || !schema::is_si_unit(units->string_value)))
            add_problem(problems, ptr + "/x-ctx-units",
                        "\"x-ctx-units\" must be a pinned SI unit (units law)");

        if (const JsonValue* sem = find_member(fnode, "x-ctx-type");
            sem != nullptr && (sem->type != JsonValue::Type::string ||
                               !schema::is_semantic_type_id(sem->string_value)))
            add_problem(problems, ptr + "/x-ctx-type", "unknown \"x-ctx-type\" semantic type");

        if (const JsonValue* desc = find_member(fnode, "description");
            desc != nullptr && desc->type != JsonValue::Type::string)
            add_problem(problems, ptr + "/description", "\"description\" must be a string");

        if (const JsonValue* notes = find_member(fnode, "notes");
            notes != nullptr && !schema::is_valid_notes(*notes))
            add_problem(problems, ptr + "/notes", "\"notes\" must be a string or array of strings");

        // Derive the packed offset once the field is well-formed (name + storage present).
        if (!field.name.empty())
        {
            const std::size_t a = field.storage.align();
            cursor = align_up(cursor, a);
            field.offset = cursor;
            cursor += field.storage.byte_size();
            if (a > max_align)
                max_align = a;
            type.fields.push_back(std::move(field));
        }
    }

    if (problems.size() != start)
        return std::nullopt;

    type.align = max_align;
    type.size = align_up(cursor, max_align);
    if (type.size == 0)
        type.size = 1; // a defensive floor; fields is non-empty so cursor >= 1 in practice
    type.layout_hash = layout_hash_of(type.fields);
    type.doc = root;
    if (!serializer::serialize_canonical(type.doc, type.canonical_doc))
        type.canonical_doc.clear(); // unreachable: a definition carries no non-finite numbers

    return type;
}

std::string component_type_introspection_json(const ComponentTypeSchema& type)
{
    auto make_string = [](std::string_view s)
    {
        JsonValue v;
        v.type = JsonValue::Type::string;
        v.string_value = std::string(s);
        return v;
    };
    auto make_uint = [](std::uint64_t n)
    {
        JsonValue v;
        v.type = JsonValue::Type::unsigned_integer;
        v.uint_value = n;
        return v;
    };
    auto set = [](JsonValue& obj, std::string_view key, JsonValue value)
    {
        obj.type = JsonValue::Type::object;
        obj.members.push_back(serializer::JsonMember{std::string(key), std::move(value)});
    };

    JsonValue entry;
    set(entry, "id", make_string(type.id));

    JsonValue version;
    version.type = JsonValue::Type::integer;
    version.int_value = type.version;
    set(entry, "version", std::move(version));

    set(entry, "size", make_uint(type.size));
    set(entry, "align", make_uint(type.align));
    set(entry, "layoutHash", make_string(std::to_string(type.layout_hash)));

    JsonValue field_array;
    field_array.type = JsonValue::Type::array;
    for (const ComponentField& f : type.fields)
    {
        JsonValue fe;
        set(fe, "name", make_string(f.name));
        std::string layout(scalar_kind_id(f.storage.base));
        if (f.storage.lanes != 1)
            layout += "x" + std::to_string(f.storage.lanes);
        set(fe, "x-ctx-storage", make_string(layout));
        set(fe, "offset", make_uint(f.offset));
        set(fe, "size", make_uint(f.storage.byte_size()));
        set(fe, "lanes", make_uint(f.storage.lanes));
        field_array.elements.push_back(std::move(fe));
    }
    set(entry, "fields", std::move(field_array));

    // The full published definition rides along (the authoritative versioned schema, R-CLI-005); the
    // fields index above is the derived layout view (offsets/sizes the schema text does not carry).
    set(entry, "schema", type.doc);

    std::string out;
    if (!serializer::serialize_canonical(entry, out))
        out = "{}"; // unreachable: introspection trees carry no non-finite numbers
    return out;
}

} // namespace context::editor::component

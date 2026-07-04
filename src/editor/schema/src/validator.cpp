// Document validation against the registered kind schemas — see validator.h.

#include "context/editor/schema/validator.h"

#include "context/editor/schema/json_access.h"
#include "context/editor/schema/vocabulary.h"
#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace context::editor::schema
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// --- JSON-pointer location in source bytes (line/column of a pointed-at value) ------------------

struct Cursor
{
    std::string_view bytes;
    std::size_t i = 0;
    std::size_t line = 1;
    std::size_t column = 1;

    [[nodiscard]] bool eof() const noexcept { return i >= bytes.size(); }
    [[nodiscard]] char peek() const noexcept { return bytes[i]; }

    void advance() noexcept
    {
        if (bytes[i] == '\n')
        {
            ++line;
            column = 1;
        }
        else
        {
            ++column;
        }
        ++i;
    }

    void skip_ws() noexcept
    {
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r'))
            advance();
    }
};

void append_utf8(std::uint32_t cp, std::string& out)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

[[nodiscard]] bool read_hex4(Cursor& c, std::uint32_t& out)
{
    out = 0;
    for (int k = 0; k < 4; ++k)
    {
        if (c.eof())
            return false;
        const char ch = c.peek();
        std::uint32_t digit = 0;
        if (ch >= '0' && ch <= '9')
            digit = static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            digit = static_cast<std::uint32_t>(ch - 'a') + 10;
        else if (ch >= 'A' && ch <= 'F')
            digit = static_cast<std::uint32_t>(ch - 'A') + 10;
        else
            return false;
        out = (out << 4) | digit;
        c.advance();
    }
    return true;
}

// Read the JSON string at the cursor (expects '"'), unescaping into `out`. The input already
// parsed as strict JSON, so malformed escapes cannot occur; the checks keep the walker total.
[[nodiscard]] bool read_string(Cursor& c, std::string& out)
{
    out.clear();
    if (c.eof() || c.peek() != '"')
        return false;
    c.advance();
    while (!c.eof() && c.peek() != '"')
    {
        if (c.peek() != '\\')
        {
            out.push_back(c.peek());
            c.advance();
            continue;
        }
        c.advance();
        if (c.eof())
            return false;
        const char esc = c.peek();
        c.advance();
        switch (esc)
        {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
        {
            std::uint32_t cp = 0;
            if (!read_hex4(c, cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && !c.eof() && c.peek() == '\\')
            {
                c.advance();
                if (c.eof() || c.peek() != 'u')
                    return false;
                c.advance();
                std::uint32_t low = 0;
                if (!read_hex4(c, low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            append_utf8(cp, out);
            break;
        }
        default:
            return false;
        }
    }
    if (c.eof())
        return false;
    c.advance(); // closing quote
    return true;
}

bool skip_value(Cursor& c); // forward

[[nodiscard]] bool skip_container(Cursor& c, char open, char close)
{
    c.advance(); // the opening bracket
    c.skip_ws();
    if (!c.eof() && c.peek() == close)
    {
        c.advance();
        return true;
    }
    while (!c.eof())
    {
        if (open == '{')
        {
            std::string key;
            if (!read_string(c, key))
                return false;
            c.skip_ws();
            if (c.eof() || c.peek() != ':')
                return false;
            c.advance();
            c.skip_ws();
        }
        if (!skip_value(c))
            return false;
        c.skip_ws();
        if (c.eof())
            return false;
        if (c.peek() == ',')
        {
            c.advance();
            c.skip_ws();
            continue;
        }
        if (c.peek() == close)
        {
            c.advance();
            return true;
        }
        return false;
    }
    return false;
}

bool skip_value(Cursor& c)
{
    if (c.eof())
        return false;
    const char ch = c.peek();
    if (ch == '"')
    {
        std::string ignored;
        return read_string(c, ignored);
    }
    if (ch == '{')
        return skip_container(c, '{', '}');
    if (ch == '[')
        return skip_container(c, '[', ']');
    // Literal: number / true / false / null — runs to the next structural delimiter.
    while (!c.eof())
    {
        const char l = c.peek();
        if (l == ',' || l == '}' || l == ']' || l == ' ' || l == '\t' || l == '\n' || l == '\r')
            break;
        c.advance();
    }
    return true;
}

// Split an RFC 6901 pointer into unescaped reference tokens (~1 -> '/', ~0 -> '~').
[[nodiscard]] std::vector<std::string> pointer_tokens(std::string_view pointer)
{
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < pointer.size())
    {
        // Each token starts at a '/'.
        ++i;
        std::string token;
        while (i < pointer.size() && pointer[i] != '/')
        {
            if (pointer[i] == '~' && i + 1 < pointer.size())
            {
                token.push_back(pointer[i + 1] == '1' ? '/' : '~');
                i += 2;
                continue;
            }
            token.push_back(pointer[i]);
            ++i;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

// Escape one key for embedding into a JSON pointer ('~' -> "~0", '/' -> "~1").
[[nodiscard]] std::string pointer_escape(std::string_view key)
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

} // namespace

bool locate_pointer(std::string_view bytes, std::string_view pointer, std::size_t& line,
                    std::size_t& column)
{
    Cursor c{bytes};
    c.skip_ws();
    if (c.eof())
        return false;

    for (const std::string& token : pointer_tokens(pointer))
    {
        if (c.peek() == '{')
        {
            c.advance();
            c.skip_ws();
            bool found = false;
            while (!c.eof() && c.peek() != '}')
            {
                std::string key;
                if (!read_string(c, key))
                    return false;
                c.skip_ws();
                if (c.eof() || c.peek() != ':')
                    return false;
                c.advance();
                c.skip_ws();
                if (key == token)
                {
                    found = true;
                    break;
                }
                if (!skip_value(c))
                    return false;
                c.skip_ws();
                if (!c.eof() && c.peek() == ',')
                {
                    c.advance();
                    c.skip_ws();
                }
            }
            if (!found)
                return false;
        }
        else if (c.peek() == '[')
        {
            std::size_t index = 0;
            if (token.empty())
                return false;
            for (const char ch : token)
            {
                if (ch < '0' || ch > '9')
                    return false;
                index = index * 10 + static_cast<std::size_t>(ch - '0');
            }
            c.advance();
            c.skip_ws();
            for (std::size_t skip = 0; skip < index; ++skip)
            {
                if (!skip_value(c))
                    return false;
                c.skip_ws();
                if (c.eof() || c.peek() != ',')
                    return false;
                c.advance();
                c.skip_ws();
            }
            if (c.eof() || c.peek() == ']')
                return false;
        }
        else
        {
            return false; // the pointer descends into a scalar
        }
    }

    line = c.line;
    column = c.column;
    return true;
}

namespace
{

// --- the document walker --------------------------------------------------------------------------

// Diagnostic codes that do NOT block derivation (informational — kinds/versions register
// incrementally, so an unrecognized binding must never hold a file's derived state hostage).
[[nodiscard]] bool is_blocking_code(std::string_view code) noexcept
{
    return code != "schema.unknown_kind" && code != "schema.version_unregistered";
}

class Walker
{
public:
    Walker(std::string_view source, const RefTargetResolver* resolver, ValidationReport& report)
        : source_(source), resolver_(resolver), report_(report)
    {
    }

    void diag(std::string code, const std::string& pointer, std::string message)
    {
        ValidationDiagnostic d;
        d.code = std::move(code);
        d.message = std::move(message);
        d.pointer = pointer;
        (void)locate_pointer(source_, pointer, d.line, d.column);
        if (is_blocking_code(d.code))
            report_.ok = false;
        report_.diagnostics.push_back(std::move(d));
    }

    // Validate `value` against schema node `node`. `allow_tag` admits the pinned union "type"
    // member when the node is an x-ctx-union variant schema.
    void walk(const JsonValue& value, const JsonValue& node, const std::string& pointer,
              bool is_root, bool allow_tag = false)
    {
        if (const JsonValue* ref = find_member(node, kKeyRef); ref != nullptr)
        {
            check_ref(value, ref->string_value, pointer);
            return;
        }
        if (const JsonValue* union_spec = find_member(node, kKeyUnion); union_spec != nullptr)
        {
            check_union(value, *union_spec, pointer);
            return;
        }
        if (const JsonValue* semantic = find_member(node, kKeySemanticType); semantic != nullptr)
        {
            const SemanticType type = semantic_type_from_id(semantic->string_value);
            for (const SemanticIssue& issue : check_semantic(type, value))
                diag("schema.semantic_invalid", pointer + issue.subpointer,
                     std::string(semantic_type_id(type)) + ": " + issue.message);
            return;
        }

        const JsonValue* type = find_member(node, "type");
        if (type != nullptr && !carrier_matches(value, type->string_value))
        {
            diag("schema.type_mismatch", pointer,
                 "expected " + type->string_value + " (kind schema)");
            return;
        }

        if (const JsonValue* enumeration = find_member(node, "enum"); enumeration != nullptr)
        {
            bool matched = false;
            for (const JsonValue& option : enumeration->elements)
                matched = matched || (value.type == JsonValue::Type::string &&
                                      value.string_value == option.string_value);
            if (!matched)
                diag("schema.enum_mismatch", pointer, "value is not one of the declared options");
        }

        if (value.type == JsonValue::Type::object)
            walk_object(value, node, pointer, is_root, allow_tag);
        else if (value.type == JsonValue::Type::array)
            walk_array(value, node, pointer);
    }

private:
    [[nodiscard]] static bool carrier_matches(const JsonValue& value, std::string_view type)
    {
        switch (value.type)
        {
        case JsonValue::Type::object:
            return type == "object";
        case JsonValue::Type::array:
            return type == "array";
        case JsonValue::Type::string:
            return type == "string";
        case JsonValue::Type::boolean:
            return type == "boolean";
        case JsonValue::Type::integer:
        case JsonValue::Type::unsigned_integer:
            return type == "number" || type == "integer";
        case JsonValue::Type::number:
            return type == "number";
        case JsonValue::Type::null_value:
        default:
            return false;
        }
    }

    void walk_object(const JsonValue& value, const JsonValue& node, const std::string& pointer,
                     bool is_root, bool allow_tag)
    {
        const JsonValue* properties = find_member(node, "properties");
        const JsonValue* additional = find_member(node, "additionalProperties");
        const bool allow_additional =
            additional == nullptr || additional->type != JsonValue::Type::boolean ||
            additional->boolean_value;

        for (const JsonMember& m : value.members)
        {
            const std::string child_pointer = pointer + "/" + pointer_escape(m.key);
            if (m.key == kNotesField)
            {
                // The schema-blessed annotation affordance (L-32): accepted on EVERY object level
                // of every authored kind, with the pinned shape.
                if (!is_valid_notes(m.value))
                    diag("schema.notes_invalid", child_pointer,
                         "notes are a string or an array of strings");
                continue;
            }
            if (is_root && (m.key == "$schema" || m.key == "version" ||
                            m.key == "componentVersions"))
                continue; // the L-32 header block — kind-independent, shape-checked by the header read
            if (allow_tag && m.key == "type")
                continue; // the pinned union tag, already checked by check_union
            const JsonValue* property =
                properties != nullptr ? find_member(*properties, m.key) : nullptr;
            if (property != nullptr)
            {
                walk(m.value, *property, child_pointer, /*is_root=*/false);
                continue;
            }
            if (!allow_additional)
                diag("schema.unknown_property", child_pointer,
                     "property is not declared by the kind schema");
        }

        if (const JsonValue* required = find_member(node, "required"); required != nullptr)
            for (const JsonValue& name : required->elements)
                if (name.type == JsonValue::Type::string &&
                    find_member(value, name.string_value) == nullptr)
                    diag("schema.required_missing", pointer,
                         "missing required property \"" + name.string_value + "\"");
    }

    void walk_array(const JsonValue& value, const JsonValue& node, const std::string& pointer)
    {
        // A lane-suffixed storage layout pins the arity: "f32x3" stores exactly 3 elements. The
        // declaration grammar was law-checked at schema compile; here the DATA must fit it.
        if (const JsonValue* storage = find_member(node, kKeyStorage); storage != nullptr)
        {
            const std::string& layout = storage->string_value;
            if (const std::size_t x = layout.rfind('x'); x != std::string::npos)
            {
                std::size_t lanes = 0;
                for (std::size_t k = x + 1; k < layout.size(); ++k)
                    lanes = lanes * 10 + static_cast<std::size_t>(layout[k] - '0');
                if (lanes >= 2 && value.elements.size() != lanes)
                    diag("schema.storage_arity", pointer,
                         "storage layout " + layout + " holds exactly " + std::to_string(lanes) +
                             " elements");
            }
        }

        if (const JsonValue* items = find_member(node, "items"); items != nullptr)
            for (std::size_t i = 0; i < value.elements.size(); ++i)
                walk(value.elements[i], *items, pointer + "/" + std::to_string(i),
                     /*is_root=*/false);
    }

    void check_ref(const JsonValue& value, const std::string& target_kind,
                   const std::string& pointer)
    {
        if (value.type != JsonValue::Type::object)
        {
            diag("schema.ref_shape_invalid", pointer,
                 "a reference is {\"$ref\": \"<guid>\", \"path\": \"<hint>\"} or "
                 "{\"$entity\": \"<id>\"} (L-34 dual form)");
            return;
        }
        const JsonValue* guid = find_member(value, kRefGuidField);
        const JsonValue* entity = find_member(value, kRefEntityField);
        if (entity != nullptr)
        {
            // Same-file entity reference: {"$entity": "<id>"} and nothing else.
            const bool shape_ok = guid == nullptr && value.members.size() == 1 &&
                                  entity->type == JsonValue::Type::string &&
                                  !entity->string_value.empty();
            if (!shape_ok)
                diag("schema.ref_shape_invalid", pointer,
                     "a same-file reference is exactly {\"$entity\": \"<id>\"}");
            return;
        }
        if (guid == nullptr || guid->type != JsonValue::Type::string || guid->string_value.empty())
        {
            diag("schema.ref_shape_invalid", pointer,
                 "a cross-file reference carries an authoritative \"$ref\" GUID (L-34)");
            return;
        }
        for (const JsonMember& m : value.members)
            if (m.key != kRefGuidField &&
                !(m.key == kRefPathField && m.value.type == JsonValue::Type::string))
            {
                diag("schema.ref_shape_invalid", pointer,
                     "a cross-file reference is {\"$ref\", \"path\"?} — nothing else");
                return;
            }
        // Target-kind enforcement through the meta-lookup seam (activates with the asset db).
        if (resolver_ != nullptr)
            if (const std::optional<std::string> actual = resolver_->kind_of(guid->string_value);
                actual.has_value() && *actual != target_kind)
                diag("schema.ref_wrong_kind", pointer,
                     "reference targets kind \"" + *actual + "\" but the field requires \"" +
                         target_kind + "\" (x-ctx-ref)");
    }

    void check_union(const JsonValue& value, const JsonValue& union_spec,
                     const std::string& pointer)
    {
        if (value.type != JsonValue::Type::object)
        {
            diag("schema.union_tag_missing", pointer,
                 "a polymorphic value is a tagged object {\"type\": \"<ns>:<shape>\", ...}");
            return;
        }
        const JsonValue* tag = find_member(value, "type");
        if (tag == nullptr || tag->type != JsonValue::Type::string)
        {
            diag("schema.union_tag_missing", pointer,
                 "the pinned tagged-union convention requires a string \"type\" member");
            return;
        }
        if (!is_union_tag(tag->string_value))
        {
            diag("schema.union_tag_invalid", pointer + "/type",
                 "union tags follow the ONE pinned convention \"<ns>:<shape>\" — never ad-hoc "
                 "encodings (R-DATA-006)");
            return;
        }
        const JsonValue* variant = find_member(union_spec, tag->string_value);
        if (variant == nullptr)
        {
            diag("schema.union_tag_unknown", pointer + "/type",
                 "tag \"" + tag->string_value + "\" is not a declared variant");
            return;
        }
        walk(value, *variant, pointer, /*is_root=*/false, /*allow_tag=*/true);
    }

    std::string_view source_;
    const RefTargetResolver* resolver_;
    ValidationReport& report_;
};

} // namespace

ValidationReport validate_document(const JsonValue& root, std::string_view source_bytes,
                                   const SchemaSet& schemas, const RefTargetResolver* resolver)
{
    ValidationReport report;

    std::vector<serializer::Diagnostic> header_diagnostics;
    const serializer::DocumentHeader header =
        serializer::read_document_header(root, header_diagnostics);
    if (!header.has_schema)
    {
        // No usable `$schema` — the document is not schema-bound (validation skips). One case
        // still surfaces: a PRESENT-but-malformed `$schema` (wrong JSON type) is an attempted
        // binding with an authored header defect, not an opt-out — silently deriving the file
        // unvalidated would drop the defect with zero signal. A document with NO `$schema`
        // member never claims the L-32 header law, so its `version`/`componentVersions` shape
        // findings stay dropped (foreign JSON must not be blocked by a law it never opted into).
        const bool attempted_binding =
            std::any_of(header_diagnostics.begin(), header_diagnostics.end(),
                        [](const serializer::Diagnostic& d) {
                            return d.code == "header.schema_not_string";
                        });
        if (attempted_binding)
        {
            Walker walker(source_bytes, resolver, report);
            for (const serializer::Diagnostic& d : header_diagnostics)
                walker.diag(d.code, "", d.message);
        }
        return report;
    }

    report.schema_id = header.schema;
    Walker walker(source_bytes, resolver, report);

    const KindSchema* latest = schemas.latest(header.schema);
    if (latest == nullptr)
    {
        // Kinds register incrementally (R-CLI-005 live set): an unknown id is surfaced but must
        // never block a file's derivation.
        walker.diag("schema.unknown_kind", "/$schema",
                    "no schema registered for kind \"" + header.schema + "\"");
        return report;
    }

    report.schema_bound = true;

    // Header-shape findings become blocking once the document claims a registered kind.
    for (const serializer::Diagnostic& d : header_diagnostics)
        walker.diag(d.code, "", d.message);

    if (!header.has_version)
    {
        walker.diag("schema.version_missing", "",
                    "a schema-bound document carries an integer \"version\" (L-32 header)");
        return report;
    }
    report.version = header.version;

    const KindSchema* exact = schemas.find(header.schema, header.version);
    if (exact == nullptr)
    {
        if (header.version > latest->version)
        {
            // L-37: a payload stamped NEWER than the installed schema is never best-effort
            // parsed — the diagnostic surfaces and last-good state is retained.
            walker.diag("schema.newer_than_engine", "/version",
                        "document is stamped version " + std::to_string(header.version) +
                            " but the newest registered \"" + header.schema + "\" schema is " +
                            std::to_string(latest->version));
        }
        else
        {
            report.schema_bound = false;
            walker.diag("schema.version_unregistered", "/version",
                        "no registered \"" + header.schema + "\" schema at version " +
                            std::to_string(header.version) +
                            " (parse-time migration is a later M2 task)");
        }
        return report;
    }

    walker.walk(root, exact->doc, "", /*is_root=*/true);
    return report;
}

} // namespace context::editor::schema

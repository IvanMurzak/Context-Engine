// Sidecar-reference extraction implementation (see sidecar_ref.h).

#include "context/editor/serializer/sidecar_ref.h"

#include <limits>

namespace context::editor::serializer
{
namespace
{

// RFC 6901: "~" -> "~0", "/" -> "~1" inside one reference token.
void append_pointer_token(std::string& pointer, std::string_view token)
{
    pointer.push_back('/');
    for (char ch : token)
    {
        if (ch == '~')
            pointer += "~0";
        else if (ch == '/')
            pointer += "~1";
        else
            pointer.push_back(ch);
    }
}

[[nodiscard]] const JsonValue* find_member(const JsonValue& object, std::string_view key)
{
    for (const JsonMember& member : object.members)
        if (member.key == key)
            return &member.value;
    return nullptr;
}

// Classify one "$sidecar"-carrying object. Returns the ref when well-formed; otherwise appends the
// sidecar.ref_malformed diagnostic (naming the pointer + the first offending detail) and returns
// nullopt. Tree diagnostics carry no source position (the parse already consumed it), so line and
// column keep their 1-based defaults — the JSON pointer in the message is the location.
std::optional<SidecarRef> classify_ref(const JsonValue& object, const std::string& pointer,
                                       std::vector<Diagnostic>& diagnostics)
{
    const auto malformed = [&](std::string reason) {
        diagnostics.push_back(Diagnostic{
            "sidecar.ref_malformed", pointer + ": " + std::move(reason), 1, 1});
        return std::nullopt;
    };

    const JsonValue* relpath = find_member(object, "$sidecar");
    if (relpath == nullptr || relpath->type != JsonValue::Type::string)
        return malformed("\"$sidecar\" must be a string");
    if (relpath->string_value.empty())
        return malformed("\"$sidecar\" must be a non-empty relative path");

    const JsonValue* hash = find_member(object, "hash");
    if (hash == nullptr)
        return malformed("\"hash\" is required beside \"$sidecar\"");
    if (hash->type != JsonValue::Type::string)
        return malformed("\"hash\" must be a decimal string (a 64-bit hash exceeds the JSON "
                         "number domain)");
    const std::optional<std::uint64_t> parsed = parse_hash_string(hash->string_value);
    if (!parsed)
        return malformed("\"hash\" is not a canonical decimal 64-bit value: \"" +
                         hash->string_value + "\"");

    SidecarRef ref;
    ref.relpath = relpath->string_value;
    ref.hash = *parsed;
    ref.json_pointer = pointer;
    return ref;
}

void walk(const JsonValue& value, std::string& pointer, std::vector<SidecarRef>& refs,
          std::vector<Diagnostic>& diagnostics)
{
    if (value.type == JsonValue::Type::object)
    {
        // An object carrying "$sidecar" declares reference intent — classify it and treat it as a
        // leaf (well-formed or not; its members are the ref's payload, never authored content).
        if (find_member(value, "$sidecar") != nullptr)
        {
            if (std::optional<SidecarRef> ref = classify_ref(value, pointer, diagnostics))
                refs.push_back(std::move(*ref));
            return;
        }
        for (const JsonMember& member : value.members)
        {
            const std::size_t rollback = pointer.size();
            append_pointer_token(pointer, member.key);
            walk(member.value, pointer, refs, diagnostics);
            pointer.resize(rollback);
        }
        return;
    }
    if (value.type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < value.elements.size(); ++i)
        {
            const std::size_t rollback = pointer.size();
            append_pointer_token(pointer, std::to_string(i));
            walk(value.elements[i], pointer, refs, diagnostics);
            pointer.resize(rollback);
        }
    }
}

} // namespace

std::optional<std::uint64_t> parse_hash_string(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    if (text.size() > 1 && text.front() == '0')
        return std::nullopt; // leading zero — not the canonical encoding
    std::uint64_t value = 0;
    constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
    for (char ch : text)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (max - digit) / 10)
            return std::nullopt; // overflow
        value = value * 10 + digit;
    }
    return value;
}

bool is_sidecar_ref(const JsonValue& value)
{
    if (value.type != JsonValue::Type::object || find_member(value, "$sidecar") == nullptr)
        return false;
    const std::string pointer;
    std::vector<Diagnostic> diagnostics; // discarded — this predicate only answers well-formedness
    return classify_ref(value, pointer, diagnostics).has_value();
}

std::vector<SidecarRef> collect_sidecar_refs(const JsonValue& root,
                                             std::vector<Diagnostic>& diagnostics)
{
    std::vector<SidecarRef> refs;
    std::string pointer; // "" is the whole-document pointer (RFC 6901)
    walk(root, pointer, refs, diagnostics);
    return refs;
}

} // namespace context::editor::serializer

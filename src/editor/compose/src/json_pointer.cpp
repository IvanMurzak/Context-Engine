// RFC 6901 JSON-pointer helpers — see json_pointer.h.

#include "context/editor/compose/json_pointer.h"

#include <cstddef>

namespace context::editor::compose
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

// A canonical base-10 array index: "0", or a non-empty digit run without a leading zero. RFC 6901
// array semantics; "-" (append) is handled by the callers (it never resolves on the read path).
[[nodiscard]] bool parse_array_index(const std::string& token, std::size_t& index)
{
    if (token.empty() || token.size() > 10)
        return false;
    if (token.size() > 1 && token[0] == '0')
        return false;
    std::size_t value = 0;
    for (char c : token)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    index = value;
    return true;
}

[[nodiscard]] JsonValue* find_member_mutable(JsonValue& object, const std::string& key)
{
    for (JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

} // namespace

bool parse_json_pointer(std::string_view pointer, std::vector<std::string>& tokens)
{
    if (pointer.empty() || pointer.front() != '/')
        return false;
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 1; i <= pointer.size(); ++i)
    {
        if (i == pointer.size() || pointer[i] == '/')
        {
            out.push_back(current);
            current.clear();
            continue;
        }
        char c = pointer[i];
        if (c == '~')
        {
            if (i + 1 >= pointer.size())
                return false;
            char e = pointer[i + 1];
            if (e == '0')
                current.push_back('~');
            else if (e == '1')
                current.push_back('/');
            else
                return false;
            ++i;
            continue;
        }
        current.push_back(c);
    }
    tokens = std::move(out);
    return true;
}

const JsonValue* resolve_json_pointer(const JsonValue& root, std::string_view pointer)
{
    std::vector<std::string> tokens;
    if (!parse_json_pointer(pointer, tokens))
        return nullptr;
    const JsonValue* node = &root;
    for (const std::string& token : tokens)
    {
        if (node->type == JsonValue::Type::object)
        {
            const JsonValue* next = nullptr;
            for (const JsonMember& m : node->members)
            {
                if (m.key == token)
                {
                    next = &m.value;
                    break;
                }
            }
            if (next == nullptr)
                return nullptr;
            node = next;
            continue;
        }
        if (node->type == JsonValue::Type::array)
        {
            std::size_t index = 0;
            if (!parse_array_index(token, index) || index >= node->elements.size())
                return nullptr;
            node = &node->elements[index];
            continue;
        }
        return nullptr; // a scalar mid-path
    }
    return node;
}

bool set_json_pointer(JsonValue& root, std::string_view pointer, const JsonValue& value)
{
    std::vector<std::string> tokens;
    if (!parse_json_pointer(pointer, tokens))
        return false;

    // Two passes: prove the path is applicable first, so a failing override leaves `root`
    // untouched (no half-created intermediate members on the orphan path).
    const JsonValue* probe = &root;
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        if (probe->type == JsonValue::Type::object)
        {
            const JsonValue* next = nullptr;
            for (const JsonMember& m : probe->members)
            {
                if (m.key == tokens[i])
                {
                    next = &m.value;
                    break;
                }
            }
            if (next == nullptr)
                break; // the remainder is created as objects — always applicable
            probe = next;
            continue;
        }
        if (probe->type == JsonValue::Type::array)
        {
            std::size_t index = 0;
            if (!parse_array_index(tokens[i], index) || index >= probe->elements.size())
                return false; // arrays are never grown/retyped by an override
            probe = &probe->elements[index];
            continue;
        }
        return false; // a scalar mid-path is never retyped
    }

    JsonValue* node = &root;
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        const bool leaf = (i + 1 == tokens.size());
        if (node->type == JsonValue::Type::object)
        {
            JsonValue* next = find_member_mutable(*node, tokens[i]);
            if (next == nullptr)
            {
                JsonMember created;
                created.key = tokens[i];
                if (!leaf)
                    created.value.type = JsonValue::Type::object;
                node->members.push_back(std::move(created));
                next = &node->members.back().value;
            }
            if (leaf)
            {
                *next = value;
                return true;
            }
            node = next;
            continue;
        }
        // The probe pass proved this is an in-range array index.
        std::size_t index = 0;
        (void)parse_array_index(tokens[i], index);
        JsonValue* next = &node->elements[index];
        if (leaf)
        {
            *next = value;
            return true;
        }
        node = next;
    }
    return false; // unreachable: tokens is non-empty
}

} // namespace context::editor::compose

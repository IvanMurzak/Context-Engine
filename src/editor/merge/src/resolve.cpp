// Conflict-resolution application (see resolve.h).

#include "context/editor/merge/resolve.h"

#include "context/editor/compose/json_pointer.h"

#include <string>
#include <vector>

namespace context::editor::merge
{

using serializer::JsonValue;

namespace
{

// Remove the member/element an RFC 6901 pointer addresses. Resolves the PARENT, then erases the last
// token from it. Fails when the pointer is malformed, the parent does not resolve, or the leaf is
// absent.
bool remove_at_pointer(JsonValue& root, std::string_view pointer, std::string& error)
{
    std::vector<std::string> tokens;
    if (!compose::parse_json_pointer(pointer, tokens) || tokens.empty())
    {
        error = "the --path is not a valid non-empty RFC 6901 JSON pointer";
        return false;
    }

    JsonValue* parent = &root;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        const std::string& token = tokens[i];
        if (parent->type == JsonValue::Type::object)
        {
            JsonValue* next = nullptr;
            for (serializer::JsonMember& m : parent->members)
                if (m.key == token)
                {
                    next = &m.value;
                    break;
                }
            if (next == nullptr)
            {
                error = "the --path parent does not resolve";
                return false;
            }
            parent = next;
        }
        else if (parent->type == JsonValue::Type::array)
        {
            if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos ||
                (token.size() > 1 && token[0] == '0'))
            {
                error = "the --path has a non-index token against an array";
                return false;
            }
            const std::size_t index = std::stoull(token);
            if (index >= parent->elements.size())
            {
                error = "the --path array index is out of range";
                return false;
            }
            parent = &parent->elements[index];
        }
        else
        {
            error = "the --path traverses a scalar";
            return false;
        }
    }

    const std::string& leaf = tokens.back();
    if (parent->type == JsonValue::Type::object)
    {
        for (auto it = parent->members.begin(); it != parent->members.end(); ++it)
            if (it->key == leaf)
            {
                parent->members.erase(it);
                return true;
            }
        error = "the --path leaf member is absent";
        return false;
    }
    if (parent->type == JsonValue::Type::array)
    {
        if (leaf.empty() || leaf.find_first_not_of("0123456789") != std::string::npos ||
            (leaf.size() > 1 && leaf[0] == '0'))
        {
            error = "the --path leaf is not a canonical array index";
            return false;
        }
        const std::size_t index = std::stoull(leaf);
        if (index >= parent->elements.size())
        {
            error = "the --path array index is out of range";
            return false;
        }
        parent->elements.erase(parent->elements.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    error = "the --path parent is a scalar";
    return false;
}

} // namespace

ApplyResult apply_resolution(JsonValue& root, std::string_view pointer,
                             const std::optional<JsonValue>& value)
{
    ApplyResult result;
    if (value.has_value())
    {
        if (!compose::set_json_pointer(root, pointer, *value))
        {
            result.error = "the resolution value could not be placed at --path (malformed pointer, "
                           "out-of-range index, or a scalar mid-path)";
            return result;
        }
    }
    else
    {
        std::string error;
        if (!remove_at_pointer(root, pointer, error))
        {
            result.error = error;
            return result;
        }
    }
    result.ok = true;
    return result;
}

} // namespace context::editor::merge

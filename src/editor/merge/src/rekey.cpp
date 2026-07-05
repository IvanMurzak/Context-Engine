// Duplicate-intra-file-id diagnostic + re-key implementation (see rekey.h).

#include "context/editor/merge/rekey.h"

#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/stable_id.h"

#include "pointer_format.h"

#include <utility>

namespace context::editor::merge
{

using serializer::JsonValue;

namespace
{

const JsonValue* member(const JsonValue& v, const char* key) noexcept
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

JsonValue* member_mut(JsonValue& v, const std::string& key) noexcept
{
    if (v.type != JsonValue::Type::object)
        return nullptr;
    for (serializer::JsonMember& m : v.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// The stable `id` member string of an object (only when it has stable-id FORM — a readable
// non-stable key is not an identity), else empty.
std::string stable_id_of(const JsonValue& v)
{
    const JsonValue* id = member(v, "id");
    if (id != nullptr && id->type == JsonValue::Type::string &&
        compose::is_stable_id(id->string_value))
        return id->string_value;
    return std::string();
}

// Depth-first walk collecting (pointer -> stable id) for every object carrying a stable id.
void collect_ids(const JsonValue& node, const std::string& pointer,
                 std::vector<std::pair<std::string, std::string>>& out)
{
    if (node.type == JsonValue::Type::object)
    {
        std::string id = stable_id_of(node);
        if (!id.empty())
            out.emplace_back(pointer, std::move(id));
        for (const serializer::JsonMember& m : node.members)
            collect_ids(m.value, detail::append_token(pointer, m.key), out);
    }
    else if (node.type == JsonValue::Type::array)
    {
        for (std::size_t i = 0; i < node.elements.size(); ++i)
            collect_ids(node.elements[i], detail::append_index(pointer, i), out);
    }
}

// Count objects whose stable `id` equals `id` (used to decide reference-rewrite ambiguity).
std::uint64_t count_holders(const JsonValue& node, const std::string& id)
{
    std::uint64_t n = 0;
    if (node.type == JsonValue::Type::object)
    {
        if (stable_id_of(node) == id)
            ++n;
        for (const serializer::JsonMember& m : node.members)
            n += count_holders(m.value, id);
    }
    else if (node.type == JsonValue::Type::array)
    {
        for (const JsonValue& el : node.elements)
            n += count_holders(el, id);
    }
    return n;
}

// Rewrite every {"$entity": "<old>"} reference to new_id; returns the count rewritten. An
// entity reference is a single-member object {"$entity": <string>} (L-34, single-file id form).
std::uint64_t rewrite_entity_refs(JsonValue& node, const std::string& old_id,
                                  const std::string& new_id)
{
    std::uint64_t n = 0;
    if (node.type == JsonValue::Type::object)
    {
        JsonValue* ref = member_mut(node, "$entity");
        if (ref != nullptr && ref->type == JsonValue::Type::string && ref->string_value == old_id)
        {
            ref->string_value = new_id;
            ++n;
        }
        for (serializer::JsonMember& m : node.members)
            n += rewrite_entity_refs(m.value, old_id, new_id);
    }
    else if (node.type == JsonValue::Type::array)
    {
        for (JsonValue& el : node.elements)
            n += rewrite_entity_refs(el, old_id, new_id);
    }
    return n;
}

// Resolve an RFC 6901 pointer to a MUTABLE node (compose owns the READ resolver; the re-key write
// path needs a mutable handle). nullptr on any malformed / unresolved token.
JsonValue* resolve_mut(JsonValue& root, const std::vector<std::string>& tokens)
{
    JsonValue* cur = &root;
    for (const std::string& token : tokens)
    {
        if (cur->type == JsonValue::Type::object)
        {
            cur = member_mut(*cur, token);
            if (cur == nullptr)
                return nullptr;
        }
        else if (cur->type == JsonValue::Type::array)
        {
            // canonical base-10 index (no leading zeros except "0"; no sign)
            if (token.empty() ||
                (token.size() > 1 && token[0] == '0') ||
                token.find_first_not_of("0123456789") != std::string::npos)
                return nullptr;
            const std::size_t index = std::stoull(token);
            if (index >= cur->elements.size())
                return nullptr;
            cur = &cur->elements[index];
        }
        else
        {
            return nullptr; // a scalar mid-path
        }
    }
    return cur;
}

} // namespace

std::vector<DuplicateId> find_duplicate_ids(const JsonValue& root)
{
    std::vector<std::pair<std::string, std::string>> pointer_ids; // (pointer, id)
    collect_ids(root, "", pointer_ids);

    // Group by id, preserving first-seen id order.
    std::vector<DuplicateId> groups;
    for (const auto& [pointer, id] : pointer_ids)
    {
        DuplicateId* existing = nullptr;
        for (DuplicateId& g : groups)
            if (g.id == id)
            {
                existing = &g;
                break;
            }
        if (existing == nullptr)
        {
            DuplicateId g;
            g.id = id;
            g.pointers.push_back(pointer);
            groups.push_back(std::move(g));
        }
        else
        {
            existing->pointers.push_back(pointer);
        }
    }

    std::vector<DuplicateId> duplicates;
    for (DuplicateId& g : groups)
        if (g.pointers.size() >= 2)
            duplicates.push_back(std::move(g));
    return duplicates;
}

RekeyResult rekey_entity(JsonValue& root, std::string_view pointer, std::string new_id)
{
    RekeyResult result;

    std::vector<std::string> tokens;
    if (!compose::parse_json_pointer(pointer, tokens))
    {
        result.error = "the --at pointer is not a valid RFC 6901 JSON pointer";
        return result;
    }

    JsonValue* target = resolve_mut(root, tokens);
    if (target == nullptr || target->type != JsonValue::Type::object)
    {
        result.error = "the --at pointer does not resolve to an object";
        return result;
    }

    JsonValue* id_member = member_mut(*target, "id");
    if (id_member == nullptr || id_member->type != JsonValue::Type::string ||
        !compose::is_stable_id(id_member->string_value))
    {
        result.error = "the --at object carries no stable intra-file id to re-key";
        return result;
    }
    result.old_id = id_member->string_value;

    if (new_id.empty())
        new_id = compose::mint_stable_id();
    else if (!compose::is_stable_id(new_id))
    {
        result.error = "the requested --id is not a valid stable id (16..32 lowercase hex chars)";
        return result;
    }
    if (new_id == result.old_id)
    {
        result.error = "the new id equals the current id — nothing to re-key";
        return result;
    }

    id_member->string_value = new_id;
    result.new_id = new_id;

    // Rewrite in-file references only when the old id is now UNAMBIGUOUS (no other holder remains).
    // With a still-duplicated id the old id stays live on the remaining holder, so references keep
    // pointing at it — the correct split of a duplicate.
    if (count_holders(root, result.old_id) == 0)
        result.references_rewritten = rewrite_entity_refs(root, result.old_id, new_id);

    result.ok = true;
    return result;
}

} // namespace context::editor::merge

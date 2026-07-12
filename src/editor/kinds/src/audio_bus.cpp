// Audio-bus content-kind semantics — see audio_bus.h.

#include "context/editor/kinds/audio_bus.h"

#include "json_access.h" // shared member() / array_member() / string_member() over the serializer tree

#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::kinds
{

using serializer::JsonValue;

namespace
{
std::string ptr_index(std::string_view base, std::size_t i)
{
    return std::string(base) + "/" + std::to_string(i);
}
} // namespace

std::vector<KindDiagnostic> analyze_audio_bus(const JsonValue& doc)
{
    std::vector<KindDiagnostic> out;

    const JsonValue* buses = array_member(doc, "buses");
    if (buses == nullptr)
        return out; // missing/mistyped buses is schema::validate_document's job

    // --- collect declared bus ids + flag duplicates (in document order) ---------------------------
    std::vector<std::string> declared;
    for (std::size_t i = 0; i < buses->elements.size(); ++i)
    {
        const JsonValue* id = string_member(buses->elements[i], "id");
        if (id == nullptr)
            continue; // shape errors are schema::validate_document's job
        bool dup = false;
        for (const std::string& s : declared)
            dup = dup || s == id->string_value;
        if (dup)
            out.push_back({"audio_bus.duplicate_bus", ptr_index("/buses", i) + "/id",
                           "duplicate bus id \"" + id->string_value +
                               "\" (bus ids are unique within a graph)"});
        else
            declared.push_back(id->string_value);
    }

    const auto is_declared = [&](const std::string& name) {
        for (const std::string& s : declared)
            if (s == name)
                return true;
        return false;
    };

    // --- every `parent` must resolve to a declared bus --------------------------------------------
    for (std::size_t i = 0; i < buses->elements.size(); ++i)
    {
        const JsonValue* parent = string_member(buses->elements[i], "parent");
        if (parent == nullptr)
            continue; // no parent => top-level (master) bus
        if (!is_declared(parent->string_value))
            out.push_back({"audio_bus.parent_unknown", ptr_index("/buses", i) + "/parent",
                           "bus parent \"" + parent->string_value +
                               "\" names no declared bus"});
    }

    // --- the parent chain from each bus must be ACYCLIC -------------------------------------------
    // Walk parent -> parent from each bus; a chain longer than the bus count, or one that revisits
    // the starting bus, is a cycle. Only walks edges whose parent resolves (unknown parents already
    // reported), so a cycle is reported at its entry bus once, in document order.
    const auto parent_of = [&](const std::string& id) -> const JsonValue* {
        for (std::size_t i = 0; i < buses->elements.size(); ++i)
        {
            const JsonValue* bid = string_member(buses->elements[i], "id");
            if (bid != nullptr && bid->string_value == id)
                return string_member(buses->elements[i], "parent");
        }
        return nullptr;
    };
    for (std::size_t i = 0; i < buses->elements.size(); ++i)
    {
        const JsonValue* id = string_member(buses->elements[i], "id");
        if (id == nullptr)
            continue;
        const std::string start = id->string_value;
        std::string cursor = start;
        bool cyclic = false;
        for (std::size_t hops = 0; hops <= declared.size(); ++hops)
        {
            const JsonValue* parent = parent_of(cursor);
            if (parent == nullptr || !is_declared(parent->string_value))
                break; // reached a master bus or an unknown parent (already reported)
            if (parent->string_value == start)
            {
                cyclic = true;
                break;
            }
            cursor = parent->string_value;
        }
        if (cyclic)
            out.push_back({"audio_bus.parent_cycle", ptr_index("/buses", i) + "/parent",
                           "bus \"" + start + "\" is its own ancestor (the parent graph has a cycle)"});
    }

    return out;
}

} // namespace context::editor::kinds

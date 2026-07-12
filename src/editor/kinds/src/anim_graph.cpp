// Anim-graph content-kind semantics — see anim_graph.h.

#include "context/editor/kinds/anim_graph.h"

#include "json_access.h" // shared member() / array_member() / string_member() over the serializer tree

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

std::vector<KindDiagnostic> analyze_anim_graph(const JsonValue& doc)
{
    std::vector<KindDiagnostic> out;

    const JsonValue* states = array_member(doc, "states");
    if (states == nullptr)
        return out; // missing/mistyped states is schema::validate_document's job

    // --- collect declared state ids + flag duplicates (in document order) -------------------------
    std::vector<std::string> declared;
    for (std::size_t i = 0; i < states->elements.size(); ++i)
    {
        const JsonValue* id = string_member(states->elements[i], "id");
        if (id == nullptr)
            continue; // shape errors are schema::validate_document's job
        bool dup = false;
        for (const std::string& s : declared)
            dup = dup || s == id->string_value;
        if (dup)
            out.push_back({"anim_graph.duplicate_state", ptr_index("/states", i) + "/id",
                           "duplicate state id \"" + id->string_value +
                               "\" (state ids are unique within a graph)"});
        else
            declared.push_back(id->string_value);
    }

    const auto is_declared = [&](const std::string& name) {
        for (const std::string& s : declared)
            if (s == name)
                return true;
        return false;
    };

    // --- the initial state must resolve to a declared state ---------------------------------------
    if (const JsonValue* initial = string_member(doc, "initial"); initial != nullptr)
        if (!is_declared(initial->string_value))
            out.push_back({"anim_graph.initial_unknown", "/initial",
                           "initial state \"" + initial->string_value +
                               "\" names no declared state"});

    // --- every transition target must resolve to a declared state ---------------------------------
    for (std::size_t si = 0; si < states->elements.size(); ++si)
    {
        const JsonValue* transitions = array_member(states->elements[si], "transitions");
        if (transitions == nullptr)
            continue;
        const std::string trans_ptr = ptr_index("/states", si) + "/transitions";
        for (std::size_t ti = 0; ti < transitions->elements.size(); ++ti)
        {
            const JsonValue* to = string_member(transitions->elements[ti], "to");
            if (to == nullptr)
                continue; // shape errors are schema::validate_document's job
            if (!is_declared(to->string_value))
                out.push_back({"anim_graph.transition_unknown_target",
                               ptr_index(trans_ptr, ti) + "/to",
                               "transition target \"" + to->string_value +
                                   "\" names no declared state"});
        }
    }

    return out;
}

} // namespace context::editor::kinds

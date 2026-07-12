// Input-bindings content-kind semantics — see input_bindings.h.

#include "context/editor/kinds/input_bindings.h"

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

// Collect the declared `id`s of the array `items`, flagging duplicates under `code` at `<base>/<i>/id`.
// Returns the set of unique ids (in first-seen order) for later resolution.
std::vector<std::string> collect_ids(const JsonValue& items, std::string_view base,
                                     const char* code, std::vector<KindDiagnostic>& out)
{
    std::vector<std::string> declared;
    for (std::size_t i = 0; i < items.elements.size(); ++i)
    {
        const JsonValue* id = string_member(items.elements[i], "id");
        if (id == nullptr)
            continue; // shape errors are schema::validate_document's job
        bool dup = false;
        for (const std::string& s : declared)
            dup = dup || s == id->string_value;
        if (dup)
            out.push_back({code, ptr_index(base, i) + "/id",
                           "duplicate id \"" + id->string_value + "\""});
        else
            declared.push_back(id->string_value);
    }
    return declared;
}
} // namespace

std::vector<KindDiagnostic> analyze_input_bindings(const JsonValue& doc)
{
    std::vector<KindDiagnostic> out;

    const JsonValue* actions = array_member(doc, "actions");
    const JsonValue* contexts = array_member(doc, "contexts");
    if (actions == nullptr || contexts == nullptr)
        return out; // missing/mistyped actions|contexts is schema::validate_document's job

    // --- declared action ids (+ duplicate-action) --------------------------------------------------
    const std::vector<std::string> declared_actions =
        collect_ids(*actions, "/actions", "input_bindings.duplicate_action", out);

    // --- declared context ids (+ duplicate-context) ------------------------------------------------
    collect_ids(*contexts, "/contexts", "input_bindings.duplicate_context", out);

    const auto action_declared = [&](const std::string& name) {
        for (const std::string& s : declared_actions)
            if (s == name)
                return true;
        return false;
    };

    // --- every binding's `action` must resolve to a declared action -------------------------------
    for (std::size_t ci = 0; ci < contexts->elements.size(); ++ci)
    {
        const JsonValue* bindings = array_member(contexts->elements[ci], "bindings");
        if (bindings == nullptr)
            continue; // no bindings, or a shape error (schema's job)
        for (std::size_t bi = 0; bi < bindings->elements.size(); ++bi)
        {
            const JsonValue* action = string_member(bindings->elements[bi], "action");
            if (action == nullptr)
                continue; // shape error (schema's job)
            if (!action_declared(action->string_value))
                out.push_back({"input_bindings.binding_unknown_action",
                               ptr_index("/contexts", ci) + "/bindings/" + std::to_string(bi) +
                                   "/action",
                               "binding action \"" + action->string_value +
                                   "\" names no declared action"});
        }
    }

    return out;
}

} // namespace context::editor::kinds

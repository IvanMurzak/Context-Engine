// Wire projection of the boundary-clean panel models (see wire.h). The Shell-side parsers in
// src/editor/shell/panels/ mirror these shapes; the feed tests link both halves and assert the
// round-trip, so the two sides cannot drift silently.

#include "context/editor/gui/panels/builders/wire.h"

#include "context/editor/serializer/canonical.h"

#include <cstdint>
#include <string>

namespace context::editor::gui::panels::builders
{

// The wire-JSON module (see wire.h on why the bare `contract::` spelling is a trap here).
using wire_json::Json;

namespace
{

namespace serializer = context::editor::serializer;

// A u64 hash as a lowercase hex STRING — Json numbers are doubles, and a hash above 2^53
// would silently lose bits as a JSON number (the kernel_server rawHash discipline).
[[nodiscard]] std::string hex_of(std::uint64_t value)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    if (value == 0)
    {
        return "0";
    }
    while (value != 0)
    {
        out.insert(out.begin(), digits[value & 0xF]);
        value >>= 4;
    }
    return out;
}

// The WidgetKind wire token. MIRRORED by the Shell-side inspector feed parser
// (src/editor/shell/panels/inspector_feed.cpp) — keep the two tables in lockstep.
[[nodiscard]] const char* widget_kind_token(inspector::WidgetKind kind)
{
    switch (kind)
    {
    case inspector::WidgetKind::text:
        return "text";
    case inspector::WidgetKind::number:
        return "number";
    case inspector::WidgetKind::toggle:
        return "toggle";
    case inspector::WidgetKind::json:
        return "json";
    case inspector::WidgetKind::readonly:
        return "readonly";
    }
    return "text";
}

[[nodiscard]] Json node_to_wire(const scenetree::SceneTreeNode& node)
{
    Json out = Json::object();
    out.set("identity", Json(node.identity));
    out.set("identityHash", Json(hex_of(node.identity_hash)));
    out.set("displayName", Json(node.display_name));
    out.set("kind",
            Json(node.kind == scenetree::NodeKind::entity ? "entity" : "instance"));
    out.set("overridden", Json(node.overridden));
    Json children = Json::array();
    for (const scenetree::SceneTreeNode& child : node.children)
    {
        children.push_back(node_to_wire(child));
    }
    out.set("children", std::move(children));
    return out;
}

} // namespace

Json scene_tree_to_wire(const scenetree::SceneTreeModel& model)
{
    Json out = Json::object();
    out.set("rootScene", Json(model.root_scene));
    out.set("ok", Json(model.ok));
    out.set("entityCount", Json(static_cast<std::uint64_t>(model.entity_count)));
    Json roots = Json::array();
    for (const scenetree::SceneTreeNode& root : model.roots)
    {
        roots.push_back(node_to_wire(root));
    }
    out.set("roots", std::move(roots));
    return out;
}

Json inspector_to_wire(const inspector::InspectorModel& model)
{
    Json out = Json::object();
    out.set("present", Json(model.has_entity));
    if (!model.has_entity)
    {
        // The no-selection state carries nothing else — the panel renders its placeholder from
        // `present` alone, and serializing empty identity fields would just invite a parser to
        // treat them as meaningful.
        return out;
    }
    out.set("rootScene", Json(model.root_scene));
    Json id_path = Json::array();
    for (const std::string& segment : model.id_path)
    {
        id_path.push_back(Json(segment));
    }
    out.set("idPath", std::move(id_path));
    out.set("identity", Json(model.identity));
    out.set("identityHash", Json(hex_of(model.identity_hash)));
    out.set("kindId", Json(model.kind_id));
    out.set("overrideCount", Json(static_cast<std::uint64_t>(model.override_count)));

    Json fields = Json::array();
    for (const inspector::InspectorField& field : model.fields)
    {
        Json entry = Json::object();
        entry.set("pointer", Json(field.pointer));
        entry.set("label", Json(field.label));
        entry.set("description", Json(field.description));
        entry.set("units", Json(field.units));
        entry.set("kind", Json(widget_kind_token(field.kind)));
        // The canonical byte form (R-FILE-001) — the engine's ONE value identity, so the Shell-side
        // re-parse reconstructs an equal JsonValue by construction. serialize_canonical emits the
        // canonical FILE form, which ends in a newline; a wire VALUE is not a file, so the trailing
        // newline is trimmed (the parse side is whitespace-tolerant either way).
        std::string value;
        if (!serializer::serialize_canonical(field.value, value))
        {
            value = "null"; // unreachable: a composed value carries no non-finite number
        }
        if (!value.empty() && value.back() == '\n')
        {
            value.pop_back();
        }
        entry.set("value", Json(std::move(value)));
        entry.set("overridden", Json(field.overridden));
        entry.set("editable", Json(field.editable));
        fields.push_back(std::move(entry));
    }
    out.set("fields", std::move(fields));
    return out;
}

} // namespace context::editor::gui::panels::builders

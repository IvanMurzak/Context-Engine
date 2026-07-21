// The live inspector feed implementation (see inspector_feed.h for the design + tolerance
// rationale). The parsers mirror builders::inspector_to_wire member-for-member; the feed tests link
// both halves and assert the round-trip.

#include "context/editor/shell/panels/inspector_feed.h"

#include "context/editor/serializer/json_parse.h"
#include "context/editor/shell/panels/scenetree_feed.h" // parse_hex_u64 — the ONE hex-wire parser

#include <utility>
#include <vector>

namespace context::editor::shell::panels
{

namespace
{

namespace serializer = context::editor::serializer;

// Read an optional string member; empty when absent or not a string (each feed owns its wire
// tolerance — the problems_feed discipline).
[[nodiscard]] std::string read_string(const contract::Json& object, const std::string& key)
{
    if (!object.is_object() || !object.contains(key))
    {
        return std::string();
    }
    const contract::Json& value = object.at(key);
    return value.is_string() ? value.as_string() : std::string();
}

[[nodiscard]] bool read_bool(const contract::Json& object, const std::string& key)
{
    return object.is_object() && object.contains(key) && object.at(key).as_bool();
}

// One wire field entry -> one model field. nullopt when the entry carries no pointer (a field the
// panel could neither label nor stage an edit against). The `value` member is the field's CANONICAL
// serialization (R-FILE-001 — the engine's one value identity); an unparseable value degrades to
// null WITH the field kept readonly, so a corrupt value is visible-but-uneditable rather than
// silently editable-as-garbage.
[[nodiscard]] std::optional<inspector::InspectorField> parse_field(const contract::Json& wire)
{
    if (!wire.is_object())
    {
        return std::nullopt;
    }
    inspector::InspectorField field;
    field.pointer = read_string(wire, "pointer");
    if (field.pointer.empty())
    {
        return std::nullopt;
    }
    field.label = read_string(wire, "label");
    field.description = read_string(wire, "description");
    field.units = read_string(wire, "units");
    field.kind = parse_widget_kind(read_string(wire, "kind"));
    field.overridden = read_bool(wire, "overridden");
    field.editable = read_bool(wire, "editable");

    serializer::ParseResult parsed = serializer::parse_json(read_string(wire, "value"));
    if (parsed.ok)
    {
        field.value = std::move(parsed.root);
    }
    else
    {
        field.value = serializer::JsonValue{}; // null — and never editable as garbage:
        field.kind = inspector::WidgetKind::readonly;
        field.editable = false;
    }
    return field;
}

} // namespace

// --------------------------------------------------------------------------------- pure parsers

inspector::WidgetKind parse_widget_kind(const std::string& token)
{
    if (token == "text")
    {
        return inspector::WidgetKind::text;
    }
    if (token == "number")
    {
        return inspector::WidgetKind::number;
    }
    if (token == "toggle")
    {
        return inspector::WidgetKind::toggle;
    }
    if (token == "json")
    {
        return inspector::WidgetKind::json;
    }
    // "readonly" and every unknown future token: visible, never editable (fail-closed).
    return inspector::WidgetKind::readonly;
}

std::optional<inspector::InspectorModel> parse_inspector(const contract::Json& wire)
{
    if (!wire.is_object() || !wire.contains("present"))
    {
        return std::nullopt; // says nothing about a selection — not the same as "no selection"
    }
    inspector::InspectorModel model;
    if (!wire.at("present").as_bool())
    {
        return model; // the engaged no-selection state (has_entity == false)
    }
    model.has_entity = true;
    model.root_scene = read_string(wire, "rootScene");
    model.identity = read_string(wire, "identity");
    model.identity_hash = parse_hex_u64(read_string(wire, "identityHash"));
    model.kind_id = read_string(wire, "kindId");
    if (wire.contains("idPath") && wire.at("idPath").is_array())
    {
        const contract::Json& id_path = wire.at("idPath");
        for (std::size_t i = 0; i < id_path.size(); ++i)
        {
            if (id_path.at(i).is_string())
            {
                model.id_path.push_back(id_path.at(i).as_string());
            }
        }
    }
    if (wire.contains("fields") && wire.at("fields").is_array())
    {
        const contract::Json& fields = wire.at("fields");
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            if (std::optional<inspector::InspectorField> field = parse_field(fields.at(i)))
            {
                if (field->overridden)
                {
                    ++model.override_count;
                }
                model.fields.push_back(std::move(*field));
            }
        }
    }
    return model;
}

std::uint64_t parse_raw_hash(const std::string& text)
{
    if (text.empty() || text.size() > 20)
    {
        return 0; // a u64 has at most 20 decimal digits; refuse rather than wrap
    }
    std::uint64_t out = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return 0;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (out > (static_cast<std::uint64_t>(-1) - digit) / 10)
        {
            return 0; // overflow -> not a u64 the daemon minted
        }
        out = out * 10 + digit;
    }
    return out;
}

std::optional<std::string> inspector_widget_pointer(const std::string& node_id)
{
    const std::string prefix = kInspectorWidgetPrefix;
    if (node_id.size() <= prefix.size() || node_id.compare(0, prefix.size(), prefix) != 0)
    {
        return std::nullopt;
    }
    return node_id.substr(prefix.size());
}

// ------------------------------------------------------------------------------------ the feed

InspectorFeed::InspectorFeed(PanelHost& host, std::string panel_id)
    : host_(host), panel_id_(std::move(panel_id))
{
}

void InspectorFeed::request(const std::string& identity)
{
    pending_ = identity;
}

void InspectorFeed::request_clear()
{
    pending_.reset();
    panel_.clear();
    host_.touch(panel_id_);
}

bool InspectorFeed::apply_result(const contract::Json& reply)
{
    // Envelope tolerance (mirrors SceneTreeFeed::apply_result): the rawHash rides the DATA level,
    // sibling of `inspector`, so resolve data first and read both from there.
    const contract::Json* data = &reply;
    if (data->is_object() && data->contains("data") && data->at("data").is_object())
    {
        data = &data->at("data");
    }
    const contract::Json* wire = data;
    if (wire->is_object() && wire->contains("inspector") && wire->at("inspector").is_object())
    {
        wire = &wire->at("inspector");
    }
    std::optional<inspector::InspectorModel> model = parse_inspector(*wire);
    if (!model.has_value())
    {
        return false;
    }
    panel_.set_model(std::move(*model), parse_raw_hash(read_string(*data, "rawHash")));
    ++results_applied_;
    host_.touch(panel_id_);
    return true;
}

PanelProvider InspectorFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return panel_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json& params)
    {
        if (command_id != inspector::InspectorPanel::kEditCommand)
        {
            return false;
        }
        const std::optional<std::string> pointer =
            inspector_widget_pointer(read_string(params, "nodeId"));
        if (!pointer.has_value())
        {
            return false;
        }
        // The edit VALUE arrives as a JSON literal string (`"1.5"`, `"\"name\""`, `"true"`). A
        // dispatch with no parseable value is DECLINED — there is nothing honest to stage, and
        // stage_edit's own field/editable checks still apply to the rest.
        serializer::ParseResult value = serializer::parse_json(read_string(params, "value"));
        if (!value.ok)
        {
            return false;
        }
        return panel_.stage_edit(*pointer, std::move(value.root));
    };
    return provider;
}

} // namespace context::editor::shell::panels

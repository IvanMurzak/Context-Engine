// The live inspector feed implementation (see inspector_feed.h for the design + tolerance
// rationale). The parsers mirror builders::inspector_to_wire member-for-member; the feed tests link
// both halves and assert the round-trip.

#include "context/editor/shell/panels/inspector_feed.h"

#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/sidecar_ref.h"      // parse_hash_string — the decimal-u64 inverse
#include "context/editor/shell/panels/scenetree_feed.h" // parse_hex_u64 — the ONE hex-wire parser
#include "wire_read.h"                                  // read_string / read_bool

#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell::panels
{

namespace
{

namespace serializer = context::editor::serializer;

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
    // FAIL-CLOSED ACROSS BOTH MEMBERS: an unknown kind token parses to `readonly` (see
    // parse_widget_kind), and a readonly widget must not stay editable just because the wire's
    // INDEPENDENT `editable` bit said so — InspectorPanel::stage_edit gates on `editable` alone,
    // so without this clamp a future/hostile token would be visible AND editable through a widget
    // this build cannot render honestly. The real builder never emits readonly+editable
    // (inspector_builder.cpp derives editable from the kind), so round-trips are unchanged.
    if (field.kind == inspector::WidgetKind::readonly)
    {
        field.editable = false;
    }

    // Parse the canonical value straight off the wire node — as_string() is a reference, so the
    // (arbitrarily large for json-kind fields) canonical serialization is never copied first.
    serializer::ParseResult parsed = serializer::parse_json(wire.at("value").as_string());
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
    const contract::Json& id_path = wire.at("idPath"); // at() is total: null when absent
    if (id_path.is_array())
    {
        for (std::size_t i = 0; i < id_path.size(); ++i)
        {
            if (id_path.at(i).is_string())
            {
                model.id_path.push_back(id_path.at(i).as_string());
            }
        }
    }
    const contract::Json& fields = wire.at("fields");
    if (fields.is_array())
    {
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
    // serializer::parse_hash_string is THE strict inverse of the daemon's decimal hash form
    // (exactly the strings std::to_string(std::uint64_t) — kernel_server's hash_string — produces);
    // any refusal degrades to 0, the model's honest "no CAS token".
    return serializer::parse_hash_string(text).value_or(0);
}

std::optional<std::string> inspector_widget_pointer(const std::string& node_id)
{
    constexpr std::string_view prefix = kInspectorWidgetPrefix;
    if (node_id.size() <= prefix.size() || !std::string_view(node_id).starts_with(prefix))
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
    // sibling of `inspector`, so resolve data first and read both from there. at() is total (a
    // shared null when absent), so each hop reads its member once.
    const contract::Json* data = &reply;
    const contract::Json& nested_data = reply.at("data");
    if (nested_data.is_object())
    {
        data = &nested_data;
    }
    const contract::Json* wire = data;
    const contract::Json& nested_inspector = data->at("inspector");
    if (nested_inspector.is_object())
    {
        wire = &nested_inspector;
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
        serializer::ParseResult value = serializer::parse_json(params.at("value").as_string());
        if (!value.ok)
        {
            return false;
        }
        return panel_.stage_edit(*pointer, std::move(value.root));
    };
    return provider;
}

} // namespace context::editor::shell::panels

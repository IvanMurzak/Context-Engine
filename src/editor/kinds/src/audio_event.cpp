// Audio-event content-kind semantics — see audio_event.h.

#include "context/editor/kinds/audio_event.h"

#include "json_access.h" // shared member() over the serializer tree

#include <optional>
#include <vector>

namespace context::editor::kinds
{

using serializer::JsonValue;

namespace
{
// A JSON number as a double, accepting the three numeric domains the serializer distinguishes
// (integer / unsigned_integer / number). nullopt when absent or not numeric.
[[nodiscard]] std::optional<double> number_member(const JsonValue& object, std::string_view key)
{
    const JsonValue* v = member(object, key);
    if (v == nullptr)
        return std::nullopt;
    switch (v->type)
    {
    case JsonValue::Type::integer:
        return static_cast<double>(v->int_value);
    case JsonValue::Type::unsigned_integer:
        return static_cast<double>(v->uint_value);
    case JsonValue::Type::number:
        return v->number_value;
    default:
        return std::nullopt;
    }
}
} // namespace

std::vector<KindDiagnostic> analyze_audio_event(const JsonValue& doc)
{
    std::vector<KindDiagnostic> out;

    const JsonValue* spatial = member(doc, "spatial");
    if (spatial == nullptr || spatial->type != JsonValue::Type::object)
        return out; // non-spatial event, or a shape error schema::validate_document reports

    const std::optional<double> min_distance = number_member(*spatial, "minDistance");
    const std::optional<double> max_distance = number_member(*spatial, "maxDistance");
    if (!min_distance.has_value() || !max_distance.has_value())
        return out; // missing/mistyped distances are schema::validate_document's job

    if (*min_distance < 0.0 || *max_distance < 0.0)
        out.push_back({"audio_event.invalid_attenuation", "/spatial",
                       "spatialization distances must be non-negative"});
    else if (*max_distance <= *min_distance)
        out.push_back({"audio_event.invalid_attenuation", "/spatial/maxDistance",
                       "maxDistance must be strictly greater than minDistance "
                       "(the attenuation range is inverted or degenerate)"});

    return out;
}

} // namespace context::editor::kinds

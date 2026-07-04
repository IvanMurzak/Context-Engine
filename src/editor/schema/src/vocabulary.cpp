// The R-DATA-006 vocabulary implementation — see vocabulary.h for the pinned shapes.

#include "context/editor/schema/vocabulary.h"

#include "context/editor/schema/json_access.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace context::editor::schema
{

using serializer::JsonValue;

namespace
{

constexpr std::array<std::string_view, 5> kSemanticIds = {"quaternion", "color", "curve",
                                                          "gradient", "bit-flags"};

// The pinned SI vocabulary (see vocabulary.h — the units LAW; never a conversion switch).
constexpr std::array<std::string_view, 16> kSiUnits = {
    "1",   "m",     "kg",    "s", "rad", "sr", "hz",     "m/s",
    "m/s^2", "rad/s", "rad/s^2", "n", "j",   "w",  "pa", "kg/m^3"};

constexpr std::array<std::string_view, 10> kStorageBases = {"f32", "f64", "i8", "i16", "i32",
                                                            "i64", "u8",  "u16", "u32", "u64"};

constexpr std::array<std::string_view, 4> kColorSpaces = {"srgb", "srgb-linear", "display-p3",
                                                          "rec2020"};

// [a-z][a-z0-9_-]* — the tag-segment grammar of the pinned union convention.
[[nodiscard]] bool is_tag_segment(std::string_view segment) noexcept
{
    if (segment.empty())
        return false;
    if (segment.front() < 'a' || segment.front() > 'z')
        return false;
    return std::all_of(segment.begin(), segment.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

void check_color_into(const JsonValue& value, const std::string& base,
                      std::vector<SemanticIssue>& issues)
{
    if (value.type != JsonValue::Type::object)
    {
        issues.push_back({base, "a color is an object {\"space\": ..., \"value\": [...]}"});
        return;
    }
    const JsonValue* space = find_member(value, "space");
    if (space == nullptr)
        issues.push_back({base, "a color must DECLARE its color space (R-DATA-006)"});
    else if (space->type != JsonValue::Type::string || !is_color_space(space->string_value))
        issues.push_back({base + "/space",
                          "unknown color space (pinned: srgb, srgb-linear, display-p3, rec2020)"});
    const JsonValue* components = find_member(value, "value");
    if (components == nullptr || components->type != JsonValue::Type::array ||
        (components->elements.size() != 3 && components->elements.size() != 4))
    {
        issues.push_back({base + "/value", "a color's value is 3 (RGB) or 4 (RGBA) numbers"});
        return;
    }
    for (std::size_t i = 0; i < components->elements.size(); ++i)
        if (!is_finite_number(components->elements[i]))
            issues.push_back(
                {base + "/value/" + std::to_string(i), "color components are finite numbers"});
}

void check_quaternion(const JsonValue& value, std::vector<SemanticIssue>& issues)
{
    if (value.type != JsonValue::Type::array || value.elements.size() != 4)
    {
        issues.push_back({"", "a quaternion is exactly [x, y, z, w] (4 numbers)"});
        return;
    }
    for (std::size_t i = 0; i < value.elements.size(); ++i)
        if (!is_finite_number(value.elements[i]))
            issues.push_back({"/" + std::to_string(i), "quaternion components are finite numbers"});
}

void check_curve(const JsonValue& value, std::vector<SemanticIssue>& issues)
{
    const JsonValue* keys = value.type == JsonValue::Type::object ? find_member(value, "keys")
                                                                  : nullptr;
    if (keys == nullptr || keys->type != JsonValue::Type::array || keys->elements.empty())
    {
        issues.push_back({"", "a curve is {\"keys\": [{\"t\", \"v\"}, ...]} with >= 1 key"});
        return;
    }
    double previous_t = 0.0;
    for (std::size_t i = 0; i < keys->elements.size(); ++i)
    {
        const std::string base = "/keys/" + std::to_string(i);
        const JsonValue& key = keys->elements[i];
        const JsonValue* t = find_member(key, "t");
        const JsonValue* v = find_member(key, "v");
        if (key.type != JsonValue::Type::object || t == nullptr || v == nullptr ||
            !is_finite_number(*t) || !is_finite_number(*v))
        {
            issues.push_back({base, "a curve key is {\"t\": <number>, \"v\": <number>}"});
            continue;
        }
        const double current_t = as_double(*t);
        if (i > 0 && current_t <= previous_t)
            issues.push_back({base + "/t", "curve key times must be strictly increasing"});
        previous_t = current_t;
    }
}

void check_gradient(const JsonValue& value, std::vector<SemanticIssue>& issues)
{
    const JsonValue* stops = value.type == JsonValue::Type::object ? find_member(value, "stops")
                                                                   : nullptr;
    if (stops == nullptr || stops->type != JsonValue::Type::array || stops->elements.empty())
    {
        issues.push_back({"", "a gradient is {\"stops\": [{\"t\", \"color\"}, ...]} with >= 1 stop"});
        return;
    }
    double previous_t = 0.0;
    for (std::size_t i = 0; i < stops->elements.size(); ++i)
    {
        const std::string base = "/stops/" + std::to_string(i);
        const JsonValue& stop = stops->elements[i];
        const JsonValue* t = find_member(stop, "t");
        const JsonValue* color = find_member(stop, "color");
        if (stop.type != JsonValue::Type::object || t == nullptr || color == nullptr ||
            !is_finite_number(*t))
        {
            issues.push_back({base, "a gradient stop is {\"t\": <0..1>, \"color\": <color>}"});
            continue;
        }
        const double current_t = as_double(*t);
        if (current_t < 0.0 || current_t > 1.0)
            issues.push_back({base + "/t", "gradient stop times lie within [0, 1]"});
        else if (i > 0 && current_t < previous_t)
            issues.push_back({base + "/t", "gradient stop times must be non-decreasing"});
        previous_t = current_t;
        check_color_into(*color, base + "/color", issues);
    }
}

void check_bit_flags(const JsonValue& value, std::vector<SemanticIssue>& issues)
{
    if (value.type != JsonValue::Type::array)
    {
        issues.push_back({"", "bit-flags are an array of unique flag-name strings"});
        return;
    }
    for (std::size_t i = 0; i < value.elements.size(); ++i)
    {
        const JsonValue& flag = value.elements[i];
        const std::string base = "/" + std::to_string(i);
        if (flag.type != JsonValue::Type::string || flag.string_value.empty())
        {
            issues.push_back({base, "a flag name is a non-empty string"});
            continue;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (value.elements[j].type == JsonValue::Type::string &&
                value.elements[j].string_value == flag.string_value)
            {
                issues.push_back({base, "duplicate flag name (bit-flags are a set)"});
                break;
            }
    }
}

} // namespace

std::string_view semantic_type_id(SemanticType type) noexcept
{
    return kSemanticIds[static_cast<std::size_t>(type)];
}

bool is_semantic_type_id(std::string_view id) noexcept
{
    return std::find(kSemanticIds.begin(), kSemanticIds.end(), id) != kSemanticIds.end();
}

SemanticType semantic_type_from_id(std::string_view id) noexcept
{
    const auto it = std::find(kSemanticIds.begin(), kSemanticIds.end(), id);
    return static_cast<SemanticType>(it - kSemanticIds.begin());
}

std::vector<SemanticIssue> check_semantic(SemanticType type, const JsonValue& value)
{
    std::vector<SemanticIssue> issues;
    switch (type)
    {
    case SemanticType::quaternion:
        check_quaternion(value, issues);
        break;
    case SemanticType::color:
        check_color_into(value, "", issues);
        break;
    case SemanticType::curve:
        check_curve(value, issues);
        break;
    case SemanticType::gradient:
        check_gradient(value, issues);
        break;
    case SemanticType::bit_flags:
        check_bit_flags(value, issues);
        break;
    }
    return issues;
}

bool is_si_unit(std::string_view unit) noexcept
{
    return std::find(kSiUnits.begin(), kSiUnits.end(), unit) != kSiUnits.end();
}

bool is_storage_layout(std::string_view layout) noexcept
{
    std::string_view base = layout;
    if (const std::size_t x = layout.rfind('x'); x != std::string_view::npos)
    {
        const std::string_view lanes = layout.substr(x + 1);
        // Only "<base>x<lanes>" splits here; a bare base ("f32") has its 'x' at position 0 of no
        // lane suffix, so an empty/invalid lane set falls through to the bare-base check below.
        if (lanes == "2" || lanes == "3" || lanes == "4" || lanes == "9" || lanes == "16")
            base = layout.substr(0, x);
    }
    return std::find(kStorageBases.begin(), kStorageBases.end(), base) != kStorageBases.end();
}

bool is_union_tag(std::string_view tag) noexcept
{
    const std::size_t colon = tag.find(':');
    if (colon == std::string_view::npos)
        return false;
    return is_tag_segment(tag.substr(0, colon)) && is_tag_segment(tag.substr(colon + 1)) &&
           tag.find(':', colon + 1) == std::string_view::npos;
}

bool is_color_space(std::string_view space) noexcept
{
    return std::find(kColorSpaces.begin(), kColorSpaces.end(), space) != kColorSpaces.end();
}

bool is_valid_notes(const JsonValue& value) noexcept
{
    if (value.type == JsonValue::Type::string)
        return true;
    if (value.type != JsonValue::Type::array)
        return false;
    return std::all_of(value.elements.begin(), value.elements.end(), [](const JsonValue& e) {
        return e.type == JsonValue::Type::string;
    });
}

} // namespace context::editor::schema

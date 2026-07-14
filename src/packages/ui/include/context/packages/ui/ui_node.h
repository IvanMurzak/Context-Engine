// The runtime UI node + its presentation primitives (M7 T1, R-UI-002/006, D2). SOURCE OF TRUTH for the
// closed runtime role vocabulary, the small CSS-like style-prop set, and the geometry POD types the
// tree/damage/provider layers share.
//
// This is a NEW runtime tree, borrowing the editor context_gui_uitree DESIGN (a closed role vocabulary
// so R-A11Y-002 can hook it post-core) with ZERO link-level sharing (D2): the editor tree is
// editor-scoped and ships only with the editor; this one ships in exported games and grows
// layout/events/damage. UI is presentation (D6) — every field here is float/observer state, never
// hashed sim state.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace context::packages::ui
{

// A node handle: an index into the tree's node store. Stable for a node's lifetime (removed slots are
// tombstoned, never reused, so a handle never silently repoints). kInvalidNode is the null handle.
using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

// The CLOSED runtime role vocabulary (D2). Kept small + closed so a binding's role is validated and so
// the post-core R-A11Y-002 accessibility package can map each role to an ARIA/semantic role without the
// tree admitting arbitrary strings. Extend deliberately, never open-endedly.
enum class Role
{
    Root,
    Panel,
    Group,
    Label,
    Button,
    Image,
    Slider,
    Checkbox,
    TextInput,
    ProgressBar,
    List,
    ListItem
};

// A stable lowercase name for a role (introspection / a11y / CLI dump). Exhaustive over Role; the
// trailing return keeps -Wreturn-type quiet without a default that would hide a missing case.
[[nodiscard]] inline const char* role_name(Role role) noexcept
{
    switch (role)
    {
    case Role::Root:
        return "root";
    case Role::Panel:
        return "panel";
    case Role::Group:
        return "group";
    case Role::Label:
        return "label";
    case Role::Button:
        return "button";
    case Role::Image:
        return "image";
    case Role::Slider:
        return "slider";
    case Role::Checkbox:
        return "checkbox";
    case Role::TextInput:
        return "textinput";
    case Role::ProgressBar:
        return "progressbar";
    case Role::List:
        return "list";
    case Role::ListItem:
        return "listitem";
    }
    return "unknown";
}

// A 2D point / size (presentation floats, D6 — never hashed).
struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

// An 8-bit-per-channel RGBA color (presentation).
struct Color
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;

    [[nodiscard]] bool operator==(const Color& o) const noexcept
    {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    [[nodiscard]] bool operator!=(const Color& o) const noexcept { return !(*this == o); }
};

// A presentation transform (GPU-composited by capable backends — R-UI-005 composited_transforms — so
// changing it need not force a relayout). translate in local units, scale multiplicative, rotation in
// radians.
struct Transform
{
    Vec2 translate;
    Vec2 scale{1.0f, 1.0f};
    float rotation = 0.0f;
};

// The small, CLOSED CSS-like style-prop set (scope-creep magnet — kept deliberately minimal at T1).
// visible/opacity/transform are the R-UI-005-adjacent presentation controls; background/foreground/
// padding round out a first useful set. Richer styling is intentionally deferred.
struct Style
{
    bool visible = true;
    float opacity = 1.0f;
    Transform transform;
    Color background{0, 0, 0, 0};
    Color foreground{255, 255, 255, 255};
    float padding = 0.0f;
};

// An axis-aligned rectangle in the surface coordinate space — a node's computed bounds (layout fills it
// in T2; settable now) and the unit of damage. w/h <= 0 is an empty rect.
struct Rect
{
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] bool empty() const noexcept { return w <= 0.0f || h <= 0.0f; }

    // Half-open overlap test (touching edges do NOT intersect).
    [[nodiscard]] bool intersects(const Rect& o) const noexcept
    {
        if (empty() || o.empty())
            return false;
        return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
    }

    // The bounding box covering both rects (an empty operand yields the other).
    [[nodiscard]] Rect unite(const Rect& o) const noexcept
    {
        if (empty())
            return o;
        if (o.empty())
            return *this;
        const float x0 = std::min(x, o.x);
        const float y0 = std::min(y, o.y);
        const float x1 = std::max(x + w, o.x + o.w);
        const float y1 = std::max(y + h, o.y + o.h);
        return Rect{x0, y0, x1 - x0, y1 - y0};
    }
};

// A retained UI node: role + presentation style + optional text/name + computed bounds + tree links.
// `name` is the stable author-facing id (data-binding key / a11y label); `alive` tombstones a removed
// node so its NodeId never repoints.
struct UiNode
{
    NodeId id = kInvalidNode;
    NodeId parent = kInvalidNode;
    std::vector<NodeId> children;
    Role role = Role::Group;
    Style style;
    std::string name;
    std::string text;
    Rect bounds;
    bool alive = true;
};

} // namespace context::packages::ui

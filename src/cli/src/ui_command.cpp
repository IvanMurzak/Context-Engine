// `context ui <verb>` implementation (see ui_command.h).

#include "context/cli/ui_command.h"

#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "context/packages/ui/errors.h"
#include "context/packages/ui/events.h"
#include "context/packages/ui/introspect.h"
#include "context/packages/ui/layout.h"
#include "context/packages/ui/ui_node.h"
#include "context/packages/ui/ui_tree.h"

#include <cmath>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace ui = context::packages::ui;

namespace
{
// --- small helpers --------------------------------------------------------------------------------

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const std::string* flag(const std::map<std::string, std::string>& flags, const char* name)
{
    auto it = flags.find(name);
    return it != flags.end() ? &it->second : nullptr;
}

// A viewport dimension flag: an unsigned integer, defaulting to `fallback`. A malformed value is
// treated as absent (falls back) rather than failing the whole verb — the layout viewport is advisory.
float viewport_dim(const std::map<std::string, std::string>& flags, const char* name, float fallback)
{
    const std::string* raw = flag(flags, name);
    if (raw == nullptr)
        return fallback;
    try
    {
        std::size_t pos = 0;
        const long long v = std::stoll(*raw, &pos, 10);
        if (pos != raw->size() || v <= 0)
            return fallback;
        return static_cast<float>(v);
    }
    catch (...)
    {
        return fallback;
    }
}

std::optional<double> parse_double(const std::string& s)
{
    try
    {
        std::size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos != s.size())
            return std::nullopt;
        return v;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// Clamp a JSON number (a 0..255 color channel) into a byte.
std::uint8_t to_channel(double raw)
{
    if (raw < 0.0)
        raw = 0.0;
    if (raw > 255.0)
        raw = 255.0;
    return static_cast<std::uint8_t>(raw + 0.5);
}

// Read a [r,g,b,a] color array from `style[key]` into `out` when present + well-formed (4+ numbers).
void read_color(const Json& style, const char* key, ui::Color& out)
{
    const Json& c = style.at(key);
    if (c.is_array() && c.size() >= 4)
        out = ui::Color{to_channel(c.at(0).as_number()), to_channel(c.at(1).as_number()),
                        to_channel(c.at(2).as_number()), to_channel(c.at(3).as_number())};
}

// Map an event name (as authored in a scene `on` block, or the `ui send` event arg) to an EventType.
// A `click` is a pointer-down in this model. Returns false for an unknown name.
bool parse_event_type(const std::string& name, ui::EventType& out)
{
    if (name == "click" || name == "pointerdown")
        out = ui::EventType::PointerDown;
    else if (name == "pointerup")
        out = ui::EventType::PointerUp;
    else if (name == "pointermove")
        out = ui::EventType::PointerMove;
    else if (name == "pointerenter")
        out = ui::EventType::PointerEnter;
    else if (name == "pointerleave")
        out = ui::EventType::PointerLeave;
    else if (name == "focus" || name == "focusgained")
        out = ui::EventType::FocusGained;
    else if (name == "blur" || name == "focuslost")
        out = ui::EventType::FocusLost;
    else if (name == "key" || name == "keydown")
        out = ui::EventType::KeyDown;
    else if (name == "keyup")
        out = ui::EventType::KeyUp;
    else if (name == "custom")
        out = ui::EventType::Custom;
    else
        return false;
    return true;
}

// --- the loaded scene -----------------------------------------------------------------------------

// One authored `on` action: a write onto the numeric state store. `add-state` accumulates `amount`
// onto the named state; `set-state` overwrites it. The doubles-only UI->state action path (D6).
struct Action
{
    bool overwrite = false; // true => set-state; false => add-state
    std::string state;
    double amount = 0.0;
};

// The headless scene the verbs operate on: the retained tree, the numeric state store (keyed by
// author state NAME — the CLI models the ctx:ui-hud state[] this way), and each bound node's state
// name. The state store is a shared_ptr so event handlers can capture it by value (surviving any move
// of the LoadedScene). UI is presentation (D6): the state store folds into no sim state hash.
struct LoadedScene
{
    ui::UiTree tree;
    std::shared_ptr<std::map<std::string, double>> state =
        std::make_shared<std::map<std::string, double>>();
    std::map<ui::NodeId, std::string> bound; // node -> bound state name
};

// A load outcome: either the built scene (via the caller's out-param) or a (code, message) failure.
struct LoadError
{
    std::string code;
    std::string message;
};

// Apply a node's authored `style` block onto the tree node.
void apply_style(ui::UiTree& tree, ui::NodeId id, const Json& style)
{
    ui::Style s = tree.node(id)->style;
    if (style.contains("visible") && style.at("visible").is_bool())
        s.visible = style.at("visible").as_bool();
    if (style.contains("opacity") && style.at("opacity").is_number())
        s.opacity = static_cast<float>(style.at("opacity").as_number());
    if (style.contains("padding") && style.at("padding").is_number())
        s.padding = static_cast<float>(style.at("padding").as_number());
    read_color(style, "background", s.background);
    read_color(style, "foreground", s.foreground);
    tree.set_style(id, s);
}

// Apply a node's authored `layout` block (the flex-lite input model) onto the tree node.
void apply_layout(ui::UiTree& tree, ui::NodeId id, const Json& layout)
{
    ui::Layout l = tree.node(id)->layout;
    if (layout.at("position").as_string() == "absolute")
        l.position = ui::Positioning::Absolute;
    else if (layout.at("position").is_string())
        l.position = ui::Positioning::Flow;
    const std::string flow = layout.at("flow").as_string();
    if (flow == "row")
        l.flow = ui::Flow::Row;
    else if (flow == "column")
        l.flow = ui::Flow::Column;
    else if (layout.contains("flow"))
        l.flow = ui::Flow::None;
    if (layout.at("size").is_array() && layout.at("size").size() >= 2)
        l.size = ui::Vec2{static_cast<float>(layout.at("size").at(0).as_number()),
                          static_cast<float>(layout.at("size").at(1).as_number())};
    if (layout.at("gap").is_number())
        l.gap = static_cast<float>(layout.at("gap").as_number());
    if (layout.at("offset").is_array() && layout.at("offset").size() >= 2)
        l.offset = ui::Vec2{static_cast<float>(layout.at("offset").at(0).as_number()),
                            static_cast<float>(layout.at("offset").at(1).as_number())};
    const Json& anchor = layout.at("anchor");
    if (anchor.is_array())
    {
        ui::Anchor a = ui::Anchor::None;
        for (std::size_t i = 0; i < anchor.size(); ++i)
        {
            const std::string& edge = anchor.at(i).as_string();
            if (edge == "left")
                a = a | ui::Anchor::Left;
            else if (edge == "top")
                a = a | ui::Anchor::Top;
            else if (edge == "right")
                a = a | ui::Anchor::Right;
            else if (edge == "bottom")
                a = a | ui::Anchor::Bottom;
        }
        l.anchor = a;
    }
    tree.set_layout(id, l);
}

// Recursively build `node_json` (and its subtree) under `parent`. Returns false + fills `err` on a
// malformed node (missing/unknown role, or a bad `on` block).
bool build_node(const Json& node_json, ui::NodeId parent, LoadedScene& scene, LoadError& err)
{
    if (!node_json.is_object() || !node_json.contains("role"))
    {
        err = {ui::kSceneInvalidCode, "a scene node is not an object or is missing its `role`"};
        return false;
    }
    ui::Role role = ui::Role::Group;
    if (!ui::role_from_name(node_json.at("role").as_string(), role))
    {
        err = {ui::kSceneInvalidCode,
               "unknown node role '" + node_json.at("role").as_string() + "'"};
        return false;
    }
    const ui::NodeId id = scene.tree.create_node(role, parent);
    if (id == ui::kInvalidNode)
    {
        err = {ui::kSceneInvalidCode, "could not create a node (invalid parent)"};
        return false;
    }
    if (node_json.contains("name"))
        scene.tree.node(id)->name = node_json.at("name").as_string();
    if (node_json.contains("text"))
        scene.tree.set_text(id, node_json.at("text").as_string());
    if (node_json.contains("style"))
        apply_style(scene.tree, id, node_json.at("style"));
    if (node_json.contains("layout"))
        apply_layout(scene.tree, id, node_json.at("layout"));

    // Read-only data binding: bind this node's displayed value to a named numeric state.
    if (node_json.contains("bind"))
    {
        const Json& bind = node_json.at("bind");
        if (bind.contains("value") && bind.at("value").is_string())
            scene.bound[id] = bind.at("value").as_string();
    }

    // Event handlers: each authored event name maps to a list of state-mutating actions; register a
    // real tree handler that applies them on dispatch (the UI->state action path).
    if (node_json.contains("on"))
    {
        const Json& on = node_json.at("on");
        if (on.is_object())
        {
            for (const auto& [event_name, action_list] : on.object_members())
            {
                ui::EventType type = ui::EventType::Custom;
                if (!parse_event_type(event_name, type))
                {
                    err = {ui::kSceneInvalidCode, "unknown event kind in `on`: '" + event_name + "'"};
                    return false;
                }
                std::vector<Action> actions;
                for (std::size_t i = 0; i < action_list.size(); ++i)
                {
                    const Json& a = action_list.at(i);
                    Action action;
                    action.state = a.at("state").as_string();
                    if (a.contains("value"))
                    {
                        action.overwrite = true;
                        action.amount = a.at("value").as_number();
                    }
                    else
                    {
                        action.amount = a.at("delta").as_number();
                    }
                    actions.push_back(action);
                }
                auto state_ptr = scene.state;
                scene.tree.add_handler(id, type, [actions, state_ptr](ui::Event&) {
                    for (const Action& a : actions)
                    {
                        if (a.overwrite)
                            (*state_ptr)[a.state] = a.amount;
                        else
                            (*state_ptr)[a.state] += a.amount;
                    }
                });
            }
        }
    }

    if (node_json.contains("children"))
    {
        const Json& children = node_json.at("children");
        for (std::size_t i = 0; i < children.size(); ++i)
            if (!build_node(children.at(i), id, scene, err))
                return false;
    }
    return true;
}

// Load a UI-scene file into `scene` and run the headless layout pass over `viewport`. Returns
// std::nullopt on success, or the failure (code, message) to surface.
std::optional<LoadError> load_scene(const std::string& scene_path, const ui::Rect& viewport,
                                    LoadedScene& scene)
{
    const std::optional<std::string> bytes = read_file(scene_path);
    if (!bytes.has_value())
        return LoadError{ui::kSceneNotFoundCode, "no UI-scene file at '" + scene_path + "'"};

    Json doc;
    try
    {
        doc = Json::parse(*bytes);
    }
    catch (const std::exception& e)
    {
        return LoadError{ui::kSceneInvalidCode,
                         "UI-scene is not well-formed JSON: " + std::string(e.what())};
    }
    if (!doc.is_object() || !doc.contains("root"))
        return LoadError{ui::kSceneInvalidCode,
                         "UI-scene has no `root` node (expected a ctx:ui-hud document)"};

    // Seed the numeric state store from state[] (each entry's `name` -> `initial`).
    if (doc.contains("state") && doc.at("state").is_array())
    {
        const Json& states = doc.at("state");
        for (std::size_t i = 0; i < states.size(); ++i)
        {
            const Json& s = states.at(i);
            if (!s.contains("name") || !s.at("name").is_string())
                continue;
            const double initial = s.contains("initial") ? s.at("initial").as_number() : 0.0;
            (*scene.state)[s.at("name").as_string()] = initial;
        }
    }

    // Build the authored root under the tree's surface Root (id 0), then lay it out.
    LoadError err;
    if (!build_node(doc.at("root"), scene.tree.root(), scene, err))
        return err;
    ui::compute_layout(scene.tree, viewport);
    return std::nullopt;
}

// --- serialization --------------------------------------------------------------------------------

// The resolved data-bound value of a node, or std::nullopt when it carries no binding.
std::optional<double> bound_value(const LoadedScene& scene, ui::NodeId id)
{
    auto it = scene.bound.find(id);
    if (it == scene.bound.end())
        return std::nullopt;
    auto sit = scene.state->find(it->second);
    return sit != scene.state->end() ? sit->second : 0.0;
}

std::size_t live_child_count(const ui::UiTree& tree, ui::NodeId id)
{
    const ui::UiNode* n = tree.node(id);
    if (n == nullptr)
        return 0;
    std::size_t count = 0;
    for (ui::NodeId child : n->children)
    {
        const ui::UiNode* c = tree.node(child);
        if (c != nullptr && c->alive)
            ++count;
    }
    return count;
}

// One node as an introspection JSON object: id, role, name/text (when set), parent, rect, visibility,
// child count, and the resolved data-bound value (when bound).
Json node_json(const LoadedScene& scene, ui::NodeId id)
{
    const ui::UiNode* n = scene.tree.node(id);
    Json out = Json::object();
    out.set("id", Json(static_cast<std::uint64_t>(id)));
    out.set("role", Json(std::string(ui::role_name(n->role))));
    if (!n->name.empty())
        out.set("name", Json(n->name));
    if (!n->text.empty())
        out.set("text", Json(n->text));
    if (n->parent == ui::kInvalidNode)
        out.set("parent", Json(nullptr));
    else
        out.set("parent", Json(static_cast<std::uint64_t>(n->parent)));
    Json rect = Json::object();
    rect.set("x", Json(static_cast<double>(n->bounds.x)));
    rect.set("y", Json(static_cast<double>(n->bounds.y)));
    rect.set("w", Json(static_cast<double>(n->bounds.w)));
    rect.set("h", Json(static_cast<double>(n->bounds.h)));
    out.set("rect", std::move(rect));
    out.set("visible", Json(n->style.visible));
    out.set("opacity", Json(static_cast<double>(n->style.opacity)));
    out.set("childCount", Json(static_cast<std::uint64_t>(live_child_count(scene.tree, id))));
    if (const std::optional<double> value = bound_value(scene, id))
    {
        out.set("boundState", Json(scene.bound.at(id)));
        out.set("boundValue", Json(*value));
    }
    return out;
}

// The whole state store as a {name: value} object — the drive-observable state `ui send` reports.
Json state_json(const LoadedScene& scene)
{
    Json out = Json::object();
    for (const auto& [name, value] : *scene.state)
        out.set(name, Json(value));
    return out;
}

// --- the individual verbs -------------------------------------------------------------------------

Envelope ui_dump(const std::string& scene_path, const std::map<std::string, std::string>& flags)
{
    LoadedScene scene;
    const ui::Rect viewport{0.0f, 0.0f, viewport_dim(flags, "width", 320.0f),
                            viewport_dim(flags, "height", 240.0f)};
    if (const std::optional<LoadError> err = load_scene(scene_path, viewport, scene))
        return Envelope::failure(err->code, err->message);

    Json nodes = Json::array();
    for (ui::NodeId id : ui::live_nodes_preorder(scene.tree))
        nodes.push_back(node_json(scene, id));

    Json data = Json::object();
    data.set("scene", Json(scene_path));
    Json vp = Json::object();
    vp.set("w", Json(static_cast<double>(viewport.w)));
    vp.set("h", Json(static_cast<double>(viewport.h)));
    data.set("viewport", std::move(vp));
    data.set("nodeCount", Json(static_cast<std::uint64_t>(nodes.size())));
    data.set("nodes", std::move(nodes));
    data.set("state", state_json(scene));
    return Envelope::success(std::move(data));
}

Envelope ui_query(const std::string& scene_path, const std::string& name,
                  const std::map<std::string, std::string>& flags)
{
    LoadedScene scene;
    const ui::Rect viewport{0.0f, 0.0f, viewport_dim(flags, "width", 320.0f),
                            viewport_dim(flags, "height", 240.0f)};
    if (const std::optional<LoadError> err = load_scene(scene_path, viewport, scene))
        return Envelope::failure(err->code, err->message);

    const ui::NodeId id = ui::find_by_name(scene.tree, name);
    if (id == ui::kInvalidNode)
        return Envelope::failure(ui::kNodeNotFoundCode,
                                 "no UI node named '" + name + "' in the scene");
    return Envelope::success(node_json(scene, id));
}

Envelope ui_send(const std::string& scene_path, const std::string& event,
                 const std::map<std::string, std::string>& flags)
{
    LoadedScene scene;
    const ui::Rect viewport{0.0f, 0.0f, viewport_dim(flags, "width", 320.0f),
                            viewport_dim(flags, "height", 240.0f)};
    if (const std::optional<LoadError> err = load_scene(scene_path, viewport, scene))
        return Envelope::failure(err->code, err->message);

    const std::string* target_name = flag(flags, "target");
    if (target_name == nullptr)
        return Envelope::failure(ui::kInvalidEventCode, "a `ui send` requires --target <node>");
    const ui::NodeId target = ui::find_by_name(scene.tree, *target_name);
    if (target == ui::kInvalidNode)
        return Envelope::failure(ui::kNodeNotFoundCode,
                                 "no UI node named '" + *target_name + "' in the scene");

    Json data = Json::object();
    data.set("event", Json(event));
    data.set("target", Json(*target_name));

    if (event == "focus")
    {
        scene.tree.set_focus(target);
    }
    else if (event == "text")
    {
        const std::string* text = flag(flags, "text");
        if (text == nullptr)
            return Envelope::failure(ui::kInvalidEventCode, "a `text` event requires --text <string>");
        scene.tree.set_text(target, *text);
        data.set("text", Json(*text));
    }
    else if (event == "key")
    {
        const std::string* code = flag(flags, "code");
        if (code == nullptr)
            return Envelope::failure(ui::kInvalidEventCode, "a `key` event requires --code <key>");
        ui::Event ev;
        ev.type = ui::EventType::KeyDown;
        ev.target = target;
        ev.key = *code;
        scene.tree.dispatch(ev);
        data.set("code", Json(*code));
    }
    else if (event == "click" || event == "pointerdown")
    {
        ui::Event ev;
        ev.type = ui::EventType::PointerDown;
        ev.target = target;
        const ui::UiNode* n = scene.tree.node(target);
        ev.pointer_x = n->bounds.x;
        ev.pointer_y = n->bounds.y;
        scene.tree.dispatch(ev);
    }
    else
    {
        return Envelope::failure(ui::kInvalidEventCode,
                                 "unknown `ui send` event '" + event +
                                     "' (expected click | focus | key | text)");
    }

    const ui::NodeId focused = scene.tree.focused();
    if (focused == ui::kInvalidNode)
        data.set("focused", Json(nullptr));
    else
        data.set("focused", Json(scene.tree.node(focused)->name));
    data.set("state", state_json(scene));
    return Envelope::success(std::move(data));
}

Envelope ui_assert(const std::string& scene_path, const std::string& name,
                   const std::map<std::string, std::string>& flags)
{
    LoadedScene scene;
    const ui::Rect viewport{0.0f, 0.0f, viewport_dim(flags, "width", 320.0f),
                            viewport_dim(flags, "height", 240.0f)};
    if (const std::optional<LoadError> err = load_scene(scene_path, viewport, scene))
        return Envelope::failure(err->code, err->message);

    const ui::NodeId id = ui::find_by_name(scene.tree, name);
    if (id == ui::kInvalidNode)
        return Envelope::failure(ui::kNodeNotFoundCode,
                                 "no UI node named '" + name + "' in the scene");
    const ui::UiNode* n = scene.tree.node(id);

    // Each assertion flag is checked; the first failure short-circuits with expected-vs-actual detail.
    if (const std::string* role = flag(flags, "role"))
    {
        const std::string actual = ui::role_name(n->role);
        if (actual != *role)
            return Envelope::failure(ui::kAssertionFailedCode, "node '" + name + "' role: expected '" +
                                                                   *role + "', actual '" + actual + "'");
    }
    if (const std::string* text = flag(flags, "text"))
    {
        if (n->text != *text)
            return Envelope::failure(ui::kAssertionFailedCode, "node '" + name + "' text: expected '" +
                                                                   *text + "', actual '" + n->text +
                                                                   "'");
    }
    if (flag(flags, "visible") != nullptr && !n->style.visible)
        return Envelope::failure(ui::kAssertionFailedCode,
                                 "node '" + name + "' expected visible, actual hidden");
    if (flag(flags, "hidden") != nullptr && n->style.visible)
        return Envelope::failure(ui::kAssertionFailedCode,
                                 "node '" + name + "' expected hidden, actual visible");
    if (const std::string* child_count = flag(flags, "child-count"))
    {
        const std::optional<double> expected = parse_double(*child_count);
        if (!expected.has_value())
            return Envelope::failure(ui::kInvalidEventCode, "--child-count is not a number");
        const double actual = static_cast<double>(live_child_count(scene.tree, id));
        if (actual != *expected)
            return Envelope::failure(ui::kAssertionFailedCode,
                                     "node '" + name + "' childCount: expected " + *child_count +
                                         ", actual " + std::to_string(static_cast<long long>(actual)));
    }
    if (const std::string* value = flag(flags, "value"))
    {
        const std::optional<double> expected = parse_double(*value);
        if (!expected.has_value())
            return Envelope::failure(ui::kInvalidEventCode, "--value is not a number");
        const std::optional<double> actual = bound_value(scene, id);
        if (!actual.has_value())
            return Envelope::failure(ui::kAssertionFailedCode,
                                     "node '" + name + "' has no data binding to compare --value against");
        if (std::fabs(*actual - *expected) > 1e-9)
            return Envelope::failure(ui::kAssertionFailedCode,
                                     "node '" + name + "' boundValue: expected " + *value + ", actual " +
                                         std::to_string(*actual));
    }

    Json data = Json::object();
    data.set("node", Json(name));
    data.set("passed", Json(true));
    data.set("detail", node_json(scene, id));
    return Envelope::success(std::move(data));
}
} // namespace

Envelope run_ui(const std::string& verb, const std::map<std::string, std::string>& bound,
                const std::map<std::string, std::string>& flags)
{
    auto scene_it = bound.find("scene");
    if (scene_it == bound.end())
        return Envelope::failure("usage.missing_argument", "ui requires a UI-scene file path");
    const std::string& scene = scene_it->second;

    if (verb == "dump")
        return ui_dump(scene, flags);
    if (verb == "query")
    {
        auto node_it = bound.find("node");
        if (node_it == bound.end())
            return Envelope::failure("usage.missing_argument", "ui query requires a node name");
        return ui_query(scene, node_it->second, flags);
    }
    if (verb == "send")
    {
        auto event_it = bound.find("event");
        if (event_it == bound.end())
            return Envelope::failure("usage.missing_argument", "ui send requires an event kind");
        return ui_send(scene, event_it->second, flags);
    }
    if (verb == "assert")
    {
        auto node_it = bound.find("node");
        if (node_it == bound.end())
            return Envelope::failure("usage.missing_argument", "ui assert requires a node name");
        return ui_assert(scene, node_it->second, flags);
    }
    return Envelope::failure("usage.unknown_verb", "unknown ui verb: '" + verb + "'");
}

} // namespace context::cli

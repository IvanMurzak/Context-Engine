// The runtime UI TypeScript-authoring binding shim (M7 T4 / a4). See script_bindings.h. Pure stdlib +
// context_ui; no V8/js header — the doubles-only host-function table is bound to the engine by the
// runtime/ts glue (the two HostFunction signatures are the same type by construction).

#include "context/packages/ui/script_bindings.h"

#include "context/packages/ui/ui_tree.h"

#include <algorithm>
#include <cmath>

namespace context::packages::ui
{

namespace
{

// Round a double arg to the nearest signed integer index (host<->JS values are exact for the small
// ints/handles this protocol uses; llround avoids truncation surprises on e.g. 2.9999999).
[[nodiscard]] long long to_index(double raw) noexcept
{
    return std::llround(raw);
}

[[nodiscard]] NodeId to_node(double raw) noexcept
{
    return static_cast<NodeId>(static_cast<std::uint32_t>(to_index(raw) & 0xFFFFFFFFLL));
}

[[nodiscard]] std::int32_t to_key(double raw) noexcept
{
    return static_cast<std::int32_t>(to_index(raw));
}

// Clamp a 0..255 channel double to a byte.
[[nodiscard]] std::uint8_t to_channel(double raw) noexcept
{
    const double c = std::clamp(raw, 0.0, 255.0);
    return static_cast<std::uint8_t>(std::llround(c));
}

[[nodiscard]] bool decode_index(double raw, long long count, long long& out) noexcept
{
    const long long v = to_index(raw);
    if (v < 0 || v >= count)
        return false;
    out = v;
    return true;
}

} // namespace

// --- numeric protocol decoders -------------------------------------------------------------------

bool decode_role(double raw, Role& out) noexcept
{
    long long v = 0;
    if (!decode_index(raw, 12, v)) // Role has 12 values (Root..ListItem)
        return false;
    out = static_cast<Role>(static_cast<int>(v));
    return true;
}

bool decode_event_type(double raw, EventType& out) noexcept
{
    long long v = 0;
    if (!decode_index(raw, 10, v)) // EventType has 10 values (PointerDown..Custom)
        return false;
    out = static_cast<EventType>(static_cast<int>(v));
    return true;
}

bool decode_positioning(double raw, Positioning& out) noexcept
{
    long long v = 0;
    if (!decode_index(raw, 2, v)) // Positioning: Flow, Absolute
        return false;
    out = static_cast<Positioning>(static_cast<int>(v));
    return true;
}

bool decode_flow(double raw, Flow& out) noexcept
{
    long long v = 0;
    if (!decode_index(raw, 3, v)) // Flow: None, Row, Column
        return false;
    out = static_cast<Flow>(static_cast<int>(v));
    return true;
}

// --- StateStore ----------------------------------------------------------------------------------

void StateStore::set(std::int32_t key, double value) { values_[key] = value; }

void StateStore::add(std::int32_t key, double delta) { values_[key] += delta; }

double StateStore::get(std::int32_t key) const noexcept
{
    const auto it = values_.find(key);
    return it == values_.end() ? 0.0 : it->second;
}

bool StateStore::has(std::int32_t key) const noexcept { return values_.find(key) != values_.end(); }

// --- UiScriptContext -----------------------------------------------------------------------------

UiScriptContext::UiScriptContext(UiTree& tree, StateStore& state) noexcept
    : tree_(tree), state_(state)
{
}

void UiScriptContext::set_invoker(std::function<void(std::uint32_t, Event&)> invoker)
{
    invoker_ = std::move(invoker);
}

NodeId UiScriptContext::create(NodeId parent, Role role) { return tree_.create_node(role, parent); }

bool UiScriptContext::set_opacity(NodeId id, double opacity)
{
    UiNode* n = tree_.node(id);
    if (n == nullptr)
        return false;
    Style s = n->style;
    s.opacity = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    return tree_.set_style(id, s);
}

bool UiScriptContext::set_visible(NodeId id, bool visible) { return tree_.set_visible(id, visible); }

bool UiScriptContext::set_background(NodeId id, double r, double g, double b, double a)
{
    UiNode* n = tree_.node(id);
    if (n == nullptr)
        return false;
    Style s = n->style;
    s.background = Color{to_channel(r), to_channel(g), to_channel(b), to_channel(a)};
    return tree_.set_style(id, s);
}

bool UiScriptContext::set_foreground(NodeId id, double r, double g, double b, double a)
{
    UiNode* n = tree_.node(id);
    if (n == nullptr)
        return false;
    Style s = n->style;
    s.foreground = Color{to_channel(r), to_channel(g), to_channel(b), to_channel(a)};
    return tree_.set_style(id, s);
}

bool UiScriptContext::set_padding(NodeId id, double padding)
{
    UiNode* n = tree_.node(id);
    if (n == nullptr)
        return false;
    Style s = n->style;
    s.padding = static_cast<float>(std::max(0.0, padding));
    return tree_.set_style(id, s);
}

bool UiScriptContext::set_layout_box(NodeId id, Positioning pos, double w, double h, Flow flow,
                                     double gap)
{
    UiNode* n = tree_.node(id);
    if (n == nullptr)
        return false;
    Layout l = n->layout;
    l.position = pos;
    l.size = Vec2{static_cast<float>(w), static_cast<float>(h)};
    l.flow = flow;
    l.gap = static_cast<float>(std::max(0.0, gap));
    return tree_.set_layout(id, l);
}

bool UiScriptContext::set_bounds(NodeId id, double x, double y, double w, double h)
{
    return tree_.set_bounds(id, Rect{static_cast<float>(x), static_cast<float>(y),
                                     static_cast<float>(w), static_cast<float>(h)});
}

void UiScriptContext::on(NodeId id, EventType type, std::uint32_t handler_id)
{
    tree_.add_handler(id, type,
                      [this, handler_id](Event& ev) { this->invoke_handler(handler_id, ev); });
}

bool UiScriptContext::bind_value(NodeId id, std::int32_t state_key)
{
    if (tree_.node(id) == nullptr)
        return false;
    bindings_[id] = state_key;
    return true;
}

bool UiScriptContext::is_bound(NodeId id) const noexcept
{
    return bindings_.find(id) != bindings_.end();
}

double UiScriptContext::bound_value(NodeId id) const noexcept
{
    const auto it = bindings_.find(id);
    return it == bindings_.end() ? 0.0 : state_.get(it->second);
}

void UiScriptContext::write_state(std::int32_t key, double value) { state_.set(key, value); }

void UiScriptContext::add_state(std::int32_t key, double delta) { state_.add(key, delta); }

double UiScriptContext::read_state(std::int32_t key) const noexcept { return state_.get(key); }

void UiScriptContext::invoke_handler(std::uint32_t handler_id, Event& ev)
{
    if (invoker_)
        invoker_(handler_id, ev);
}

// --- the doubles-only host-function table --------------------------------------------------------

namespace
{

[[nodiscard]] UiScriptContext* ctx_of(void* user) noexcept
{
    return static_cast<UiScriptContext*>(user);
}

[[nodiscard]] double arg(const double* args, std::size_t nargs, std::size_t i) noexcept
{
    return i < nargs ? args[i] : 0.0;
}

// __ui_create(parent, role) -> nodeId (kInvalidNode on a bad parent/role)
double h_create(void* user, const double* args, std::size_t nargs)
{
    UiScriptContext* ctx = ctx_of(user);
    Role role = Role::Group;
    if (nargs < 2 || !decode_role(args[1], role))
        return static_cast<double>(kInvalidNode);
    return static_cast<double>(ctx->create(to_node(args[0]), role));
}

// __ui_set_opacity(node, opacity) -> 1/0
double h_set_opacity(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_opacity(to_node(arg(args, nargs, 0)), arg(args, nargs, 1)) ? 1.0 : 0.0;
}

// __ui_set_visible(node, visible) -> 1/0
double h_set_visible(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_visible(to_node(arg(args, nargs, 0)), arg(args, nargs, 1) != 0.0) ? 1.0
                                                                                                : 0.0;
}

// __ui_set_background(node, r, g, b, a) -> 1/0
double h_set_background(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_background(to_node(arg(args, nargs, 0)), arg(args, nargs, 1),
                                        arg(args, nargs, 2), arg(args, nargs, 3),
                                        arg(args, nargs, 4))
               ? 1.0
               : 0.0;
}

// __ui_set_foreground(node, r, g, b, a) -> 1/0
double h_set_foreground(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_foreground(to_node(arg(args, nargs, 0)), arg(args, nargs, 1),
                                        arg(args, nargs, 2), arg(args, nargs, 3),
                                        arg(args, nargs, 4))
               ? 1.0
               : 0.0;
}

// __ui_set_padding(node, padding) -> 1/0
double h_set_padding(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_padding(to_node(arg(args, nargs, 0)), arg(args, nargs, 1)) ? 1.0 : 0.0;
}

// __ui_set_layout(node, position, w, h, flow, gap) -> 1/0
double h_set_layout(void* user, const double* args, std::size_t nargs)
{
    Positioning pos = Positioning::Flow;
    Flow flow = Flow::None;
    if (nargs < 6 || !decode_positioning(args[1], pos) || !decode_flow(args[4], flow))
        return 0.0;
    return ctx_of(user)->set_layout_box(to_node(args[0]), pos, args[2], args[3], flow, args[5]) ? 1.0
                                                                                                : 0.0;
}

// __ui_set_bounds(node, x, y, w, h) -> 1/0
double h_set_bounds(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->set_bounds(to_node(arg(args, nargs, 0)), arg(args, nargs, 1),
                                    arg(args, nargs, 2), arg(args, nargs, 3), arg(args, nargs, 4))
               ? 1.0
               : 0.0;
}

// __ui_on(node, eventType, handlerId) -> 1/0 (0 = bad event type)
double h_on(void* user, const double* args, std::size_t nargs)
{
    EventType type = EventType::Custom;
    if (nargs < 3 || !decode_event_type(args[1], type))
        return 0.0;
    ctx_of(user)->on(to_node(args[0]), type, static_cast<std::uint32_t>(to_index(args[2]) & 0xFFFFFFFFLL));
    return 1.0;
}

// __ui_bind_value(node, stateKey) -> 1/0
double h_bind_value(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->bind_value(to_node(arg(args, nargs, 0)), to_key(arg(args, nargs, 1))) ? 1.0
                                                                                                : 0.0;
}

// __ui_write_state(key, value) -> value
double h_write_state(void* user, const double* args, std::size_t nargs)
{
    const double value = arg(args, nargs, 1);
    ctx_of(user)->write_state(to_key(arg(args, nargs, 0)), value);
    return value;
}

// __ui_add_state(key, delta) -> new value
double h_add_state(void* user, const double* args, std::size_t nargs)
{
    const std::int32_t key = to_key(arg(args, nargs, 0));
    ctx_of(user)->add_state(key, arg(args, nargs, 1));
    return ctx_of(user)->read_state(key);
}

// __ui_read_state(key) -> value
double h_read_state(void* user, const double* args, std::size_t nargs)
{
    return ctx_of(user)->read_state(to_key(arg(args, nargs, 0)));
}

} // namespace

const std::vector<UiHostBinding>& ui_host_bindings()
{
    static const std::vector<UiHostBinding> kBindings = {
        {"__ui_create", &h_create},
        {"__ui_set_opacity", &h_set_opacity},
        {"__ui_set_visible", &h_set_visible},
        {"__ui_set_background", &h_set_background},
        {"__ui_set_foreground", &h_set_foreground},
        {"__ui_set_padding", &h_set_padding},
        {"__ui_set_layout", &h_set_layout},
        {"__ui_set_bounds", &h_set_bounds},
        {"__ui_on", &h_on},
        {"__ui_bind_value", &h_bind_value},
        {"__ui_write_state", &h_write_state},
        {"__ui_add_state", &h_add_state},
        {"__ui_read_state", &h_read_state},
    };
    return kBindings;
}

} // namespace context::packages::ui

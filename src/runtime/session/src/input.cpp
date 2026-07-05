// Input stream model + canonical (de)serialization (see input.h).

#include "context/runtime/session/input.h"

#include "context/runtime/session/json_build.h"

#include <algorithm>

namespace context::runtime::session
{

namespace
{
// Find (or create, keeping the vector sorted by tick) the TickInputs entry for `tick`.
TickInputs& tick_entry(std::vector<TickInputs>& ticks, std::uint64_t tick)
{
    auto it = std::lower_bound(ticks.begin(), ticks.end(), tick,
                               [](const TickInputs& t, std::uint64_t k) { return t.tick < k; });
    if (it != ticks.end() && it->tick == tick)
        return *it;
    TickInputs entry;
    entry.tick = tick;
    return *ticks.insert(it, std::move(entry));
}
} // namespace

void InputStream::add_event(std::uint64_t tick, InputEvent event)
{
    tick_entry(ticks, tick).events.push_back(std::move(event));
}

void InputStream::add_action(std::uint64_t tick, ActionActivation action)
{
    tick_entry(ticks, tick).actions.push_back(std::move(action));
}

const TickInputs* InputStream::at_tick(std::uint64_t tick) const
{
    auto it = std::lower_bound(ticks.begin(), ticks.end(), tick,
                               [](const TickInputs& t, std::uint64_t k) { return t.tick < k; });
    if (it != ticks.end() && it->tick == tick)
        return &*it;
    return nullptr;
}

std::uint64_t InputStream::span() const noexcept
{
    std::uint64_t max_tick = 0;
    bool any = false;
    for (const TickInputs& t : ticks)
        if (!t.empty())
        {
            any = true;
            if (t.tick >= max_tick)
                max_tick = t.tick;
        }
    return any ? max_tick + 1 : 0;
}

serializer::JsonValue input_stream_to_json(const InputStream& stream)
{
    serializer::JsonValue arr = jb::array();
    for (const TickInputs& t : stream.ticks)
    {
        if (t.empty())
            continue;
        serializer::JsonValue entry = jb::object();
        jb::set(entry, "tick", jb::uinteger(t.tick));

        serializer::JsonValue events = jb::array();
        for (const InputEvent& e : t.events)
        {
            serializer::JsonValue ev = jb::object();
            jb::set(ev, "code", jb::str(e.code));
            jb::set(ev, "device", jb::str(e.device));
            jb::set(ev, "value", jb::integer(e.value));
            jb::push(events, std::move(ev));
        }
        jb::set(entry, "events", std::move(events));

        serializer::JsonValue actions = jb::array();
        for (const ActionActivation& a : t.actions)
        {
            serializer::JsonValue act = jb::object();
            jb::set(act, "action", jb::str(a.action));
            jb::set(act, "phase", jb::str(a.phase));
            jb::set(act, "value", jb::integer(a.value));
            jb::push(actions, std::move(act));
        }
        jb::set(entry, "actions", std::move(actions));

        jb::push(arr, std::move(entry));
    }
    return arr;
}

InputStream input_stream_from_json(const serializer::JsonValue& node)
{
    InputStream stream;
    if (node.type != serializer::JsonValue::Type::array)
        return stream;
    for (const serializer::JsonValue& entry : node.elements)
    {
        const std::uint64_t tick = jb::as_uint(jb::member(entry, "tick"));
        if (const serializer::JsonValue* events = jb::member(entry, "events");
            events != nullptr && events->type == serializer::JsonValue::Type::array)
            for (const serializer::JsonValue& e : events->elements)
                stream.add_event(tick, InputEvent{jb::as_str(jb::member(e, "device")),
                                                  jb::as_str(jb::member(e, "code")),
                                                  jb::as_int(jb::member(e, "value"))});
        if (const serializer::JsonValue* actions = jb::member(entry, "actions");
            actions != nullptr && actions->type == serializer::JsonValue::Type::array)
            for (const serializer::JsonValue& a : actions->elements)
                stream.add_action(tick, ActionActivation{jb::as_str(jb::member(a, "action")),
                                                         jb::as_str(jb::member(a, "phase")),
                                                         jb::as_int(jb::member(a, "value"))});
    }
    return stream;
}

} // namespace context::runtime::session

// The UI event + handler model (M7 T1, R-UI-006). Pointer / focus / key / custom events and the handler
// callback type the retained tree dispatches (target-then-bubble). Headless: an event is a plain value,
// dispatch runs with no renderer, so UI logic is fully CI-assertable without a GPU.

#pragma once

#include "context/packages/ui/ui_node.h"

#include <functional>
#include <string>

namespace context::packages::ui
{

// The event kinds the runtime tree dispatches. Pointer/key carry positional/key data; focus events are
// emitted by UiTree::set_focus; Custom carries a caller-named channel for app-level UI intents.
enum class EventType
{
    PointerDown,
    PointerUp,
    PointerMove,
    PointerEnter,
    PointerLeave,
    FocusGained,
    FocusLost,
    KeyDown,
    KeyUp,
    Custom
};

// A single dispatched event. `target` is the node the event is delivered to (T1: the caller supplies it;
// T2's hit-testing computes it). `current` is updated to each node visited as the event bubbles toward
// the root. A handler sets `handled = true` to stop propagation. Fields not relevant to an event kind
// keep their defaults (a KeyDown ignores pointer_x/y; a PointerDown ignores `key`).
struct Event
{
    EventType type = EventType::Custom;
    NodeId target = kInvalidNode;
    NodeId current = kInvalidNode;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    std::string key;         // device-agnostic key name for KeyDown/KeyUp (e.g. "Enter", "W")
    std::string custom_name; // channel name for a Custom event
    bool handled = false;
};

// A UI event handler. Receives the event by reference so it can mark it handled (and read `current`).
using Handler = std::function<void(Event&)>;

} // namespace context::packages::ui

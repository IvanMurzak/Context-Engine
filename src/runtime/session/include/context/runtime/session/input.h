// Synthetic input model + the recordable/replayable input stream (R-QA-005, R-CLI-009 injection).
//
// Headless session control injects two kinds of input, both deterministically timestamped to a sim
// tick: raw synthetic INPUT EVENTS (device/code/value — the low-level layer) and ACTION ACTIVATIONS
// (a mapped action + phase + value — the gameplay/UI layer). The InputStream is the ordered,
// per-tick record of everything injected; recording captures it and replay feeds it back at the
// same ticks, so a run is reproducible from (seed + input stream) alone.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::runtime::session
{

// The canonical-JSON DOM lives in context::editor::serializer; alias it locally so the session
// tier's document signatures stay terse (redeclared identically across headers — allowed).
namespace serializer = ::context::editor::serializer;

// A raw synthetic input event (the low-level layer): a device, a code on it, and an integer value
// (e.g. device="key" code="W" value=1; device="mouse" code="x" value=42).
struct InputEvent
{
    std::string device;
    std::string code;
    std::int64_t value = 0;
};

// A mapped action activation (the gameplay/UI layer): the action name, its phase, and a value. Phase
// follows the standard input-action lifecycle — "started" / "performed" / "canceled" — shared by
// gameplay actions ("move_x", "fire") and UI actions ("ui_submit", "ui_cancel").
struct ActionActivation
{
    std::string action;
    std::string phase; // "started" | "performed" | "canceled"
    std::int64_t value = 0;
};

// Everything injected for one sim tick.
struct TickInputs
{
    std::uint64_t tick = 0;
    std::vector<InputEvent> events;
    std::vector<ActionActivation> actions;

    [[nodiscard]] bool empty() const noexcept { return events.empty() && actions.empty(); }
};

// The ordered per-tick input record — the recordable/replayable stream. Entries are kept sorted by
// tick; a tick with no injected input has no entry (the input system sees an empty tick).
struct InputStream
{
    std::vector<TickInputs> ticks;

    void add_event(std::uint64_t tick, InputEvent event);
    void add_action(std::uint64_t tick, ActionActivation action);

    // The inputs scheduled at `tick`, or nullptr if none were injected there.
    [[nodiscard]] const TickInputs* at_tick(std::uint64_t tick) const;

    [[nodiscard]] bool empty() const noexcept { return ticks.empty(); }
    // The highest tick carrying any injected input, + 1 (0 when empty) — a natural minimum tick
    // count for a replay that must cover every injected input.
    [[nodiscard]] std::uint64_t span() const noexcept;
};

// Canonical (de)serialization to the serializer JsonValue tree (used by session-state + replay docs).
[[nodiscard]] serializer::JsonValue input_stream_to_json(const InputStream& stream);
[[nodiscard]] InputStream input_stream_from_json(const serializer::JsonValue& node);

} // namespace context::runtime::session

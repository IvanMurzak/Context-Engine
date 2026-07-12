// Action maps + input contexts — the authored front-end data the input package routes with (M6 P7,
// R-SYS-007 / L-45).
//
// A game's input configuration is a set of stackable INPUT CONTEXTS. Each context is a named set of
// BINDINGS on a LAYER (gameplay or UI) that maps a raw device source (a device + a code on it) to a
// mapped ACTION name — the same action names the sim InputState sink already understands ("move_x",
// "fire", "ui_menu"). A context may additionally CAPTURE: a capturing UI context blocks input that no
// higher context bound from reaching gameplay contexts below it (the L-45 layered UI-capture stack —
// the "UI has focus, gameplay does not receive input" rule of R-SYS-007).
//
// This is pure authored data (POD strings) — no float, no sim state, no kernel dependency; the routing
// behaviour on top lives in input_router.h. It mirrors the authored ctx:input-bindings content kind
// (src/editor/kinds/input_bindings.h) one-to-one so an authored bindings document loads into these
// structs.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace context::packages::input
{

// The routing layer a context belongs to. Gameplay input is the sim-affecting channel (mapped
// actions fold into InputState); UI input is arbitrated ABOVE gameplay (R-SYS-007 focus arbitration).
enum class Layer
{
    Gameplay,
    Ui
};

// The recognised raw device sources (R-SYS-007): the `device` field on a raw input event. Kept as a
// small closed set so a binding's device is validated at install time. These match the session
// InputEvent.device strings the sink already carries.
inline constexpr std::string_view kDeviceKeyboard = "keyboard";
inline constexpr std::string_view kDeviceMouse = "mouse";
inline constexpr std::string_view kDeviceGamepad = "gamepad";
inline constexpr std::string_view kDeviceTouch = "touch";
inline constexpr std::string_view kDeviceVr = "vr"; // VR-controller

// Whether `device` names one of the recognised device sources.
[[nodiscard]] inline bool is_known_device(std::string_view device) noexcept
{
    return device == kDeviceKeyboard || device == kDeviceMouse || device == kDeviceGamepad ||
           device == kDeviceTouch || device == kDeviceVr;
}

// A single binding: a raw (device, code) source mapped to a named action. `code` is device-specific
// (e.g. keyboard "W", mouse "MouseLeft", gamepad "ButtonSouth", vr "TriggerRight").
struct Binding
{
    std::string device;
    std::string code;
    std::string action;
};

// A stackable input context: a named, layered set of bindings, optionally CAPTURING. When pushed onto
// the router's active stack it maps its bound sources to actions; a capturing UI context additionally
// swallows any unbound input so it never reaches a gameplay context below (L-45 modal capture).
struct InputContext
{
    std::string id;
    Layer layer = Layer::Gameplay;
    bool capture = false;
    std::vector<Binding> bindings;
};

} // namespace context::packages::input

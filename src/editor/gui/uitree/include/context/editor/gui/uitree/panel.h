// Headless editor panel + the a11y / keyboard-nav harness hook (R-EDIT-001 / R-A11Y-001).
//
// A Panel is a headless-instantiable editor surface: a named root UiNode plus the set of commands it
// exposes. Every built-in panel (scene tree / inspector / Problems / palette) is built ON this, so
// its logic is CI-assertable WITHOUT booting CEF — the seam that makes every M5 fan-out panel
// testable headless. The a11y audit + focus-order model below are the CI-assertable half of
// R-A11Y-001 (the "a11y-harness hook"); the CEF/axe DOM scan a later task adds is the other half.

#pragma once

#include "context/editor/gui/uitree/node.h"

#include <string>
#include <vector>

namespace context::editor::gui::uitree
{

// A named action a panel exposes. A command is the keyboard/CLI path for a GUI affordance — bound to
// one or more focusable UiNodes via UiNode::set_command (R-CLI-001 CLI-completeness as structural
// accessibility). F0b models command identity + reachability; the actual handler wiring is per-panel
// (later M5 tasks).
struct Command
{
    std::string id;
    std::string title;
};

class Panel
{
public:
    Panel(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {}

    Panel& set_root(UiNode root)
    {
        root_ = std::move(root);
        has_root_ = true;
        return *this;
    }
    Panel& add_command(std::string command_id, std::string title)
    {
        commands_.push_back(Command{std::move(command_id), std::move(title)});
        return *this;
    }

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }
    [[nodiscard]] bool has_root() const noexcept { return has_root_; }
    [[nodiscard]] const UiNode& root() const noexcept { return root_; }
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }

    [[nodiscard]] bool has_command(const std::string& command_id) const;

private:
    std::string id_;
    std::string title_;
    bool has_root_ = false;
    UiNode root_{Role::region, ""};
    std::vector<Command> commands_;
};

// --- headless a11y + keyboard-nav harness (R-A11Y-001) ------------------------------------------

struct A11yViolation
{
    std::string node_id; // the offending node (or command id for command-level findings)
    std::string code;    // "missing-name" | "orphan-command" | "unreachable-command" | "duplicate-id"
    std::string message;
};

// The keyboard (tab) focus order: the focusable nodes in depth-first document order — the model a
// keyboard-only navigation test asserts against. Returns node ids.
[[nodiscard]] std::vector<std::string> focus_order(const Panel& panel);

// Automated accessibility audit over the headless tree (R-A11Y-001), returning the (possibly empty)
// violation list:
//   * every focusable node AND every name-requiring role must carry a non-empty accessible name
//     ("missing-name");
//   * every node bound to a command must reference a command the panel exposes ("orphan-command");
//   * every command the panel exposes must be reachable from at least one focusable node — so every
//     action has a keyboard path ("unreachable-command");
//   * node ids must be unique within the panel ("duplicate-id").
// This is the headless half of the a11y-harness hook; a later task hardens it with the CEF/axe DOM
// scan + a real keyboard-driver test in the editor-cef-smoke CI job.
[[nodiscard]] std::vector<A11yViolation> audit_a11y(const Panel& panel);

// Render the panel body to semantic HTML (delegates to uitree::render_html on the root). The CEF
// host loads this to paint the placeholder panel; headless callers assert it directly.
[[nodiscard]] std::string render_html(const Panel& panel);

// Compose the FULL HTML document a host loads for `panel`: the escaped panel title, the sandbox CSP
// as a <meta http-equiv> header, and the panel body from render_html above.
//
// This exists so no host hand-concatenates a document around render_html's output: `title` and `csp`
// are interpolated into HTML exactly like any other value, so both go through the C-F6
// escape_html_text contract (node.h). A CSP string is not authored project data today, but it IS
// per-contribution manifest data, and a document-composition site that escapes its body while
// concatenating its head raw is precisely the gap the contract exists to close.
[[nodiscard]] std::string render_document(const Panel& panel, const std::string& csp);

} // namespace context::editor::gui::uitree

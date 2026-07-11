// UI-logic tree node (R-EDIT-001 / R-A11Y-001): the headless, CEF-free node every editor panel is
// built from, so palette/undo/Problems/inspector logic is CI-assertable WITHOUT booting CEF.
//
// A node carries the accessibility model directly: an ARIA-style `role`, an accessible `label`
// (name), keyboard-`focusable`ility, and an optional binding to a named panel `command`. Modelling
// a11y in the logic tree — not just in the rendered DOM — makes "semantic HTML + ARIA + complete
// keyboard navigation" (R-A11Y-001) something the headless tests assert, and makes render_html()
// (node.cpp) emit semantic HTML by construction rather than <div> soup.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::uitree
{

// The closed ARIA-style role vocabulary for the headless tree. Kept small + explicit so the a11y
// audit can reason about which roles REQUIRE an accessible name and which semantic HTML tag each
// maps to (node.cpp). Extended as built-in panels land; a package-contributed role is out of F0b
// scope (the extension contract carries its own contribution kinds — gui/contract/).
enum class Role
{
    region,   // a labelled landmark/section — a panel body (name required)
    group,    // a generic grouping (no accessible name required)
    tree,     // a scene-tree container
    treeitem, // a scene-tree row
    list,     // a generic list container
    listitem, // a list row
    button,   // an actionable control (name required)
    textbox,  // an editable field (name required)
    checkbox, // a toggle (name required)
    heading,  // a section heading (name required)
    status,   // a live status region — e.g. the Problems count (name required)
    text,     // inert text content (no accessible name of its own)
};

// Does `role` require an accessible name (label) to be conformant with R-A11Y-001? Landmarks and
// interactive/heading/status roles do; generic groupings and inert text do not.
[[nodiscard]] bool role_requires_name(Role role);

// The stable ARIA role token string (grep-able) emitted into the rendered HTML + a11y reports.
[[nodiscard]] const char* role_token(Role role);

// The semantic HTML tag chosen for `role` (semantic HTML, not <div> soup — R-A11Y-001).
[[nodiscard]] const char* role_html_tag(Role role);

// A node in the headless UI-logic tree. Built with a fluent set_* API for terse tree construction;
// every setter returns *this so a whole panel body is one expression.
class UiNode
{
public:
    UiNode(Role role, std::string id) : role_(role), id_(std::move(id)) {}

    UiNode& set_label(std::string label)
    {
        label_ = std::move(label);
        return *this;
    }
    UiNode& set_text(std::string text)
    {
        text_ = std::move(text);
        return *this;
    }
    UiNode& set_focusable(bool focusable)
    {
        focusable_ = focusable;
        return *this;
    }
    // Bind this node to a named panel command — the non-pointer/keyboard path for a GUI affordance
    // (R-CLI-001 CLI-completeness as a structural accessibility property).
    UiNode& set_command(std::string command_id)
    {
        command_ = std::move(command_id);
        return *this;
    }
    UiNode& add_child(UiNode child)
    {
        children_.push_back(std::move(child));
        return *this;
    }

    [[nodiscard]] Role role() const noexcept { return role_; }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool focusable() const noexcept { return focusable_; }
    [[nodiscard]] const std::string& command() const noexcept { return command_; }
    [[nodiscard]] const std::vector<UiNode>& children() const noexcept { return children_; }

private:
    Role role_;
    std::string id_;
    std::string label_;
    std::string text_;
    bool focusable_ = false;
    std::string command_; // empty = the node exposes no command
    std::vector<UiNode> children_;
};

// Render one node (and its subtree) to semantic HTML with ARIA role + aria-label + a tabindex on
// focusable nodes (R-A11Y-001). Headless — returns a string; the CEF host loads it to paint. The
// output is deterministic (stable attribute order) so it is byte-assertable in a ctest. HTML text is
// escaped so a label/text value cannot break out of its attribute/element.
[[nodiscard]] std::string render_html(const UiNode& node);

} // namespace context::editor::gui::uitree

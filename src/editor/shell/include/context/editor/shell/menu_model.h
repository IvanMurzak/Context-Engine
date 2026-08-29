// The PUBLISHED MENU MODEL, Shell side (editor-window-chrome d3, menu structure 03 / target 02 §4).
//
// WHAT THIS IS. d3 builds ONE declarative menu model in editor-core — every item backed by a command
// id in the e07b registry — and renders it two ways: a web menubar inside the titlebar strip on
// Windows/Linux, and the native global NSMenu bar on macOS. The macOS half needs the model to CROSS
// the bridge (`menu.publish`, window_bridge.h kMenuPublishMethod), and this header is the Shell-side
// shape it crosses into: a total, fail-closed parse of the renderer-published JSON into plain C++
// values the Cocoa backend can build an NSMenu from (cocoa_menu.h).
//
// THE c1 SPLIT, APPLIED AGAIN (cocoa_chrome.h's discipline): every DECISION here — what a
// well-formed model is, which items survive a malformed sibling, how an accelerator string tokenizes
// — is PURE and compiled + ctest-run on every OS (tests/test_menu_model.cpp), while the AppKit calls
// that consume the parsed model live in cocoa_window.mm, honestly untested off macOS.
//
// TOTALITY. `params` is RENDERER-CONTROLLED input, so the parse is total and bounded: a model that
// is not an object with a `menus` array is refused outright (nullopt — the bridge answers
// `window.bad_params`), an ITEM that cannot be read is DROPPED rather than failing its whole menu
// (the parseRecentProject discipline: one bad entry costs that entry), and depth/size caps keep a
// hostile payload from building an unbounded NSMenu tree.

#pragma once

#include "context/editor/contract/json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The submenu NESTING cap, counting the top-level menu as depth 1. The d3 tree needs exactly 2
// (File > Open Recent); 3 leaves one level of headroom before a renderer bug (or a hostile payload)
// is truncated rather than recursed into.
inline constexpr std::size_t kMenuMaxDepth = 3;

// The total item cap across the whole model (separators and submenu headers included). The d3 tree
// is ~40 items; 512 is far above any honest model and far below anything that could hurt.
inline constexpr std::size_t kMenuMaxItems = 512;

// The wire `type` tokens a menu item carries. Mirrored by the TS serializer (menu.ts
// MENU_ITEM_TYPE_*); an item with an unknown type is DROPPED (a newer renderer's item kind an older
// Shell cannot render is honestly absent, never a parse failure that costs the whole menu).
inline constexpr const char* kMenuItemTypeCommand = "command";
inline constexpr const char* kMenuItemTypeSeparator = "separator";
inline constexpr const char* kMenuItemTypeSubmenu = "submenu";

// One PARSED accelerator: the modifier set plus the (lowercased single-char, or named) key token.
// Produced by `parse_menu_accelerator` below; consumed by the Cocoa backend, which maps `ctrl` onto
// the COMMAND key — the macOS convention for the primary modifier (the CmdOrCtrl reading), and the
// only mapping under which the published `Ctrl+Z` accelerator is the ⌘Z macOS users expect.
struct MenuAccelerator
{
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool meta = false;
    std::string key; // lowercased; single char for letters/digits, the token verbatim otherwise
};

// One parsed menu item. A tagged struct rather than a variant: the three kinds share rendering
// context (a submenu is a labelled item WITH children), and the consumer switches on `kind` exactly
// once while building the native tree.
struct MenuItem
{
    enum class Kind
    {
        command,
        separator,
        submenu,
    };

    Kind kind = Kind::command;
    std::string command_id;   // command items only; non-empty by construction
    std::string label;        // command + submenu items
    std::string accelerator;  // command items; "" = none (display + key-equivalent source)
    std::string tooltip;      // command items; "" = none (the disabled-reason channel)
    bool enabled = true;      // command items; the publish-time enablement snapshot
    std::vector<MenuItem> items; // submenu items only
};

// One top-level menu (File / Edit / View / …).
struct MenuDefinition
{
    std::string id;    // grep-stable ("file", "edit", …); may be empty (the label still renders)
    std::string label; // non-empty by construction
    std::vector<MenuItem> items;
};

// The whole published model.
struct MenuModel
{
    std::vector<MenuDefinition> menus;

    // Every item in the model, command items only, counted recursively — the smoke/test observable
    // for "the parse kept what it should have".
    [[nodiscard]] std::size_t command_count() const;
};

// Parse one published model. nullopt when `params` is not an object carrying a `menus` ARRAY — the
// fail-closed outer shape (the bridge then answers `window.bad_params`). Inside that shape the parse
// is DROP-TOLERANT per entry: a menu with no readable label, an item with no readable type/id/label,
// an over-deep submenu, or any item past the `kMenuMaxItems` cap is skipped, never fatal.
[[nodiscard]] std::optional<MenuModel> parse_menu_model(const contract::Json& params);

// Tokenize one accelerator display string ("Ctrl+Shift+Z", "Alt+ArrowLeft") into its modifier set +
// key token. nullopt for anything that does not parse — an empty string, a dangling `+`, a modifier
// with no key, an unknown modifier token — so a consumer never builds a key equivalent out of a
// guess. Modifier tokens (case-insensitive): Ctrl, Shift, Alt, Meta, Cmd (an alias of Meta). The
// key token keeps its spelling, lowercased when it is a single ASCII letter.
[[nodiscard]] std::optional<MenuAccelerator> parse_menu_accelerator(const std::string& text);

} // namespace context::editor::shell

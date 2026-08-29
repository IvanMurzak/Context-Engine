// T1 for the published menu model's parse (editor-window-chrome d3, menu structure 03).
//
// WHAT THIS PROVES, on every leg (the c1 discipline: the DECISIONS are pure and cross-platform even
// though their production consumer is the macOS NSMenu builder):
//
//   1. THE OUTER SHAPE FAILS CLOSED — anything that is not `{menus: [...]}` parses to nullopt, so
//      the bridge can answer `window.bad_params` for a malformed publish instead of installing a
//      silently-empty menu bar.
//   2. INSIDE THAT SHAPE THE PARSE IS DROP-TOLERANT PER ENTRY — one unreadable item costs exactly
//      that item (the parseRecentProject discipline), and an unknown `type` token (a NEWER
//      renderer's item kind) is honestly absent rather than fatal.
//   3. THE CAPS HOLD — a hostile payload cannot recurse past `kMenuMaxDepth` or grow the tree past
//      `kMenuMaxItems`.
//   4. THE ACCELERATOR TOKENIZER IS TOTAL — every chord the d3 tree publishes parses, and every
//      malformed spelling refuses rather than guessing a key equivalent.

#include "context/editor/shell/menu_model.h"

#include "shell_test.h"

#include <string>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

[[nodiscard]] Json command_item(const char* id, const char* label, const char* accelerator = "",
                                bool enabled = true)
{
    Json item = Json::object();
    item.set("type", Json(std::string(kMenuItemTypeCommand)));
    item.set("id", Json(std::string(id)));
    item.set("label", Json(std::string(label)));
    if (accelerator[0] != '\0')
    {
        item.set("accelerator", Json(std::string(accelerator)));
    }
    item.set("enabled", Json(enabled));
    return item;
}

[[nodiscard]] Json menu_of(const char* id, const char* label, Json items)
{
    Json menu = Json::object();
    menu.set("id", Json(std::string(id)));
    menu.set("label", Json(std::string(label)));
    menu.set("items", std::move(items));
    return menu;
}

void the_outer_shape_fails_closed()
{
    CHECK(!parse_menu_model(Json{}).has_value());
    CHECK(!parse_menu_model(Json(std::string("menus"))).has_value());
    CHECK(!parse_menu_model(Json::array()).has_value());
    Json no_menus = Json::object();
    no_menus.set("something", Json(std::string("else")));
    CHECK(!parse_menu_model(no_menus).has_value());
    Json wrong_type = Json::object();
    wrong_type.set("menus", Json(std::string("not an array")));
    CHECK(!parse_menu_model(wrong_type).has_value());

    // The MINIMAL well-formed model: an empty menus array parses to an empty model — an honest
    // "nothing to render", never a refusal.
    Json empty = Json::object();
    empty.set("menus", Json::array());
    const std::optional<MenuModel> model = parse_menu_model(empty);
    CHECK(model.has_value());
    CHECK(model->menus.empty());
    CHECK(model->command_count() == 0);
}

void a_full_menu_round_trips_with_every_member()
{
    Json items = Json::array();
    Json with_tooltip = command_item("edit.cut", "Cut", "Ctrl+X", false);
    with_tooltip.set("tooltip", Json(std::string("clipboard is future work")));
    items.push_back(std::move(with_tooltip));
    Json separator = Json::object();
    separator.set("type", Json(std::string(kMenuItemTypeSeparator)));
    items.push_back(std::move(separator));
    Json sub_items = Json::array();
    sub_items.push_back(command_item("project.openRecent.0", "alpha"));
    Json submenu = Json::object();
    submenu.set("type", Json(std::string(kMenuItemTypeSubmenu)));
    submenu.set("label", Json(std::string("Open Recent")));
    submenu.set("items", std::move(sub_items));
    items.push_back(std::move(submenu));

    Json model_json = Json::object();
    Json menus = Json::array();
    menus.push_back(menu_of("file", "File", std::move(items)));
    model_json.set("menus", std::move(menus));

    const std::optional<MenuModel> model = parse_menu_model(model_json);
    CHECK(model.has_value());
    CHECK(model->menus.size() == 1);
    const MenuDefinition& file = model->menus[0];
    CHECK(file.id == "file");
    CHECK(file.label == "File");
    CHECK(file.items.size() == 3);
    CHECK(file.items[0].kind == MenuItem::Kind::command);
    CHECK(file.items[0].command_id == "edit.cut");
    CHECK(file.items[0].label == "Cut");
    CHECK(file.items[0].accelerator == "Ctrl+X");
    CHECK(file.items[0].tooltip == "clipboard is future work");
    CHECK(file.items[0].enabled == false);
    CHECK(file.items[1].kind == MenuItem::Kind::separator);
    CHECK(file.items[2].kind == MenuItem::Kind::submenu);
    CHECK(file.items[2].label == "Open Recent");
    CHECK(file.items[2].items.size() == 1);
    CHECK(file.items[2].items[0].command_id == "project.openRecent.0");
    // `enabled` ABSENT defaults true — an older renderer that never evaluated enablement meant its
    // items to work.
    CHECK(file.items[2].items[0].enabled);
    CHECK(model->command_count() == 2);
}

void a_malformed_entry_costs_exactly_that_entry()
{
    Json items = Json::array();
    items.push_back(command_item("help.about", "About"));
    items.push_back(Json(std::string("not an object")));  // dropped
    Json nameless = Json::object();                       // command with no id: dropped
    nameless.set("type", Json(std::string(kMenuItemTypeCommand)));
    nameless.set("label", Json(std::string("Nameless")));
    items.push_back(std::move(nameless));
    Json unknown = Json::object();                        // a NEWER renderer's kind: dropped
    unknown.set("type", Json(std::string("toggle")));
    unknown.set("id", Json(std::string("x")));
    unknown.set("label", Json(std::string("X")));
    items.push_back(std::move(unknown));
    items.push_back(command_item("help.docs", "Documentation"));

    Json menus = Json::array();
    menus.push_back(menu_of("help", "Help", std::move(items)));
    // A LABEL-LESS menu is dropped whole — it cannot render a menubar entry.
    Json label_less = Json::object();
    label_less.set("id", Json(std::string("ghost")));
    label_less.set("items", Json::array());
    menus.push_back(std::move(label_less));
    Json model_json = Json::object();
    model_json.set("menus", std::move(menus));

    const std::optional<MenuModel> model = parse_menu_model(model_json);
    CHECK(model.has_value());
    CHECK(model->menus.size() == 1);
    CHECK(model->menus[0].items.size() == 2);
    CHECK(model->menus[0].items[0].command_id == "help.about");
    CHECK(model->menus[0].items[1].command_id == "help.docs");
}

void the_caps_hold_against_a_hostile_payload()
{
    // DEPTH: a submenu nested past kMenuMaxDepth is truncated, not recursed into. Depth counts the
    // top-level menu as 1, so the chain below puts a command at depth 5 — beyond the cap of 3.
    Json level4 = Json::object();
    level4.set("type", Json(std::string(kMenuItemTypeSubmenu)));
    level4.set("label", Json(std::string("level4")));
    Json level4_items = Json::array();
    level4_items.push_back(command_item("too.deep", "Too Deep"));
    level4.set("items", std::move(level4_items));
    Json level3 = Json::object();
    level3.set("type", Json(std::string(kMenuItemTypeSubmenu)));
    level3.set("label", Json(std::string("level3")));
    Json level3_items = Json::array();
    level3_items.push_back(std::move(level4));
    level3.set("items", std::move(level3_items));
    Json level2 = Json::object();
    level2.set("type", Json(std::string(kMenuItemTypeSubmenu)));
    level2.set("label", Json(std::string("level2")));
    Json level2_items = Json::array();
    level2_items.push_back(std::move(level3));
    level2.set("items", std::move(level2_items));

    Json deep_items = Json::array();
    deep_items.push_back(std::move(level2));
    Json deep_json = Json::object();
    Json deep_menus = Json::array();
    deep_menus.push_back(menu_of("deep", "Deep", std::move(deep_items)));
    deep_json.set("menus", std::move(deep_menus));
    const std::optional<MenuModel> deep = parse_menu_model(deep_json);
    CHECK(deep.has_value());
    // level2 (depth 2) holds level3 (depth 3); level3 is EMPTY — level4 (depth 4) was refused.
    CHECK(deep->menus[0].items.size() == 1);
    CHECK(deep->menus[0].items[0].items.size() == 1);
    CHECK(deep->menus[0].items[0].items[0].items.empty());
    CHECK(deep->command_count() == 0);

    // SIZE: items past the model-wide budget are dropped, never grown into an unbounded tree.
    Json many = Json::array();
    for (int i = 0; i < 700; ++i)
    {
        many.push_back(command_item("flood.item", "Flood"));
    }
    Json flood_json = Json::object();
    Json flood_menus = Json::array();
    flood_menus.push_back(menu_of("flood", "Flood", std::move(many)));
    flood_json.set("menus", std::move(flood_menus));
    const std::optional<MenuModel> flood = parse_menu_model(flood_json);
    CHECK(flood.has_value());
    CHECK(flood->menus[0].items.size() == kMenuMaxItems);
    CHECK(flood->command_count() == kMenuMaxItems);
}

void the_accelerator_tokenizer_is_total()
{
    // Every chord shape the d3 tree actually publishes (keymap.ts DEFAULT_KEYBINDINGS spellings).
    const std::optional<MenuAccelerator> undo = parse_menu_accelerator("Ctrl+Z");
    CHECK(undo.has_value());
    CHECK(undo->ctrl && !undo->shift && !undo->alt && !undo->meta);
    CHECK(undo->key == "z"); // single ASCII letters lowercase, the canonical key-equivalent form

    const std::optional<MenuAccelerator> redo = parse_menu_accelerator("Ctrl+Shift+Z");
    CHECK(redo.has_value());
    CHECK(redo->ctrl && redo->shift);

    const std::optional<MenuAccelerator> move = parse_menu_accelerator("Alt+ArrowLeft");
    CHECK(move.has_value());
    CHECK(move->alt && !move->ctrl);
    CHECK(move->key == "ArrowLeft"); // named keys keep their spelling

    const std::optional<MenuAccelerator> meta = parse_menu_accelerator("Cmd+Q");
    CHECK(meta.has_value());
    CHECK(meta->meta && meta->key == "q");

    // Malformed spellings REFUSE rather than guess: an empty chord, a dangling `+`, a lone
    // modifier, an unknown modifier token.
    CHECK(!parse_menu_accelerator("").has_value());
    CHECK(!parse_menu_accelerator("Ctrl+").has_value());
    CHECK(!parse_menu_accelerator("Ctrl").has_value());
    CHECK(!parse_menu_accelerator("Ctrl+Shift").has_value());
    CHECK(!parse_menu_accelerator("Hyper+Z").has_value());
    CHECK(!parse_menu_accelerator("Ctrl++").has_value());
}

} // namespace

int main()
{
    the_outer_shape_fails_closed();
    a_full_menu_round_trips_with_every_member();
    a_malformed_entry_costs_exactly_that_entry();
    the_caps_hold_against_a_hostile_payload();
    the_accelerator_tokenizer_is_total();
    SHELL_TEST_MAIN_END();
}

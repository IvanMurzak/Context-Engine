// The published menu model's total, fail-closed parse (editor-window-chrome d3) — see menu_model.h.

#include "context/editor/shell/menu_model.h"

#include <cctype>

namespace context::editor::shell
{

namespace
{

// A string member read, "" for absent/mismatched — the read_seed discipline: believed about what it
// says, silent on the rest.
[[nodiscard]] std::string read_string(const contract::Json& object, const char* key)
{
    const contract::Json& value = object.at(key);
    return value.is_string() ? value.as_string() : std::string{};
}

// Parse one item ARRAY into `out`, recursively for submenus. `depth` counts the enclosing menu as 1;
// `budget` is the model-wide remaining item allowance, decremented for every KEPT item. Drop-tolerant
// per entry (menu_model.h's contract); an over-deep submenu or an exhausted budget drops the rest.
void parse_items(const contract::Json& array, std::size_t depth, std::size_t& budget,
                 std::vector<MenuItem>& out)
{
    for (std::size_t i = 0; i < array.size(); ++i)
    {
        if (budget == 0)
        {
            return; // the cap is the whole point: a hostile payload stops growing the tree here
        }
        const contract::Json& entry = array.at(i);
        if (!entry.is_object())
        {
            continue;
        }
        const std::string type = read_string(entry, "type");
        if (type == kMenuItemTypeSeparator)
        {
            MenuItem item;
            item.kind = MenuItem::Kind::separator;
            out.push_back(std::move(item));
            --budget;
            continue;
        }
        if (type == kMenuItemTypeCommand)
        {
            MenuItem item;
            item.kind = MenuItem::Kind::command;
            item.command_id = read_string(entry, "id");
            item.label = read_string(entry, "label");
            if (item.command_id.empty() || item.label.empty())
            {
                continue; // an unnameable or unreadable item is dropped, never a nameless entry
            }
            item.accelerator = read_string(entry, "accelerator");
            item.tooltip = read_string(entry, "tooltip");
            // `enabled` defaults TRUE when absent: an older renderer that never evaluated
            // enablement published items it meant to work.
            item.enabled = !entry.contains("enabled") || entry.at("enabled").as_bool();
            out.push_back(std::move(item));
            --budget;
            continue;
        }
        if (type == kMenuItemTypeSubmenu)
        {
            if (depth + 1 > kMenuMaxDepth)
            {
                continue; // over-deep: truncated rather than recursed into (menu_model.h)
            }
            MenuItem item;
            item.kind = MenuItem::Kind::submenu;
            item.label = read_string(entry, "label");
            const contract::Json& children = entry.at("items");
            if (item.label.empty() || !children.is_array())
            {
                continue;
            }
            --budget; // the submenu header itself is an item
            parse_items(children, depth + 1, budget, item.items);
            out.push_back(std::move(item));
            continue;
        }
        // An unknown type token: a newer renderer's item kind this Shell cannot render — honestly
        // absent, never a parse failure that costs the whole menu (menu_model.h's contract).
    }
}

} // namespace

std::size_t MenuModel::command_count() const
{
    std::size_t count = 0;
    // A tiny explicit stack rather than recursion-by-helper: the depth cap bounds it anyway, but a
    // counter should not need a second recursive walk to audit the first.
    std::vector<const std::vector<MenuItem>*> pending;
    for (const MenuDefinition& menu : menus)
    {
        pending.push_back(&menu.items);
    }
    while (!pending.empty())
    {
        const std::vector<MenuItem>* items = pending.back();
        pending.pop_back();
        for (const MenuItem& item : *items)
        {
            if (item.kind == MenuItem::Kind::command)
            {
                ++count;
            }
            else if (item.kind == MenuItem::Kind::submenu)
            {
                pending.push_back(&item.items);
            }
        }
    }
    return count;
}

std::optional<MenuModel> parse_menu_model(const contract::Json& params)
{
    if (!params.is_object())
    {
        return std::nullopt;
    }
    const contract::Json& menus = params.at("menus");
    if (!menus.is_array())
    {
        return std::nullopt;
    }
    MenuModel model;
    std::size_t budget = kMenuMaxItems;
    for (std::size_t i = 0; i < menus.size(); ++i)
    {
        const contract::Json& entry = menus.at(i);
        if (!entry.is_object())
        {
            continue;
        }
        MenuDefinition menu;
        menu.id = read_string(entry, "id");
        menu.label = read_string(entry, "label");
        const contract::Json& items = entry.at("items");
        if (menu.label.empty() || !items.is_array())
        {
            continue; // a label-less menu cannot render a menubar entry — dropped, never nameless
        }
        parse_items(items, 1, budget, menu.items);
        model.menus.push_back(std::move(menu));
    }
    return model;
}

std::optional<MenuAccelerator> parse_menu_accelerator(const std::string& text)
{
    if (text.empty())
    {
        return std::nullopt;
    }
    MenuAccelerator out;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t plus = text.find('+', start);
        const bool last = plus == std::string::npos;
        std::string token = last ? text.substr(start) : text.substr(start, plus - start);
        if (token.empty())
        {
            return std::nullopt; // a dangling/doubled `+` — refuse rather than guess
        }
        // Case-insensitive modifier compare, without locale surprises: ASCII-lower the copy.
        std::string lowered = token;
        for (char& c : lowered)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!last)
        {
            if (lowered == "ctrl")
            {
                out.ctrl = true;
            }
            else if (lowered == "shift")
            {
                out.shift = true;
            }
            else if (lowered == "alt")
            {
                out.alt = true;
            }
            else if (lowered == "meta" || lowered == "cmd")
            {
                out.meta = true;
            }
            else
            {
                return std::nullopt; // an unknown modifier — refuse the whole chord
            }
            start = plus + 1;
            continue;
        }
        // The key token: a lone modifier name in key position is not a chord.
        if (lowered == "ctrl" || lowered == "shift" || lowered == "alt" || lowered == "meta" ||
            lowered == "cmd")
        {
            return std::nullopt;
        }
        out.key = token.size() == 1 ? lowered : token;
        return out;
    }
}

} // namespace context::editor::shell

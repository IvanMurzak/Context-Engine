"""Unit tests for tools/check_kit_tokens.py — the M9 e06c1 component-kit gates.

THE POINT OF THIS FILE IS NON-VACUITY, mechanised. A lint that no longer fires is indistinguishable
from a codebase that is clean, and the difference only surfaces the day something ships unthemed. So
every rule the tokens-only lint claims to enforce is asserted twice here: once that a clean kit
stylesheet PASSES, and once that a planted violation of that exact rule EXITS 1 — a raw hex colour, a
colour function, a named colour, a raw font size, a raw font weight, a named font family, a raw
border length, a raw duration, a raw easing curve, a `var()` fallback and a non-`--ctx-` custom
property. Proving it by hand once at authoring time would have proven it for one commit; this proof
re-runs in the `python-tests` CI job on every PR.

The role-coverage gate gets the same treatment from the other direction: it must FAIL when the C++
role vocabulary grows a member the kit does not style, when the kit maps a role C++ never emits, when
a class has no rule, and when a `.ctx-widget-` rule survives in the app stylesheet.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import check_kit_tokens  # noqa: E402


# --------------------------------------------------------------------------------------- fixtures

CLEAN_KIT_CSS = """
/* A kit stylesheet's own prose may name a raw value like #0a0a0a - comments are stripped. */
.ctx-widget-button {
    font: inherit;
    padding: 2px 8px;
    color: var(--ctx-colors-ink);
    background: var(--ctx-colors-chip);
    border: var(--ctx-shape-border-width) solid var(--ctx-colors-line);
    border-radius: var(--ctx-shape-radius-sm);
    cursor: pointer;
}

.ctx-widget-button:focus-visible {
    outline: var(--ctx-shape-focus-ring-width) solid var(--ctx-colors-focus-ring);
    outline-offset: calc(-1 * var(--ctx-shape-focus-ring-width));
}
"""


def _kit(tmp_path: Path, css: str = CLEAN_KIT_CSS) -> Path:
    styles = tmp_path / "styles"
    styles.mkdir(parents=True, exist_ok=True)
    (styles / "kit.css").write_text(css, encoding="utf-8")
    return styles


def _run_tokens_only(styles: Path) -> int:
    return check_kit_tokens.main(["--tokens-only", "--kit-styles", str(styles)])


# ---------------------------------------------------------------------------- the tokens-only lint


def test_a_clean_kit_stylesheet_passes(tmp_path: Path) -> None:
    assert _run_tokens_only(_kit(tmp_path)) == 0


def test_comments_may_name_raw_values(tmp_path: Path) -> None:
    """A gate that fired on its own documentation would teach authors to stop documenting."""
    css = "/* the Dark theme's colors.panel is #0a0a0a, rgb(10,10,10) */\n" + CLEAN_KIT_CSS
    assert _run_tokens_only(_kit(tmp_path, css)) == 0


@pytest.mark.parametrize(
    ("declaration", "why"),
    [
        ("color: #ff0000;", "a raw hex colour"),
        ("background: rgba(0, 0, 0, 0.25);", "a raw colour function"),
        ("background: hsl(210, 50%, 20%);", "a raw hsl colour"),
        ("outline-color: red;", "a named CSS colour"),
        ("border-color: darkslategrey;", "another named CSS colour"),
        ("font-size: 13px;", "a raw font length"),
        ("font-weight: 600;", "a raw font weight"),
        ('font-family: "Geist", sans-serif;', "a named font family"),
        ("font-family: monospace;", "a generic font family"),
        ("border: 1px solid var(--ctx-colors-line);", "a raw length in the shape family"),
        ("border-radius: 3px;", "a raw radius"),
        ("outline-width: 2px;", "a raw outline width"),
        ("transition: background-color 120ms linear;", "a raw duration"),
        ("transition: color var(--ctx-motion-duration-fast) cubic-bezier(0.4, 0, 0.2, 1);",
         "a raw easing curve"),
        ("border-radius: var(--ctx-shape-radius-sm, 3px);", "a var() fallback"),
        ("color: var(--kit-ink);", "a non---ctx- custom property"),
    ],
)
def test_a_planted_raw_value_fails(tmp_path: Path, declaration: str, why: str) -> None:
    """THE non-vacuity proof: each rule the lint advertises must actually fire. (`why` names it.)"""
    css = CLEAN_KIT_CSS + "\n.ctx-widget-text {\n    %s\n}\n" % declaration
    assert _run_tokens_only(_kit(tmp_path, css)) == 1, why


@pytest.mark.parametrize(
    "declaration",
    [
        "padding: 2px 6px;",          # box spacing: e06a publishes no token (kit README, gap 1)
        "margin: 0 0 6px;",
        "display: block;",
        "cursor: pointer;",
        "list-style: none;",
        "min-width: 0;",
        "height: 100%;",
        "font: inherit;",
        "color: currentColor;",
        "background: transparent;",
        "outline-offset: calc(-1 * var(--ctx-shape-focus-ring-width));",
    ],
)
def test_legitimate_declarations_are_not_findings(tmp_path: Path, declaration: str) -> None:
    """The other half of a useful gate: it must not fire on what the kit is allowed to write."""
    css = CLEAN_KIT_CSS + "\n.ctx-widget-text {\n    %s\n}\n" % declaration
    assert _run_tokens_only(_kit(tmp_path, css)) == 0


@pytest.mark.parametrize(
    ("css", "why"),
    [
        (".ctx-widget-text { color: #ff0000; }", "the ONLY declaration is not at a line start"),
        (
            ".ctx-widget-text {\n    color: var(--ctx-colors-ink); background: rgba(0, 0, 0, 0.5);\n}",
            "the SECOND declaration on a line",
        ),
        (
            ".ctx-widget-text { color: var(--ctx-colors-ink); font-size: 13px; }",
            "a raw font length in a one-line rule",
        ),
    ],
)
def test_a_compactly_written_rule_is_scanned_too(tmp_path: Path, css: str, why: str) -> None:
    """Formatting must not be a bypass.

    The scan is anchored on the declaration SEPARATOR (`{` / `;`), not on the start of a line. A
    line-anchored scan reads only each line's FIRST declaration, so every rule the lint advertises
    was evadable by writing the rule compactly -- legal CSS that no other gate rejects, and exactly
    the shape a stylesheet minifier or a hurried author produces.
    """
    assert _run_tokens_only(_kit(tmp_path, CLEAN_KIT_CSS + "\n" + css + "\n")) == 1, why


def test_a_selector_is_never_mistaken_for_a_declaration(tmp_path: Path) -> None:
    """The other side of de-anchoring: a pseudo-class must not read as `property: value`.

    A selector follows `}` or the start of a rule list -- never `{` or `;` -- so it cannot enter the
    scan. Asserted rather than reasoned about, because a false positive here would fire on the
    shipped stylesheet's own `:hover` / `:focus-visible` selector lists.
    """
    css = """
.ctx-widget-listitem:hover,
.ctx-widget-treeitem:hover {
    background: var(--ctx-colors-panel2);
}
"""
    assert _run_tokens_only(_kit(tmp_path, css)) == 0


@pytest.mark.parametrize("weight", ["430", "520", "600"])
def test_any_three_digit_font_weight_is_a_finding(tmp_path: Path, weight: str) -> None:
    """The shipped themes' own weights are 430 / 520 / 600.

    A rule matching only the `n00` ladder waves through the two literals an author copying from
    `dark.theme.json` is likeliest to type.
    """
    css = CLEAN_KIT_CSS + "\n.ctx-widget-text {\n    font-weight: %s;\n}\n" % weight
    assert _run_tokens_only(_kit(tmp_path, css)) == 1


def test_a_missing_styles_directory_is_a_configuration_error(tmp_path: Path) -> None:
    assert _run_tokens_only(tmp_path / "nope") == 2


def test_an_empty_styles_directory_is_a_configuration_error(tmp_path: Path) -> None:
    """A gate with no input must not report success -- that is the vacuous pass in its purest form."""
    empty = tmp_path / "styles"
    empty.mkdir()
    assert _run_tokens_only(empty) == 2


# --------------------------------------------------------------------------- the role-coverage gate

CPP_ROLES = """
const char* role_requires_name(Role role)
{
    return "not this one";
}

const char* role_token(Role role)
{
    switch (role)
    {
    case Role::button:
        return "button";
    case Role::text:
        return "text";
    }
    return "text";
}
"""

KIT_INDEX_TS = """
export const WIDGET_CLASSES: Readonly<Record<string, string>> = {
    button: "ctx-widget-button",
    text: "ctx-widget-text",
};
"""

COVERED_CSS = """
.ctx-widget-button {
    color: var(--ctx-colors-ink);
}

.ctx-widget-text {
    color: var(--ctx-colors-ink);
}
"""

CLEAN_APP_CSS = """
.ctx-panel-body {
    color: var(--ctx-colors-ink);
}
"""


def _coverage_case(tmp_path: Path, *, cpp: str = CPP_ROLES, ts: str = KIT_INDEX_TS,
                   css: str = COVERED_CSS, app: str = CLEAN_APP_CSS,
                   second_sheet: str | None = None) -> int:
    (tmp_path / "node.cpp").write_text(cpp, encoding="utf-8")
    (tmp_path / "index.ts").write_text(ts, encoding="utf-8")
    (tmp_path / "app.css").write_text(app, encoding="utf-8")
    styles = _kit(tmp_path, css)
    if second_sheet is not None:
        (styles / "components.css").write_text(second_sheet, encoding="utf-8")
    return check_kit_tokens.main(
        [
            "--role-coverage",
            "--kit-source", str(tmp_path / "index.ts"),
            "--kit-styles", str(styles),
            "--uitree-source", str(tmp_path / "node.cpp"),
            "--app-stylesheet", str(tmp_path / "app.css"),
        ]
    )


def test_an_agreeing_closed_set_passes(tmp_path: Path) -> None:
    assert _coverage_case(tmp_path) == 0


def test_a_new_cpp_role_with_no_kit_entry_fails(tmp_path: Path) -> None:
    """The headline failure: a thirteenth role would otherwise ship unthemed and silent."""
    cpp = CPP_ROLES.replace(
        '    case Role::text:\n        return "text";',
        '    case Role::text:\n        return "text";\n    case Role::slider:\n        return "slider";',
    )
    assert _coverage_case(tmp_path, cpp=cpp) == 1


@pytest.mark.parametrize("token", ["slider", "listbox2", "doc-abstract", "menu_item"])
def test_a_new_cpp_role_is_seen_whatever_its_token_is_spelled_like(tmp_path: Path,
                                                                  token: str) -> None:
    """The headline failure must not depend on the new role's SPELLING.

    Reading only all-lowercase-letter tokens made `cpp_roles` come back SHORT for a role carrying a
    digit or a hyphen, so the gate printed OK on precisely the thirteenth-role case it exists to
    catch -- a vacuous pass, which is worse than no gate at all.
    """
    cpp = CPP_ROLES.replace(
        '    case Role::text:\n        return "text";',
        '    case Role::text:\n        return "text";\n    case Role::%s:\n        return "%s";'
        % (token.replace("-", "_"), token),
    )
    assert _coverage_case(tmp_path, cpp=cpp) == 1


def test_a_kit_entry_cpp_never_emits_fails(tmp_path: Path) -> None:
    ts = KIT_INDEX_TS.replace(
        '    text: "ctx-widget-text",',
        '    text: "ctx-widget-text",\n    slider: "ctx-widget-slider",',
    )
    assert _coverage_case(tmp_path, ts=ts) == 1


def test_a_mapped_role_with_no_kit_rule_fails(tmp_path: Path) -> None:
    css = COVERED_CSS.replace(".ctx-widget-text {\n    color: var(--ctx-colors-ink);\n}\n", "")
    assert _coverage_case(tmp_path, css=css) == 1


def test_an_orphan_kit_rule_fails(tmp_path: Path) -> None:
    css = COVERED_CSS + "\n.ctx-widget-slider {\n    color: var(--ctx-colors-ink);\n}\n"
    assert _coverage_case(tmp_path, css=css) == 1


def test_a_surviving_widget_rule_in_app_css_fails(tmp_path: Path) -> None:
    """The 'exactly ONE styling owner' half: a second sheet makes the cascade decide appearance."""
    app = CLEAN_APP_CSS + "\n.ctx-widget-button {\n    background: var(--ctx-colors-chip);\n}\n"
    assert _coverage_case(tmp_path, app=app) == 1


def test_an_unreadable_cpp_role_table_is_a_configuration_error(tmp_path: Path) -> None:
    assert _coverage_case(tmp_path, cpp="// role_token was renamed\n") == 2


def test_an_unreadable_widget_map_is_a_configuration_error(tmp_path: Path) -> None:
    assert _coverage_case(tmp_path, ts="export const SOMETHING_ELSE = {};\n") == 2


# ------------------------------------------------- the second-kit-sheet widget-ownership rule (e06c2)
#
# e06c1 enforced "one styling owner" as "no OTHER FILE declares a `.ctx-widget-` rule", which was the
# same thing while the kit was one stylesheet. e06c2 made it two, and one authored family legitimately
# reaches a widget class (a LIVE badge IS the `status` role primitive, reused rather than copied). The
# property is therefore enforced at the level it was always ABOUT -- appearance must never be decided
# by document order -- so a second sheet may narrow a widget class ONLY through a compound selector.


def test_a_second_kit_sheet_may_compound_a_widget_class(tmp_path: Path) -> None:
    """`.ctx-widget-text.ctx-badge` is 0,2,0 and beats the widget layer whatever the load order."""
    second = ".ctx-widget-text.ctx-badge {\n    color: var(--ctx-colors-ink);\n}\n"
    assert _coverage_case(tmp_path, second_sheet=second) == 0


def test_a_second_kit_sheet_may_not_style_a_widget_class_BARE(tmp_path: Path) -> None:
    """A bare rule TIES at 0,1,0, and CMake staging order then decides which one paints."""
    second = ".ctx-widget-text {\n    color: var(--ctx-colors-ink);\n}\n"
    assert _coverage_case(tmp_path, second_sheet=second) == 1


def test_the_compound_check_is_not_defeated_by_greedy_backtracking(tmp_path: Path) -> None:
    """The regex must anchor the END of the class name before asking "is a dot next?".

    Without excluding the class-name characters from the lookahead, `[a-z0-9-]+` simply backtracks:
    `.ctx-widget-text.ctx-badge` matches as `...tex` followed by `t`, so the "is it compounded"
    question gets answered about a PREFIX of the class name and every compound selector reads as bare
    (or, with the lookahead the other way round, every bare selector reads as compounded). This is the
    same greedy-backtracking hole that made e06c1's role regex miss a thirteenth role.
    """
    assert _coverage_case(
        tmp_path,
        second_sheet=".ctx-widget-text.ctx-chip {\n    color: var(--ctx-colors-ink);\n}\n",
    ) == 0
    assert _coverage_case(
        tmp_path,
        second_sheet=".ctx-widget-text:hover {\n    color: var(--ctx-colors-ink);\n}\n",
    ) == 1


# ------------------------------------------------------- the kit TS source-tokens gate (e06c2)

CLEAN_KIT_TS = """
export function createThing(): HTMLElement {
    const element = document.createElement("div");
    element.classList.add("ctx-thing");
    element.textContent = "hello";
    return element;
}
"""


def _kit_src(tmp_path: Path, ts: str = CLEAN_KIT_TS) -> Path:
    source = tmp_path / "src"
    source.mkdir(parents=True, exist_ok=True)
    (source / "index.ts").write_text(ts, encoding="utf-8")
    return source


def _run_source_tokens(source: Path) -> int:
    return check_kit_tokens.main(["--source-tokens", "--kit-source-dir", str(source)])


def test_a_clean_kit_module_passes(tmp_path: Path) -> None:
    assert _run_source_tokens(_kit_src(tmp_path)) == 0


@pytest.mark.parametrize(
    ("statement", "why"),
    [
        ('element.style.color = "#ff0000";', "an inline style property write"),
        ('element.style.setProperty("--kit-ink", "#fff");', "a setProperty write"),
        ('element.style.cssText = "color: red";', "a cssText write"),
        ('element.setAttribute("style", "color: red");', "a style ATTRIBUTE"),
        ('document.createElement("style");', "a runtime <style> element"),
        ("sheet.insertRule(rule);", "a runtime stylesheet mutation"),
        ('const bg = "#0a0a0a";', "a raw hex colour in a string literal"),
        ('const bg = "rgba(0, 0, 0, 0.5)";', "a raw colour function in a string literal"),
    ],
)
def test_a_planted_styling_bypass_fails(tmp_path: Path, statement: str, why: str) -> None:
    """THE non-vacuity proof for the TS half: each shape the CSS lint cannot see must fire here."""
    assert _run_source_tokens(_kit_src(tmp_path, CLEAN_KIT_TS + "\n" + statement + "\n")) == 1, why


@pytest.mark.parametrize(
    "statement",
    [
        'element.classList.add("ctx-chip");',                 # classes are how the kit styles things
        'element.setAttribute("data-tone", "good");',         # a tone attribute, not a style
        "element.hidden = true;",                             # visibility through the platform
        'const label = "Remove item";',                       # ordinary prose
        'const green = "Green channel";',                     # a NAMED colour word in prose is fine
        "const style = computeStyleName();",                  # a variable merely called `style`
        "if (a.style === b.style) { return; }",               # a comparison, not a write
    ],
)
def test_legitimate_kit_typescript_is_not_a_finding(tmp_path: Path, statement: str) -> None:
    """The other half of a useful gate: it must not fire on what a kit module is allowed to write.

    The named-colour case is the deliberate asymmetry with the CSS lint. In a CSS value position
    `green` can only be a colour; in a TypeScript string it is prose, and a lint that reddened on a
    label reading "Green channel" would teach authors to work around it.
    """
    assert _run_source_tokens(_kit_src(tmp_path, CLEAN_KIT_TS + "\n" + statement + "\n")) == 0


def test_a_comment_may_name_a_raw_colour(tmp_path: Path) -> None:
    """A gate that fired on its own documentation would teach authors to stop documenting."""
    ts = "// the Dark theme's colors.panel is #0a0a0a, rgb(10, 10, 10)\n" + CLEAN_KIT_TS
    assert _run_source_tokens(_kit_src(tmp_path, ts)) == 0
    block = "/* #0a0a0a\n   rgba(0,0,0,0.5) */\n" + CLEAN_KIT_TS
    assert _run_source_tokens(_kit_src(tmp_path, block)) == 0


def test_a_string_containing_a_double_slash_does_not_hide_the_rest_of_the_line(
    tmp_path: Path,
) -> None:
    """The one-character bypass a regex-based comment strip would have.

    `re.sub(r"//.*", "", text)` deletes from the FIRST `//` to end of line -- including the contents
    of a string that happens to contain one. `const c = "x//" + "#ff0000";` would then be invisible to
    every rule below it on that line. The scanner tracks string state instead, so it is not.
    """
    ts = CLEAN_KIT_TS + '\nconst c = "context-editor://app/" + "#ff0000";\n'
    assert _run_source_tokens(_kit_src(tmp_path, ts)) == 1


def test_a_missing_or_empty_kit_source_directory_is_a_configuration_error(tmp_path: Path) -> None:
    assert _run_source_tokens(tmp_path / "nope") == 2
    empty = tmp_path / "src"
    empty.mkdir()
    assert _run_source_tokens(empty) == 2


# ------------------------------------------------------- the twelve-family roster gate (e06c2)

FULL_ROSTER = """
export const COMPONENT_FAMILIES: readonly ComponentFamily[] = [
%s
];
"""

_ROSTER_ROWS = [
    ("buttons", "createButton", "ctx-button", '"button"'),
    ("fields", "createTextField", "ctx-field", '"textbox"'),
    ("tabs", "createTabs", "ctx-tabs", ""),
    ("trees", "createTree", "ctx-tree", '"tree"'),
    ("tables", "createTable", "ctx-table", ""),
    ("chips", "createChip", "ctx-chip", ""),
    ("badges", "createBadge", "ctx-badge", '"status"'),
    ("toasts", "createToastRegion", "ctx-toast", ""),
    ("empty-states", "createEmptyState", "ctx-empty-state", ""),
    ("skeletons", "createSkeleton", "ctx-skeleton", ""),
    ("dialogs", "createDialog", "ctx-dialog", ""),
    ("tooltips", "createTooltip", "ctx-tooltip", ""),
]

_WIDGETS_TS = """
export const WIDGET_CLASSES: Readonly<Record<string, string>> = {
    button: "ctx-widget-button",
    textbox: "ctx-widget-textbox",
    tree: "ctx-widget-tree",
    status: "ctx-widget-status",
};
"""


def _roster(rows: list[tuple[str, str, str, str]]) -> str:
    body = "\n".join(
        '    { family: "%s", factory: "%s", rootClass: "%s", reusesRoles: [%s] },'
        % (family, factory, root, roles)
        for family, factory, root, roles in rows
    )
    return FULL_ROSTER % body


def _family_case(tmp_path: Path, *, rows: list[tuple[str, str, str, str]] | None = None,
                 drop_factory: str = "", drop_class: str = "") -> int:
    rows = list(_ROSTER_ROWS) if rows is None else rows
    source = tmp_path / "src"
    source.mkdir(parents=True, exist_ok=True)
    (source / "index.ts").write_text(_roster(rows), encoding="utf-8")
    (source / "widgets.ts").write_text(_WIDGETS_TS, encoding="utf-8")
    (source / "families.ts").write_text(
        "\n".join(
            "export function %s(): HTMLElement { return document.createElement('div'); }" % factory
            for _, factory, _, _ in rows
            if factory != drop_factory
        )
        + "\n",
        encoding="utf-8",
    )
    styles = tmp_path / "styles"
    styles.mkdir(parents=True, exist_ok=True)
    (styles / "components.css").write_text(
        "\n".join(
            ".%s {\n    color: var(--ctx-colors-ink);\n}" % root
            for _, _, root, _ in rows
            if root != drop_class
        )
        + "\n",
        encoding="utf-8",
    )
    return check_kit_tokens.main(
        [
            "--family-coverage",
            "--kit-index", str(source / "index.ts"),
            "--kit-source", str(source / "widgets.ts"),
            "--kit-source-dir", str(source),
            "--kit-styles", str(styles),
        ]
    )


def test_the_full_twelve_family_roster_passes(tmp_path: Path) -> None:
    assert _family_case(tmp_path) == 0


def test_a_dropped_family_fails(tmp_path: Path) -> None:
    """The headline failure: the kit quietly shipping eleven of the design's twelve families."""
    assert _family_case(tmp_path, rows=_ROSTER_ROWS[:-1]) == 1


def test_a_family_the_design_does_not_name_fails(tmp_path: Path) -> None:
    rows = list(_ROSTER_ROWS)
    rows[0] = ("buttonz", "createButton", "ctx-button", '"button"')
    assert _family_case(tmp_path, rows=rows) == 1


def test_a_factory_no_module_exports_fails(tmp_path: Path) -> None:
    """A roster that advertises a surface a consumer cannot call."""
    assert _family_case(tmp_path, drop_factory="createTabs") == 1


def test_a_root_class_no_stylesheet_styles_fails(tmp_path: Path) -> None:
    """The family would render in the browser's own chrome, themed by nothing."""
    assert _family_case(tmp_path, drop_class="ctx-dialog") == 1


def test_a_reuse_claim_naming_a_role_that_does_not_exist_fails(tmp_path: Path) -> None:
    """"Builds on the e06c1 primitive" must name a member of the CLOSED widget-class set."""
    rows = list(_ROSTER_ROWS)
    rows[2] = ("tabs", "createTabs", "ctx-tabs", '"tab"')
    assert _family_case(tmp_path, rows=rows) == 1


def test_an_unreadable_roster_is_a_configuration_error(tmp_path: Path) -> None:
    source = tmp_path / "src"
    source.mkdir(parents=True)
    (source / "index.ts").write_text("export const SOMETHING_ELSE = [];\n", encoding="utf-8")
    (source / "widgets.ts").write_text(_WIDGETS_TS, encoding="utf-8")
    styles = tmp_path / "styles"
    styles.mkdir()
    (styles / "components.css").write_text(".ctx-button { color: var(--ctx-colors-ink); }\n",
                                           encoding="utf-8")
    assert check_kit_tokens.main(
        [
            "--family-coverage",
            "--kit-index", str(source / "index.ts"),
            "--kit-source", str(source / "widgets.ts"),
            "--kit-source-dir", str(source),
            "--kit-styles", str(styles),
        ]
    ) == 2


# ------------------------------------------------------------------------------------ the real tree


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def test_the_shipped_kit_is_tokens_only() -> None:
    """The gate, over the REAL kit -- so this suite fails if the shipped stylesheet regresses."""
    styles = _repo_root() / "src" / "editor" / "webui" / "kit" / "styles"
    assert _run_tokens_only(styles) == 0


def test_the_shipped_role_set_agrees_across_all_three_languages() -> None:
    # `--kit-source` names widgets.ts since e06c2: the map moved out of the barrel so the authored
    # families that import it do not put the kit's own entry point inside an import cycle.
    root = _repo_root()
    assert (
        check_kit_tokens.main(
            [
                "--role-coverage",
                "--kit-source", str(root / "src/editor/webui/kit/src/widgets.ts"),
                "--kit-styles", str(root / "src/editor/webui/kit/styles"),
                "--uitree-source", str(root / "src/editor/gui/uitree/src/node.cpp"),
                "--app-stylesheet", str(root / "src/editor/webui/app/app.css"),
            ]
        )
        == 0
    )


def test_the_shipped_role_set_is_the_documented_twelve() -> None:
    """A belt-and-braces count: the closed set is TWELVE, and all three sources say so."""
    root = _repo_root()
    roles = check_kit_tokens.read_cpp_roles(root / "src/editor/gui/uitree/src/node.cpp")
    classes = check_kit_tokens.read_widget_classes(root / "src/editor/webui/kit/src/widgets.ts")
    styled = check_kit_tokens.read_styled_classes(root / "src/editor/webui/kit/styles")
    assert len(roles) == 12
    assert set(classes) == roles
    # The styled set may not GROW past the closed role set even though the kit now ships two
    # stylesheets: `components.css` reaches exactly one widget class (`.ctx-widget-status.ctx-badge`,
    # the live badge reusing the `status` primitive), which is already a member.
    assert set(classes.values()) == styled


def test_the_shipped_kit_typescript_smuggles_no_appearance() -> None:
    """The e06c2 TS half, over the REAL kit -- so this suite fails if a module regresses."""
    root = _repo_root()
    assert (
        check_kit_tokens.main(
            ["--source-tokens", "--kit-source-dir", str(root / "src/editor/webui/kit/src")]
        )
        == 0
    )


def test_the_shipped_kit_publishes_the_twelve_families_of_design_06() -> None:
    root = _repo_root()
    assert (
        check_kit_tokens.main(
            [
                "--family-coverage",
                "--kit-index", str(root / "src/editor/webui/kit/src/index.ts"),
                "--kit-source", str(root / "src/editor/webui/kit/src/widgets.ts"),
                "--kit-source-dir", str(root / "src/editor/webui/kit/src"),
                "--kit-styles", str(root / "src/editor/webui/kit/styles"),
            ]
        )
        == 0
    )


def test_the_shipped_roster_matches_the_designs_list_exactly() -> None:
    """The roster's CONTENT, not merely its agreement with itself.

    `DESIGN_COMPONENT_FAMILIES` is this tool's own copy of design 06 section 3's list (the design
    document lives outside this repository), so comparing the two here is a real cross-check rather
    than a tautology.
    """
    root = _repo_root()
    entries = check_kit_tokens.read_component_families(
        root / "src/editor/webui/kit/src/index.ts"
    )
    assert [family for family, _, _, _ in entries] == list(
        check_kit_tokens.DESIGN_COMPONENT_FAMILIES
    )

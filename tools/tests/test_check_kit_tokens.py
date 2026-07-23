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
                   css: str = COVERED_CSS, app: str = CLEAN_APP_CSS) -> int:
    (tmp_path / "node.cpp").write_text(cpp, encoding="utf-8")
    (tmp_path / "index.ts").write_text(ts, encoding="utf-8")
    (tmp_path / "app.css").write_text(app, encoding="utf-8")
    styles = _kit(tmp_path, css)
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


# ------------------------------------------------------------------------------------ the real tree


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def test_the_shipped_kit_is_tokens_only() -> None:
    """The gate, over the REAL kit -- so this suite fails if the shipped stylesheet regresses."""
    styles = _repo_root() / "src" / "editor" / "webui" / "kit" / "styles"
    assert _run_tokens_only(styles) == 0


def test_the_shipped_role_set_agrees_across_all_three_languages() -> None:
    root = _repo_root()
    assert (
        check_kit_tokens.main(
            [
                "--role-coverage",
                "--kit-source", str(root / "src/editor/webui/kit/src/index.ts"),
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
    classes = check_kit_tokens.read_widget_classes(root / "src/editor/webui/kit/src/index.ts")
    styled = check_kit_tokens.read_styled_classes(root / "src/editor/webui/kit/styles")
    assert len(roles) == 12
    assert set(classes) == roles
    assert set(classes.values()) == styled

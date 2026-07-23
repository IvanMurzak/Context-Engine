#!/usr/bin/env python3
"""The M9 e06c1 component-kit gates: TOKENS-ONLY, and the CLOSED 12-role widget layer.

Two independent checks over the same package, each registered as its own ctest so a failure names
which property broke (`webui-kit-tokens-only` / `webui-kit-role-coverage`). Both are SOURCE scans --
no browser, no build artifact -- so they ride the `webui-*` family on all three default `build` legs
and run locally under a plain `ctest --preset dev`.

--------------------------------------------------------------------------------------------------
``--tokens-only``  (design 06 section 1: "components reference ONLY semantic tokens; raw values live
in themes alone")

Scans every ``*.css`` under the kit's styles directory and fails on:

  * ANY raw colour -- ``#rgb``/``#rrggbb``/``#rrggbbaa``, ``rgb()``/``rgba()``/``hsl()``/``hsla()``,
    or a named CSS colour -- in any property, in any position;
  * a raw value in a TOKENISED PROPERTY FAMILY: fonts (``font``, ``font-family``, ``font-size``,
    ``font-weight``, ``letter-spacing``, ``line-height``, ``font-variant-numeric``), shape
    (``border*``, ``outline*``, ``border-radius``) and motion (``transition*``, ``animation*``);
  * any ``var()`` FALLBACK, and any ``var(--x)`` whose name is not a ``--ctx-`` token.

Declarations are found by their SEPARATOR (``{`` / ``;`` / start of file), never by line position, so
a compactly written rule -- ``.ctx-widget-x { color: #ff0000; background: rgba(0,0,0,.5); }`` -- is
scanned like any other. A line-anchored scan would see only each line's FIRST declaration, which
makes every rule above bypassable by formatting alone.

The last rule is the one that makes the first three hold. A fallback (``var(--ctx-colors-line,
rgba(230, 237, 245, 0.25))`` -- the shape app.css shipped) is a raw literal in kit source with a
token-shaped disguise, and a kit-local ``--`` variable is a second source of truth for a value the
theme already owns. Neither can be reached in production anyway: `boot.ts` applies a theme before the
bridge handshake completes and a panel hydrates only after that, so no widget has ever painted
without the token set present.

WHAT THIS DELIBERATELY DOES NOT COVER, so the omission is a recorded decision rather than a silent
hole: bare box spacing (``padding`` / ``margin`` / ``gap`` lengths). e06a publishes no spacing or
padding token -- design 06 section 1 describes the density group as "control heights, paddings", but
the shipped schema's ``shape.density`` is three control HEIGHTS and ``mockups/TOKENS.md`` section 3
records the spacing scale as an un-adopted PROPOSAL. Linting those lengths today would only force
``calc()`` over a height token: a bypass wearing compliance. ``src/editor/webui/kit/README.md``
carries the follow-up.

--------------------------------------------------------------------------------------------------
``--role-coverage``  (design 04 section 4 step 4: the widget classes ARE kit components)

A three-way agreement no compiler can see, because it spans C++, TypeScript and CSS:

  1. the C++ role vocabulary -- every string ``uitree::role_token`` can return (``node.cpp``);
  2. the kit's ``WIDGET_CLASSES`` map (``kit/src/index.ts``);
  3. the classes the kit stylesheet actually styles.

All three must name the same closed set, in BOTH directions. The failure this prevents is silent by
construction: ``render_html`` emits only the twelve tags ``role_html_tag`` maps to and the hydration
runtime looks each node's role up in the map, so a THIRTEENTH role added to the C++ enum renders
real, interactive, completely unthemed DOM -- no build error, no test failure, nothing in a log.

It additionally asserts that NO ``.ctx-widget-`` rule survives in the app stylesheet -- the "exactly
one styling owner" property. Two stylesheets styling the same class is not a duplicate to tidy later:
it is a cascade whose winner depends on document order, which is decided by CMake staging and by the
runtime injection order of a vendored engine.

Exit codes: 0 = clean, 1 = at least one finding, 2 = a configuration error (bad path, unparseable
source). A configuration error is deliberately NOT 1: a gate that cannot find its inputs must be
distinguishable from a gate that ran and passed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# --------------------------------------------------------------------------------- shared helpers

# `/* ... */` is CSS's only comment form. Stripped before every scan: this file's own prose explains
# the rule it enforces (a raw `#0a0a0a` is named in the kit stylesheet's header), and a gate that
# fired on its own documentation would teach authors to stop documenting.
_CSS_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def _strip_css_comments(text: str) -> str:
    return _CSS_COMMENT.sub(" ", text)


class CheckError(Exception):
    """A configuration error -- a missing input or an unparseable source. Exits 2, never 1."""


# ------------------------------------------------------------------------------- the tokens gate

_HEX_COLOUR = re.compile(r"#[0-9a-fA-F]{3,8}\b")
_COLOUR_FUNCTION = re.compile(r"\b(?:rgba?|hsla?|hwb|lab|lch|oklab|oklch|color)\s*\(", re.IGNORECASE)

# Named CSS colours. Not the full 148: the ones a stylesheet is realistically written with, matched
# as whole words in a VALUE position. `transparent` and `currentcolor` are deliberately absent -- they
# are keywords with no colour of their own, so they cannot bypass the theme.
_NAMED_COLOURS = frozenset(
    """
    aliceblue antiquewhite aqua aquamarine azure beige bisque black blanchedalmond blue blueviolet
    brown burlywood cadetblue chartreuse chocolate coral cornflowerblue cornsilk crimson cyan
    darkblue darkcyan darkgoldenrod darkgray darkgreen darkgrey darkkhaki darkmagenta darkolivegreen
    darkorange darkorchid darkred darksalmon darkseagreen darkslateblue darkslategray darkslategrey
    darkturquoise darkviolet deeppink deepskyblue dimgray dimgrey dodgerblue firebrick floralwhite
    forestgreen fuchsia gainsboro ghostwhite gold goldenrod gray green greenyellow grey honeydew
    hotpink indianred indigo ivory khaki lavender lavenderblush lawngreen lemonchiffon lightblue
    lightcoral lightcyan lightgoldenrodyellow lightgray lightgreen lightgrey lightpink lightsalmon
    lightseagreen lightskyblue lightslategray lightslategrey lightsteelblue lightyellow lime
    limegreen linen magenta maroon mediumaquamarine mediumblue mediumorchid mediumpurple
    mediumseagreen mediumslateblue mediumspringgreen mediumturquoise mediumvioletred midnightblue
    mintcream mistyrose moccasin navajowhite navy oldlace olive olivedrab orange orangered orchid
    palegoldenrod palegreen paleturquoise palevioletred papayawhip peachpuff peru pink plum
    powderblue purple rebeccapurple red rosybrown royalblue saddlebrown salmon sandybrown seagreen
    seashell sienna silver skyblue slateblue slategray slategrey snow springgreen steelblue tan teal
    thistle tomato turquoise violet wheat white whitesmoke yellow yellowgreen
    """.split()
)

# Property families whose values a theme token exists for. Matched against the property NAME, so a
# shorthand (`border`, `outline`, `font`) is covered along with its longhands.
_FONT_PROPERTIES = re.compile(
    r"^(?:font|font-family|font-size|font-weight|font-style|font-stretch|font-variant-numeric"
    r"|letter-spacing|line-height)$"
)
_SHAPE_PROPERTIES = re.compile(r"^(?:border|border-[a-z-]+|outline|outline-[a-z-]+)$")
_MOTION_PROPERTIES = re.compile(r"^(?:transition|transition-[a-z-]+|animation|animation-[a-z-]+)$")

# A raw LENGTH (`3px`, `0.5rem`) and a raw TIME (`120ms`, `2.6s`). A bare `0` is unitless and carries
# no design decision, so it is not a literal in this sense; `%` is relative to the element's own box
# rather than to anything a theme could name.
_RAW_LENGTH = re.compile(r"(?<![\w.-])\d*\.?\d+(?:px|rem|em|pt|pc|ch|ex|vh|vw|vmin|vmax|cm|mm|in|q)\b",
                         re.IGNORECASE)
_RAW_TIME = re.compile(r"(?<![\w.-])\d*\.?\d+m?s\b", re.IGNORECASE)
# An easing curve spelled out rather than taken from `--ctx-motion-easing-*`.
_RAW_EASING = re.compile(r"\b(?:cubic-bezier|steps)\s*\(", re.IGNORECASE)
# A font FAMILY name: a quoted string, or a generic family keyword.
_RAW_FAMILY = re.compile(
    r"""['"]|(?<![\w-])(?:sans-serif|serif|monospace|cursive|fantasy|system-ui|ui-monospace"""
    r"""|ui-sans-serif|ui-serif|ui-rounded)(?![\w-])""",
    re.IGNORECASE,
)
# A bare font WEIGHT number (`600`) -- `font-weight: var(--ctx-typography-weight-semibold)` instead.
# ANY three-digit weight, not just the `n00` ladder: the shipped themes' own weights are 430 / 520 /
# 600, so a rule matching only multiples of 100 would wave through exactly the two literals an author
# copying from `dark.theme.json` is most likely to type. The lookarounds keep it off a decimal
# (`0.011em`), a percentage and anything inside an identifier; only the font family of properties
# reaches this test, so a three-digit length elsewhere is judged by `_RAW_LENGTH` instead.
_RAW_WEIGHT = re.compile(r"(?<![\w.#-])\d{3}(?![\w.%-])")

# `var(` ... the token name ... an optional `,` fallback. Captured so both halves can be judged.
_VAR_CALL = re.compile(r"var\(\s*(--[\w-]+)\s*(,)?")

# One `property: value` declaration.
#
# Anchored on the DECLARATION SEPARATOR -- the `{` that opens a block, the `;` that ends the previous
# declaration, or the start of the file -- and NOT on the start of a LINE. That distinction is the
# gate's non-vacuity: a line-anchored scan reads only the first declaration on each line, so the
# entirely legal one-liner `.ctx-widget-x { color: #ff0000; background: rgba(0,0,0,.5); }` passed the
# whole lint clean. Every rule below is worthless against a stylesheet an author happened to write
# compactly, and e06c2 adds stylesheets to this same directory that inherit the gate by construction.
#
# A SELECTOR cannot be mistaken for a declaration under this anchor: a selector follows `}` or the
# start of a rule list, never `{` or `;`. A pseudo-class (`:hover`) therefore never enters the scan.
_DECLARATION = re.compile(r"(?:^|[{;])\s*([a-zA-Z-]+)\s*:\s*([^;{}]+)")


def _strip_vars(value: str) -> str:
    """Remove every `var(--ctx-...)` reference, leaving only what the author wrote AROUND them.

    The point of the scan is what a value contributes OF ITS OWN; a token reference contributes the
    theme's decision. Removing them first is what lets `border: var(--ctx-shape-border-width) solid
    var(--ctx-colors-line)` read as `solid` (clean) while `border: 1px solid var(--ctx-colors-line)`
    still reads as `1px solid` (a raw length in the shape family).
    """
    out = []
    index = 0
    while True:
        match = _VAR_CALL.search(value, index)
        if match is None:
            out.append(value[index:])
            return "".join(out)
        out.append(value[index : match.start()])
        # Skip to the matching `)`, tracking nesting so a `calc()` inside a fallback cannot end it
        # early. (A fallback is rejected outright elsewhere; this only has to not MIS-parse.)
        depth = 1
        cursor = match.end()
        while cursor < len(value) and depth > 0:
            if value[cursor] == "(":
                depth += 1
            elif value[cursor] == ")":
                depth -= 1
            cursor += 1
        index = cursor


def _colour_findings(prop: str, value: str) -> list[str]:
    findings: list[str] = []
    bare = _strip_vars(value)
    if _HEX_COLOUR.search(bare):
        findings.append(f"`{prop}` carries a raw hex colour")
    if _COLOUR_FUNCTION.search(bare):
        findings.append(f"`{prop}` carries a raw colour function (rgb/hsl/...)")
    for word in re.findall(r"(?<![\w-])[a-zA-Z]+(?![\w-])", bare):
        if word.lower() in _NAMED_COLOURS:
            findings.append(f"`{prop}` carries the named CSS colour `{word}`")
            break
    return findings


def _family_findings(prop: str, value: str) -> list[str]:
    """Font-family literals, judged only where a family can legally appear.

    `_RAW_FAMILY` matches a quote or a GENERIC-FAMILY keyword, and no CSS-wide keyword (`inherit`,
    `normal`, `none`, ...) is either -- so `font: inherit`, the shape the kit actually writes, is
    already clean without a second keyword allowlist to maintain.
    """
    if prop not in {"font", "font-family"}:
        return []
    if _RAW_FAMILY.search(_strip_vars(value)):
        return [f"`{prop}` names a font family directly"]
    return []


def _scan_declaration(prop: str, value: str) -> list[str]:
    prop = prop.lower()
    findings = list(_colour_findings(prop, value))
    bare = _strip_vars(value)

    if _FONT_PROPERTIES.match(prop):
        findings.extend(_family_findings(prop, value))
        if _RAW_LENGTH.search(bare):
            findings.append(f"`{prop}` carries a raw font length")
        if _RAW_WEIGHT.search(bare):
            findings.append(f"`{prop}` carries a raw font weight")
    if _SHAPE_PROPERTIES.match(prop) and _RAW_LENGTH.search(bare):
        findings.append(f"`{prop}` carries a raw length -- the `shape` token group covers it")
    if _MOTION_PROPERTIES.match(prop):
        if _RAW_TIME.search(bare):
            findings.append(f"`{prop}` carries a raw duration -- the `motion` token group covers it")
        if _RAW_EASING.search(bare):
            findings.append(f"`{prop}` carries a raw easing curve -- use `--ctx-motion-easing-*`")

    for name, fallback in _VAR_CALL.findall(value):
        if not name.startswith("--ctx-"):
            findings.append(
                f"`{prop}` reads `{name}`, which is not a `--ctx-` theme token -- a kit-local custom "
                f"property is a second source of truth for a value the theme owns"
            )
        if fallback:
            findings.append(
                f"`{prop}` gives `{name}` a var() FALLBACK -- a fallback is a raw value in kit "
                f"source, and it is unreachable in production (a theme is applied before any panel "
                f"hydrates)"
            )
    return findings


def check_tokens_only(styles_dir: Path) -> list[str]:
    if not styles_dir.is_dir():
        raise CheckError(f"not a directory: {styles_dir}")
    sheets = sorted(styles_dir.rglob("*.css"))
    if not sheets:
        raise CheckError(f"no kit stylesheet under {styles_dir} -- the gate would pass vacuously")

    failures: list[str] = []
    for sheet in sheets:
        text = _strip_css_comments(sheet.read_text(encoding="utf-8", errors="replace"))
        for match in _DECLARATION.finditer(text):
            prop, value = match.group(1), match.group(2)
            # From the PROPERTY's own offset, not the match's: the match starts at the preceding `;`
            # or `{`, which is routinely on the line above.
            line = text.count("\n", 0, match.start(1)) + 1
            for finding in _scan_declaration(prop, value):
                failures.append(f"{sheet.name}:{line}: {finding} -- `{value.strip()}`")
    return failures


# ------------------------------------------------------------------------------ the coverage gate

_CPP_ROLE_TOKEN = re.compile(r"const\s+char\*\s+role_token\s*\(\s*Role\s+\w+\s*\)")
_TS_WIDGET_CLASSES = re.compile(r"export\s+const\s+WIDGET_CLASSES[^=]*=\s*\{(.*?)\n\}", re.DOTALL)
_TS_ENTRY = re.compile(r"(\w+)\s*:\s*\"([^\"]+)\"")
_CSS_WIDGET_SELECTOR = re.compile(r"\.(ctx-widget-[a-z0-9-]+)")


def _function_body(text: str, header: re.Pattern[str], what: str) -> str:
    """The brace-balanced body of the function whose signature `header` matches."""
    match = header.search(text)
    if match is None:
        raise CheckError(f"could not find {what} -- the gate cannot verify what it cannot read")
    start = text.find("{", match.end())
    if start < 0:
        raise CheckError(f"{what} has no body")
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise CheckError(f"{what}'s body is unbalanced")


def read_cpp_roles(node_cpp: Path) -> set[str]:
    if not node_cpp.is_file():
        raise CheckError(f"not a file: {node_cpp}")
    body = _function_body(node_cpp.read_text(encoding="utf-8"), _CPP_ROLE_TOKEN,
                          "uitree::role_token(Role)")
    # EVERY identifier-shaped string the function returns, not just the all-lowercase-letter ones. A
    # role token carrying a digit or a hyphen (`listbox2`, an ARIA `doc-*` role) was previously
    # invisible to this scan, so `cpp_roles` came back SHORT and the thirteenth role produced no
    # finding at all -- the gate reported OK on precisely the failure it exists to catch. Prose is
    # still excluded: a diagnostic string contains spaces and cannot match.
    roles = set(re.findall(r'return\s+"([A-Za-z0-9_-]+)"', body))
    if not roles:
        raise CheckError(f"{node_cpp.name}: role_token() returned no recognisable role tokens")
    return roles


def read_widget_classes(kit_index_ts: Path) -> dict[str, str]:
    if not kit_index_ts.is_file():
        raise CheckError(f"not a file: {kit_index_ts}")
    match = _TS_WIDGET_CLASSES.search(kit_index_ts.read_text(encoding="utf-8"))
    if match is None:
        raise CheckError(f"{kit_index_ts.name}: no `export const WIDGET_CLASSES = {{ ... }}` literal")
    classes = dict(_TS_ENTRY.findall(match.group(1)))
    if not classes:
        raise CheckError(f"{kit_index_ts.name}: WIDGET_CLASSES is empty")
    return classes


def read_styled_classes(styles_dir: Path) -> set[str]:
    if not styles_dir.is_dir():
        raise CheckError(f"not a directory: {styles_dir}")
    styled: set[str] = set()
    for sheet in sorted(styles_dir.rglob("*.css")):
        text = _strip_css_comments(sheet.read_text(encoding="utf-8", errors="replace"))
        styled.update(_CSS_WIDGET_SELECTOR.findall(text))
    return styled


def check_role_coverage(kit_index_ts: Path, styles_dir: Path, node_cpp: Path,
                        app_stylesheet: Path) -> list[str]:
    failures: list[str] = []
    cpp_roles = read_cpp_roles(node_cpp)
    classes = read_widget_classes(kit_index_ts)
    styled = read_styled_classes(styles_dir)

    for role in sorted(cpp_roles - set(classes)):
        failures.append(
            f"the C++ role `{role}` (uitree::role_token) has NO entry in the kit's WIDGET_CLASSES -- "
            f"the hydration runtime renders it with no widget class at all, so it ships unthemed and "
            f"nothing else reports it"
        )
    for role in sorted(set(classes) - cpp_roles):
        failures.append(
            f"the kit maps a role `{role}` that uitree::role_token never emits -- a dead class, or a "
            f"C++ rename that left this map behind"
        )
    for role, css_class in sorted(classes.items()):
        if css_class not in styled:
            failures.append(
                f"role `{role}` maps to `.{css_class}`, which NO kit stylesheet styles -- the closed "
                f"role set grew a member the kit does not dress"
            )
    for css_class in sorted(styled - set(classes.values())):
        failures.append(
            f"the kit styles `.{css_class}`, which no role maps to -- an orphan rule, or a class "
            f"renamed on one side only"
        )

    # The "exactly ONE styling owner" half. A widget rule left behind in the app stylesheet is not a
    # duplicate to tidy later: it is a cascade whose winner depends on staging + injection order.
    if not app_stylesheet.is_file():
        raise CheckError(f"not a file: {app_stylesheet}")
    app_text = _strip_css_comments(app_stylesheet.read_text(encoding="utf-8", errors="replace"))
    for css_class in sorted(set(_CSS_WIDGET_SELECTOR.findall(app_text))):
        failures.append(
            f"{app_stylesheet.name} still styles `.{css_class}` -- the hydration widget layer has "
            f"exactly one owner (the kit), and a second stylesheet makes appearance depend on "
            f"document order rather than on the rules"
        )
    return failures


# ------------------------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tokens-only", action="store_true",
                        help="run the tokens-only lint over the kit stylesheets")
    parser.add_argument("--role-coverage", action="store_true",
                        help="run the closed-role-set + one-styling-owner gate")
    parser.add_argument("--kit-styles", help="the kit's styles directory (kit/styles)")
    parser.add_argument("--kit-source", help="the kit's module entry (kit/src/index.ts)")
    parser.add_argument("--uitree-source", help="the uitree role table (gui/uitree/src/node.cpp)")
    parser.add_argument("--app-stylesheet", help="the app base stylesheet (webui/app/app.css)")
    args = parser.parse_args(argv)

    if not (args.tokens_only or args.role_coverage):
        print("[kit-tokens] ERROR: pass --tokens-only and/or --role-coverage", file=sys.stderr)
        return 2

    failures: list[str] = []
    label = "kit-tokens"
    try:
        if args.tokens_only:
            if args.kit_styles is None:
                raise CheckError("--tokens-only requires --kit-styles")
            failures += check_tokens_only(Path(args.kit_styles))
        if args.role_coverage:
            label = "kit-roles" if not args.tokens_only else label
            missing = [n for n, v in (("--kit-source", args.kit_source),
                                      ("--kit-styles", args.kit_styles),
                                      ("--uitree-source", args.uitree_source),
                                      ("--app-stylesheet", args.app_stylesheet)) if v is None]
            if missing:
                raise CheckError(f"--role-coverage requires {', '.join(missing)}")
            failures += check_role_coverage(Path(args.kit_source), Path(args.kit_styles),
                                            Path(args.uitree_source), Path(args.app_stylesheet))
    except CheckError as error:
        print(f"[{label}] ERROR: {error}", file=sys.stderr)
        return 2

    if failures:
        for failure in failures:
            print(f"[{label}] FINDING: {failure}", file=sys.stderr)
        print(
            f"[{label}] FAILED: {len(failures)} finding(s). Kit stylesheets consume ONLY "
            f"`var(--ctx-*)` theme tokens (design 06 section 1), and the twelve hydration roles are "
            f"a CLOSED set the kit must style exactly once (design 04 section 4). See "
            f"src/editor/webui/kit/README.md.",
            file=sys.stderr,
        )
        return 1

    print(f"[{label}] OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

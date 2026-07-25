"""Tests for tools/check_no_external_begin_frame.py -- the L-41 / cef#4033 pacing gate (R-QA-013).

EVERY CASE BELOW WAS FOUND BY PLANTING, not by reading the pattern (conventions.md § "Authoring a
SOURCE-SCAN gate"). The gate was run against the LIVE tree with each shape spliced into
`src/editor/gui/host/src/editor_host.cpp`, `src/editor/shell/src/compositor.cpp` or the L-41 pin
itself, restored between rounds with a byte copy that does NOT preserve the timestamp, and the round
closed with a post-restore run that had to come back GREEN. Thirteen shapes, all matching
expectation; each is pinned here so a later widening of the patterns cannot silently drop one.

The axes deliberately spanned (the call has more spellings than a naive `\\bAPI\\s*\\(` covers):
the canonical call · a chained getter · a call split across lines · a member-function POINTER that
never writes a paren at all · a runtime string a selector could be built from · the ENABLING flag in
three spellings (plain assignment, C++20 designated initialiser, setter call) · a MUTATION of the
sanctioned handoff in a file that is not the sink · the sink DELETED · the sink FLIPPED.
And the two false-positive controls that matter most, because they are shapes the LIVE tree already
contains: a prose mention inside a comment, and a printf format string naming the field.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from conftest import load_tool

check = load_tool("check_no_external_begin_frame")

_REPO = Path(__file__).resolve().parents[2]

# The sink the gate's check 3 keys on, in its shipped shape. Every synthetic repo carries it so that
# check never colours a result meant to be about checks 1 or 2.
_PIN_FILE = "src/editor/gui/compositor/src/surface.cpp"
_GOOD_PIN = """
namespace context::editor::gui::compositor
{
SurfaceHandoff make_handoff(const SurfacePlan& plan, bool gpu)
{
    SurfaceHandoff handoff;
    handoff.external_begin_frame = false; // L-41: never SendExternalBeginFrame (cef#4033)
    (void)plan;
    (void)gpu;
    return handoff;
}
}
"""


def _repo(tmp_path: Path, files: dict[str, str], *, pin: str | None = _GOOD_PIN) -> Path:
    """Materialize a synthetic repo root: `files` maps a repo-relative path to its content."""
    for rel, body in files.items():
        path = tmp_path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
    if pin is not None:
        pin_path = tmp_path / _PIN_FILE
        pin_path.parent.mkdir(parents=True, exist_ok=True)
        pin_path.write_text(pin, encoding="utf-8")
    return tmp_path


def _run(root: Path) -> int:
    return check.main(["--repo-root", str(root)])


def _findings(root: Path) -> list[str]:
    findings, _scanned = check.scan(root)
    return findings


# --- happy path -----------------------------------------------------------------------------------


def test_clean_tree_passes(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n"})
    assert _run(root) == 0


def test_the_live_tree_is_clean() -> None:
    """The integration pass: the gate must be GREEN on the SHIPPED sources.

    Without this the suite could pass against synthetic fixtures while the real repo violated the
    very rule the gate exists for -- and it is also what keeps the comment-stripping honest against
    the real prose mentions in surface.h / cef_shell.h / the M5 exit gate.
    """
    findings, scanned = check.scan(_REPO)
    assert findings == []
    assert scanned > 100  # a plausible tree was actually walked, not an empty one


# --- check 1: the banned API is not NAMED, in any spelling ----------------------------------------


@pytest.mark.parametrize(
    ("label", "body"),
    [
        # P1 -- the canonical call.
        ("canonical", "void f(Host* host) { host->SendExternalBeginFrame(); }\n"),
        # P2 -- a chained getter, which is how it would actually be written from a CefBrowser.
        ("chained", "void f(Browser* b) { b->GetHost()->SendExternalBeginFrame(); }\n"),
        # P3 -- split across lines. A single-line regex over `->\\s*API\\s*\\(` would still match, but
        # anything anchored to the receiver on the same line would not.
        ("multiline", "void f(Host* host)\n{\n    host->\n        SendExternalBeginFrame(\n        );\n}\n"),
        # P4 -- a member-function POINTER. The OMITS-THE-TOKEN class: there is no call, no paren and
        # no arrow-then-name adjacency; a call-shaped pattern passes this clean.
        ("member pointer", "void f() { auto fn = &CefBrowserHost::SendExternalBeginFrame; (void)fn; }\n"),
        # P5 -- a string a runtime lookup could dispatch through. Same class as P4 from the other
        # direction: the identifier appears with no C++ call syntax around it at all.
        ("runtime string", 'void f() { const char* verb = "SendExternalBeginFrame"; (void)verb; }\n'),
        # A dotted receiver rather than an arrow, for completeness of the receiver axis.
        ("dot receiver", "void f(Host& host) { host.SendExternalBeginFrame(); }\n"),
    ],
)
def test_the_banned_api_is_caught_in_every_spelling(tmp_path: Path, label: str, body: str) -> None:
    root = _repo(tmp_path, {"src/editor/gui/host/src/editor_host.cpp": body})
    findings = _findings(root)
    assert len(findings) == 1, f"{label}: {findings}"
    assert "SendExternalBeginFrame" in findings[0]
    assert "editor_host.cpp" in findings[0]


def test_the_finding_names_the_line(tmp_path: Path) -> None:
    """A gate whose message does not locate the violation costs a tree-wide grep to act on."""
    root = _repo(
        tmp_path,
        {"src/editor/gui/host/src/editor_host.cpp": "\n\n\nvoid f(Host* h) { h->SendExternalBeginFrame(); }\n"},
    )
    assert ":4:" in _findings(root)[0]


# --- check 2: the ENABLING flag is never turned on ------------------------------------------------


@pytest.mark.parametrize(
    ("label", "body"),
    [
        # P6 -- the plain assignment on CefWindowInfo.
        ("assignment", "void f(CefWindowInfo& i) { i.external_begin_frame_enabled = true; }\n"),
        # P7 -- the C++20 designated initialiser. The construction site rather than a later write.
        ("designated init", "void f() { CefWindowInfo i{.external_begin_frame_enabled = true}; (void)i; }\n"),
        # P8 -- a setter call. Neither of the two above sees this one.
        ("setter call", "void f(CefWindowInfo& i) { i.set_external_begin_frame_enabled(true); }\n"),
        # P9 -- a MUTATION of the sanctioned handoff, in a file that is NOT the sink. This is
        # conventions.md clause (5)'s mutation axis: the value was built correctly by make_handoff and
        # is flipped afterwards, so no rule keyed on the construction site can see it.
        ("post-hoc mutation", "void f(SurfaceHandoff& h) { h.external_begin_frame = true; }\n"),
        # The non-`true` true values a C or Objective-C++ caller would write.
        ("integer 1", "void f(CefWindowInfo& i) { i.external_begin_frame_enabled = 1; }\n"),
        ("objc YES", "void f(CefWindowInfo& i) { i.external_begin_frame_enabled = YES; }\n"),
    ],
)
def test_enabling_the_flag_is_caught(tmp_path: Path, label: str, body: str) -> None:
    root = _repo(tmp_path, {"src/editor/shell/src/compositor.cpp": body})
    findings = _findings(root)
    assert len(findings) == 1, f"{label}: {findings}"
    assert "external_begin_frame" in findings[0]
    assert "TRUE" in findings[0]


def test_setting_the_flag_false_is_fine(tmp_path: Path) -> None:
    """The prohibition is on TRUE. A caller that explicitly pins its own copy false is doing the
    right thing and must not be told off for it."""
    root = _repo(
        tmp_path,
        {"src/editor/shell/src/compositor.cpp": "void f(SurfaceHandoff& h) { h.external_begin_frame = false; }\n"},
    )
    assert _findings(root) == []


# --- check 3: the SINK-side positive rule ---------------------------------------------------------


def test_a_deleted_pin_is_a_finding(tmp_path: Path) -> None:
    """P10. Checks 1 and 2 are PROHIBITIONS: both pass trivially against a tree that deleted the
    thing they protect. This is the case that makes the gate more than a pair of greps."""
    root = _repo(
        tmp_path,
        {"src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n"},
        pin="namespace ns { SurfaceHandoff make() { SurfaceHandoff h; return h; } }\n",
    )
    findings = _findings(root)
    assert len(findings) == 1
    assert "EXACTLY ONE assignment" in findings[0]
    assert "found 0" in findings[0]


def test_a_flipped_pin_is_a_finding(tmp_path: Path) -> None:
    """P11. Two findings: check 2 sees the `= true`, check 3 sees the pin's value. Both are wanted --
    they fail for different reasons and a future refactor could silence either one alone."""
    root = _repo(
        tmp_path,
        {"src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n"},
        pin=_GOOD_PIN.replace("= false;", "= true;"),
    )
    findings = _findings(root)
    assert len(findings) == 2
    assert any("not\nfalse" in f.replace(", ", ",\n") or "not false" in f for f in findings)


def test_a_duplicated_pin_is_a_finding(tmp_path: Path) -> None:
    """Two writers of the one value is the shape a merge produces, and it makes 'which one wins'
    a question the reader has to answer from ordering."""
    root = _repo(
        tmp_path,
        {"src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n"},
        pin=_GOOD_PIN + "\nvoid other(SurfaceHandoff& h) { h.external_begin_frame = false; }\n",
    )
    findings = _findings(root)
    assert len(findings) == 1
    assert "found 2" in findings[0]


def test_a_missing_pin_file_is_a_finding(tmp_path: Path) -> None:
    """If the sink moves, the gate must SAY so rather than quietly becoming two prohibitions."""
    root = _repo(tmp_path, {"src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n"}, pin=None)
    findings = _findings(root)
    assert len(findings) == 1
    assert "was not scanned" in findings[0]


# --- the false-positive controls ------------------------------------------------------------------


@pytest.mark.parametrize(
    ("label", "body"),
    [
        # P12 -- prose. The live tree mentions the API in surface.h, cef_shell.h, the M5 exit gate and
        # the viewport tests; a gate that reported those would be switched off the same day.
        ("line comment", "// Never call SendExternalBeginFrame (L-41 / cef#4033).\nint f() { return 0; }\n"),
        ("block comment", "/* external_begin_frame_enabled = true is forbidden by L-41. */\nint f() { return 0; }\n"),
        (
            "trailing comment",
            "int f() { return 0; } // L-41 forbids SendExternalBeginFrame here\n",
        ),
        # P13 -- a printf format string naming the FIELD. editor_host.cpp really does log
        # "(external_begin_frame=%d)", and `%d` is not a true value.
        ("format string", 'void f() { printf("external_begin_frame=%d\\n", 0); }\n'),
        # A read, not a write -- what every existing assertion in the repo does.
        ("assertion read", "void f(const SurfaceHandoff& h) { CHECK(!h.external_begin_frame); }\n"),
    ],
)
def test_comment_and_read_shapes_are_not_findings(tmp_path: Path, label: str, body: str) -> None:
    root = _repo(tmp_path, {"src/editor/gui/host/src/editor_host.cpp": body})
    assert _findings(root) == [], label


def test_a_comment_inside_a_string_is_not_stripped(tmp_path: Path) -> None:
    """The stripper must not treat `//` inside a string literal as the start of a comment -- doing so
    would swallow the rest of the line and could hide a real violation after a URL."""
    body = (
        'void f(Host* h) { const char* url = "https://example.test/"; h->SendExternalBeginFrame(); }\n'
    )
    root = _repo(tmp_path, {"src/editor/gui/host/src/editor_host.cpp": body})
    assert len(_findings(root)) == 1


# --- scanning surface + configuration -------------------------------------------------------------


def test_vendored_and_generated_trees_are_pruned(tmp_path: Path) -> None:
    """The fetched CEF distribution DECLARES SendExternalBeginFrame; it is not this repo's code."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n",
            "src/_cef/include/cef_browser.h": "virtual void SendExternalBeginFrame() = 0;\n",
            "src/editor/webui/node_modules/pkg/index.js": "export const x = 'SendExternalBeginFrame';\n",
        },
    )
    assert _findings(root) == []


def test_a_generated_binary_tree_is_pruned_by_its_cache(tmp_path: Path) -> None:
    """`build` cannot be pruned by NAME -- the repo has a real src/editor/build/ module -- so a
    generated tree is identified by the CMakeCache.txt at its root, exactly as check_cef_staging does."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/src/shell.cpp": "int shell() { return 0; }\n",
            "src/build/dev/CMakeCache.txt": "CMAKE_BUILD_TYPE:STRING=Release\n",
            "src/build/dev/copy.cpp": "void f(Host* h) { h->SendExternalBeginFrame(); }\n",
        },
    )
    assert _findings(root) == []


def test_typescript_sources_are_scanned(tmp_path: Path) -> None:
    """editor-core is TypeScript, and a bridge verb that asked the Shell to pace externally would be
    written there -- so the scan must not be C++-only."""
    root = _repo(
        tmp_path,
        {"src/editor/webui/core/src/index.ts": "export const verb = 'SendExternalBeginFrame';\n"},
    )
    assert len(_findings(root)) == 1


def test_objective_cpp_sources_are_scanned(tmp_path: Path) -> None:
    """The macOS Shell backend (M9 e12b) is Objective-C++, so `.mm` must be in the scanned set."""
    root = _repo(
        tmp_path,
        {"src/editor/shell/src/cocoa_window.mm": "void f(Host* h) { h->SendExternalBeginFrame(); }\n"},
    )
    assert len(_findings(root)) == 1


def test_a_bad_repo_root_is_a_configuration_error(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as excinfo:
        check.scan(tmp_path / "nope")
    assert "no src/ directory" in str(excinfo.value)


def test_an_empty_src_is_a_configuration_error(tmp_path: Path) -> None:
    """An empty scan set would report `clean` over nothing -- the vacuous pass this refuses."""
    (tmp_path / "src").mkdir()
    with pytest.raises(SystemExit) as excinfo:
        check.scan(tmp_path)
    assert "no source files" in str(excinfo.value)


def test_main_returns_one_on_a_finding(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {"src/editor/gui/host/src/editor_host.cpp": "void f(Host* h) { h->SendExternalBeginFrame(); }\n"},
    )
    assert _run(root) == 1

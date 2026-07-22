"""Tests for tools/check_no_raw_key_handlers.py — the M9 e07d raw-key-handler lint (R-QA-013).

Covers the happy path (clean sources), every FINDING shape (addEventListener + onkey* property, single
and double quotes), the `key-handler-ok:` marker escape hatch (same line + look-back window), the
test-source exemption, and the configuration-error exit — plus an integration pass over the LIVE
editor-core sources so the lint stays honest against the shipped tree.
"""

from __future__ import annotations

from pathlib import Path

from conftest import load_tool

check = load_tool("check_no_raw_key_handlers")

_CORE_SRC = (
    Path(__file__).resolve().parents[2] / "src" / "editor" / "webui" / "core" / "src"
)


def _write(root: Path, name: str, body: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")


def test_clean_sources_pass(tmp_path: Path) -> None:
    _write(tmp_path, "a.ts", "el.addEventListener('click', onClick);\nfoo.bar = 1;\n")
    assert check.main(["--sources", str(tmp_path)]) == 0


def test_raw_keydown_listener_fails(tmp_path: Path) -> None:
    _write(tmp_path, "a.ts", 'window.addEventListener("keydown", onKey);\n')
    assert check.main(["--sources", str(tmp_path)]) == 1


def test_single_quote_and_keyup_keypress_fail(tmp_path: Path) -> None:
    _write(tmp_path, "a.ts", "el.addEventListener('keyup', h);\n")
    assert check.main(["--sources", str(tmp_path)]) == 1
    _write(tmp_path, "b.ts", 'el.addEventListener("keypress", h);\n')
    assert check.main(["--sources", str(tmp_path)]) == 1


def test_onkey_property_assignment_fails(tmp_path: Path) -> None:
    _write(tmp_path, "a.ts", "element.onkeydown = handler;\n")
    assert check.main(["--sources", str(tmp_path)]) == 1


def test_marker_on_same_line_is_allowed(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "a.ts",
        'el.addEventListener("keydown", h); // key-handler-ok: ARIA activation, dispatches a command\n',
    )
    assert check.main(["--sources", str(tmp_path)]) == 0


def test_marker_in_lookback_window_is_allowed(tmp_path: Path) -> None:
    body = (
        "// key-handler-ok: palette list navigation while open, not a global shortcut\n"
        'input.addEventListener("keydown", this.onKeyDown);\n'
    )
    _write(tmp_path, "a.ts", body)
    assert check.main(["--sources", str(tmp_path)]) == 0


def test_marker_too_far_above_does_not_help(tmp_path: Path) -> None:
    body = (
        "// key-handler-ok: this justification is too far from the attach site\n"
        "const a = 1;\n"
        "const b = 2;\n"
        "const c = 3;\n"
        'el.addEventListener("keydown", h);\n'
    )
    _write(tmp_path, "a.ts", body)
    assert check.main(["--sources", str(tmp_path)]) == 1


def test_test_sources_are_exempt(tmp_path: Path) -> None:
    # A key handler under a test/ directory is not shipped and must not trip the lint.
    _write(tmp_path, "test/a.test.ts", 'x.addEventListener("keydown", h);\n')
    # And a non-test sibling keeps the scan from short-circuiting on "no sources".
    _write(tmp_path, "b.ts", "const ok = true;\n")
    assert check.main(["--sources", str(tmp_path)]) == 0


def test_missing_directory_is_a_config_error(tmp_path: Path) -> None:
    assert check.main(["--sources", str(tmp_path / "nope")]) == 2


def test_empty_directory_is_a_config_error(tmp_path: Path) -> None:
    (tmp_path / "empty").mkdir()
    assert check.main(["--sources", str(tmp_path / "empty")]) == 2


def test_live_editor_core_sources_are_clean() -> None:
    """The shipped editor-core tree must pass its own lint (the integration guard)."""
    assert _CORE_SRC.is_dir(), f"editor-core sources not found at {_CORE_SRC}"
    assert check.main(["--sources", str(_CORE_SRC)]) == 0

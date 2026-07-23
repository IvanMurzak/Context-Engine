"""Tests for tools/check_config_writers.py — the C-F14 single-writer gate (M9 e06d, R-QA-013).

The point of this suite is MUTATION COVERAGE, not happy-path coverage: a gate whose only test is "the
live tree passes" is indistinguishable from a gate that always passes. So every rule is exercised by
planting the violation it exists to catch into a synthetic tree, and the live repository is then
checked once at the end (it must pass, and its anchors must be where the tool believes they are).
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

check_config_writers = load_tool("check_config_writers")

REPO_ROOT = Path(__file__).resolve().parents[2]


def make_tree(tmp_path: Path) -> Path:
    """A minimal tree carrying the two anchors the tool requires, and nothing else."""
    writer = tmp_path / check_config_writers.SOLE_WRITER
    writer.parent.mkdir(parents=True, exist_ok=True)
    writer.write_text(
        'bool write_user_config(const fs::path& p) { std::ofstream out(p); return true; }\n',
        encoding="utf-8",
    )
    header = tmp_path / check_config_writers.WRITER_HEADER
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("bool write_user_config(const fs::path&);\n", encoding="utf-8")
    client = tmp_path / check_config_writers.CONFIG_SET_OWNER
    client.parent.mkdir(parents=True, exist_ok=True)
    client.write_text('export const CONFIG_SET_METHOD = "config.set";\n', encoding="utf-8")
    return tmp_path


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------------
# the baseline: a tree with only the sanctioned writer passes
# ---------------------------------------------------------------------------


def test_clean_tree_passes(tmp_path):
    root = make_tree(tmp_path)
    assert check_config_writers.check(root) == []


def test_missing_anchor_is_a_config_error(tmp_path):
    """A moved anchor must FAIL LOUDLY, never pass silently — the whole point of exit 2."""
    (tmp_path / "src").mkdir()
    with pytest.raises(FileNotFoundError):
        check_config_writers.check(tmp_path)
    assert check_config_writers.main(["--repo-root", str(tmp_path)]) == 2


# ---------------------------------------------------------------------------
# rule 1 — exactly one C++ writer
# ---------------------------------------------------------------------------


def test_second_cpp_writer_is_caught(tmp_path):
    """The pre-e06d shape, reproduced: welcome.cpp read the config AND wrote it itself."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp",
          'auto doc = read_user_config(path);\n'
          'std::ofstream out(path, std::ios::binary);\n')
    violations = check_config_writers.check(root)
    assert len(violations) == 1
    assert "welcome.cpp" in violations[0]
    assert "ONE writer" in violations[0]


def test_second_writer_via_rename_is_caught(tmp_path):
    """Publishing by rename is a write too — staging elsewhere does not launder it."""
    root = make_tree(tmp_path)
    write(root, "src/cli/config_command.cpp",
          'const auto p = user_config_path();\nfs::rename(temp, p, ec);\n')
    assert any("config_command.cpp" in v for v in check_config_writers.check(root))


def test_a_reader_is_not_a_writer(tmp_path):
    """Reading the document is unrestricted — only WRITING is funnelled."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp", "auto doc = read_user_config(path);\n")
    assert check_config_writers.check(root) == []


def test_an_unrelated_writer_is_not_flagged(tmp_path):
    """A file that writes something else entirely never names the config, so it is none of our
    business — the gate must not become a general ban on std::ofstream."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/editor_state.cpp",
          'std::ofstream out(".editor/editor-state.json");\n')
    assert check_config_writers.check(root) == []


def test_a_test_fixture_writer_is_not_flagged(tmp_path):
    """A test authoring its own temp config is an input, not a second product writer."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/tests/test_user_config.cpp",
          'std::ofstream out(config);\nauto doc = read_user_config(config);\n')
    assert check_config_writers.check(root) == []


# ---------------------------------------------------------------------------
# rule 2 — editor-core carries no client-side persistence
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "snippet",
    [
        'localStorage.setItem("theme", id);',
        'sessionStorage.setItem("theme", id);',
        "const db = indexedDB.open(\"ctx\");",
        'import { writeFileSync } from "node:fs";',
        'const fs = require("fs");',
    ],
)
def test_client_side_persistence_is_caught(tmp_path, snippet):
    """The shortcut a Settings-panel author reaches for. It works, and it is a second store."""
    root = make_tree(tmp_path)
    write(root, "src/editor/webui/core/src/settings.ts", snippet + "\n")
    violations = check_config_writers.check(root)
    assert len(violations) == 1
    assert "settings.ts" in violations[0]


def test_persistence_named_only_in_a_comment_is_not_a_violation(tmp_path):
    """editorstate.ts's own header says "no localStorage here" — prose about the rule is not a breach."""
    root = make_tree(tmp_path)
    write(root, "src/editor/webui/core/src/editorstate.ts",
          "// There is no fetch, no localStorage, no file API here - only the bridge.\n"
          "export const EDITOR_STATE_GET_METHOD = \"editor.state.get\";\n")
    assert check_config_writers.check(root) == []


def test_persistence_in_a_ts_test_is_still_caught(tmp_path):
    """Rule 2 applies to tests too: there is no legitimate browser-store use in this workspace."""
    root = make_tree(tmp_path)
    write(root, "src/editor/webui/core/src/test/settings.test.ts", 'localStorage.clear();\n')
    assert any("settings.test.ts" in v for v in check_config_writers.check(root))


# ---------------------------------------------------------------------------
# rule 3 — one door for the write request
# ---------------------------------------------------------------------------


def test_second_config_set_caller_is_caught(tmp_path):
    root = make_tree(tmp_path)
    write(root, "src/editor/webui/core/src/settings.ts",
          'await bridge.call("config.set", { key: "theme", value: id });\n')
    violations = check_config_writers.check(root)
    assert len(violations) == 1
    assert "config.set" in violations[0]


def test_the_typed_client_may_name_it(tmp_path):
    root = make_tree(tmp_path)  # the owner module already names it
    assert check_config_writers.check(root) == []


# ---------------------------------------------------------------------------
# the live repository
# ---------------------------------------------------------------------------


def test_live_repo_has_exactly_one_writer():
    assert check_config_writers.check(REPO_ROOT) == []


def test_live_anchors_exist():
    """Pinned separately from the scan: if these move, every rule above silently stops applying."""
    assert (REPO_ROOT / check_config_writers.SOLE_WRITER).is_file()
    assert (REPO_ROOT / check_config_writers.WRITER_HEADER).is_file()
    assert (REPO_ROOT / check_config_writers.CONFIG_SET_OWNER).is_file()

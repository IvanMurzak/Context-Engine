"""Tests for tools/check_session_ownership.py — the C-F3 session-file ownership gate (M9 e09d).

MUTATION COVERAGE, not happy-path coverage: a gate whose only test is "the live tree passes" is
indistinguishable from a gate that always passes. Every rule is exercised by planting the violation
it exists to catch into a synthetic tree, and the live repository is checked once at the end.

EVERY CASE BELOW THAT CARRIES A `# FOUND BY PLANTING` NOTE IS A REAL GAP THIS GATE ONCE HAD. The
first revision passed its own reading and was defeated, in one round against the real tree, by three
ordinary ways of spelling a filename and two ways of making its anti-vacuity half report a writer
that no longer writes. Those five are pinned here so no future edit can reintroduce them, per
`conventions.md` § "Authoring a SOURCE-SCAN gate" clause (3).
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

gate = load_tool("check_session_ownership")
config_writers = load_tool("check_config_writers")

REPO_ROOT = Path(__file__).resolve().parents[2]

SESSION = ".editor/session.json"
EDITOR_STATE = ".editor/editor-state.json"


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_tree(tmp_path: Path) -> Path:
    """A minimal tree carrying every anchor the tool requires, and nothing else."""
    write(
        tmp_path,
        gate.SOLE_WRITERS[SESSION][0],
        'fs::path session_state_path(const fs::path& r) { return r / ".editor" / "session.json"; }\n'
        "bool persist_session_state(const fs::path& r) { std::ofstream out(session_state_path(r));"
        " return true; }\n",
    )
    write(tmp_path, gate.WRITER_HEADERS[SESSION], "bool persist_session_state(const fs::path&);\n")
    write(
        tmp_path,
        gate.SOLE_WRITERS[EDITOR_STATE][0],
        "fs::path editor_state_path(const fs::path& r) { return r / \".editor\" /"
        ' "editor-state.json"; }\n'
        "bool write_state(const fs::path& r) { std::ofstream out(editor_state_path(r));"
        " return true; }\n",
    )
    write(tmp_path, gate.WRITER_HEADERS[EDITOR_STATE], "class EditorStateStore;\n")
    for owner in gate.GATEWAY_OWNERS:
        write(tmp_path, owner, "class ProjectOverrideWriteGateway {};\n")
    for reader, (label, _reason) in gate.DOCUMENTED_READERS.items():
        write(
            tmp_path,
            reader,
            'auto text = read_file(project / ".editor" / "editor-state.json");\n'
            "std::ofstream out(focus_request_path(project));\n"
            if label == EDITOR_STATE
            else 'auto text = read_file(project / ".editor" / "session.json");\n'
            "std::ofstream out(other_path(project));\n",
        )
    return tmp_path


# ---------------------------------------------------------------------------
# baseline + the config-error contract
# ---------------------------------------------------------------------------


def test_clean_tree_passes(tmp_path):
    assert gate.check(make_tree(tmp_path)) == []


def test_missing_anchor_is_a_config_error(tmp_path):
    """A moved anchor must FAIL LOUDLY, never pass silently — the whole point of exit 2."""
    (tmp_path / "src").mkdir()
    with pytest.raises(FileNotFoundError):
        gate.check(tmp_path)
    assert gate.main(["--repo-root", str(tmp_path)]) == 2


def test_repo_root_that_is_not_a_directory_is_exit_2(tmp_path):
    missing = tmp_path / "nope"
    assert gate.main(["--repo-root", str(missing)]) == 2


# ---------------------------------------------------------------------------
# rules 1 + 2 — exactly one C++ writer per session file, over the WRITE-SPELLING axis
#
# The six C++ spellings that defeated the sibling `check_config_writers.py` gate in its own review
# (a `std::fstream` + `ios::out`, `copy_file`, `fopen(p,"w")`, the C library's `std::rename`, an
# unqualified `ofstream`, and the `basic_ofstream` template named directly) are each pinned here
# against BOTH session files. The two remaining e06d evaders were JavaScript quote styles for a wire
# method this gate has no analogue of — its file-NAME spelling axis below is the same class of hole.
# ---------------------------------------------------------------------------


SECOND_WRITER_SPELLINGS = [
    ("std_ofstream", "std::ofstream out(p);"),
    ("unqualified_ofstream", "ofstream out(p);"),
    ("basic_ofstream", "std::basic_ofstream<char> out(p);"),
    ("fstream_ios_out", "std::fstream s(p, std::ios::out);"),
    ("fs_rename", "fs::rename(tmp, p);"),
    ("std_rename", "std::rename(tmp, p);"),
    ("copy_file", "std::filesystem::copy_file(src, p);"),
    ("copy_options_overwrite", "std::filesystem::copy(src, p, copy_options::overwrite_existing);"),
    ("fopen_w", 'std::fopen(p, "w");'),
    ("freopen_w", 'std::freopen(p, "w", stream);'),
    ("atomic_write_wrapper", "filesync::atomic_write(p, bytes);"),
    ("write_file_wrapper", "write_file(p, bytes);"),
]


@pytest.mark.parametrize("name,spelling", SECOND_WRITER_SPELLINGS, ids=[n for n, _ in SECOND_WRITER_SPELLINGS])
def test_second_writer_of_editor_state_is_caught(tmp_path, name, spelling):
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp",
          'const auto p = project / ".editor" / "editor-state.json";\n' + spelling + "\n")
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "welcome.cpp" in violations[0]
    assert "ONE writer" in violations[0]


@pytest.mark.parametrize("name,spelling", SECOND_WRITER_SPELLINGS, ids=[n for n, _ in SECOND_WRITER_SPELLINGS])
def test_second_writer_of_session_json_is_caught(tmp_path, name, spelling):
    root = make_tree(tmp_path)
    write(root, "src/cli/src/some_command.cpp",
          'const auto p = project / ".editor" / "session.json";\n' + spelling + "\n")
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "some_command.cpp" in violations[0]


def test_write_spelling_list_is_a_superset_of_the_sibling_gate(tmp_path):
    """DRIFT TRIPWIRE: a write spelling one gate has learned, the other must know.

    Both gates guard "exactly one writer of file X" and both are defeated by the same spelling. They
    keep separate patterns (they are separate scripts with separate blast radii), so this asserts the
    containment rather than a shared import: every spelling `check_config_writers.CPP_WRITE` matches,
    this gate's `CPP_WRITE` must match too.
    """
    corpus = [
        "std::ofstream out(p);",
        "ofstream out(p);",
        "std::basic_ofstream<char> out(p);",
        "std::fstream s(p, std::ios::out);",
        "fs::rename(a, b);",
        "std::rename(a, b);",
        "std::filesystem::copy_file(a, b);",
        'std::fopen(p, "w");',
        'std::freopen(p, "w", s);',
    ]
    for line in corpus:
        if config_writers.CPP_WRITE.search(line) is not None:
            assert gate.CPP_WRITE.search(line) is not None, (
                f"{line!r} is a write spelling check_config_writers.py knows and this gate does not"
            )


# ---------------------------------------------------------------------------
# rules 1 + 2 — the FILE-NAME spelling axis
#
# FOUND BY PLANTING. The first revision matched the document name WITH its opening quote
# (`"editor-state\.json"`), which reads as correctly anchored and was defeated by all three of these
# against the real tree. None is evasive; each is a spelling a person writes without thinking.
# ---------------------------------------------------------------------------


FILE_NAME_SPELLINGS = [
    ("path_prefix_in_one_literal", 'std::ofstream o(root / ".editor/editor-state.json");'),
    ("raw_string_literal", 'std::ofstream o(root / R"(editor-state.json)");'),
    ("windows_backslash_literal", 'std::ofstream o(root / ".editor\\\\editor-state.json");'),
    ("via_the_path_helper", "std::ofstream o(editor_state_path(root));"),
    ("via_the_quarantine_helper", "std::ofstream o(editor_state_quarantine_path(root, 0));"),
]


@pytest.mark.parametrize("name,line", FILE_NAME_SPELLINGS, ids=[n for n, _ in FILE_NAME_SPELLINGS])
def test_file_name_spelling_is_caught(tmp_path, name, line):
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp", line + "\n")
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "welcome.cpp" in violations[0]


@pytest.mark.parametrize(
    "line",
    [
        'std::ofstream o("snapshot.session.json");',   # the C-F4 runtime `session *` file family
        'std::ofstream o("trace.session.json");',
        'std::ofstream o("my-editor-state.json");',
        'std::ofstream o("legacy_editor-state.json");',
    ],
)
def test_a_different_file_of_a_similar_name_is_not_a_hit(tmp_path, line):
    """C-F4 keeps the deterministic `session *` FILE-harness family distinct from `.editor/`."""
    root = make_tree(tmp_path)
    write(root, "src/runtime/session/src/save.cpp", line + "\n")
    assert gate.check(root) == []


# ---------------------------------------------------------------------------
# rules 1 + 2 — the shapes that must NOT be reported
# ---------------------------------------------------------------------------


def test_a_reader_that_writes_nothing_is_not_a_hit(tmp_path):
    root = make_tree(tmp_path)
    write(root, "src/cli/src/doctor_command.cpp",
          'std::ifstream in(project / ".editor" / "editor-state.json");\n')
    assert gate.check(root) == []


def test_a_writer_that_names_neither_file_is_not_a_hit(tmp_path):
    root = make_tree(tmp_path)
    write(root, "src/cli/src/doctor_command.cpp", 'std::ofstream out("report.json");\n')
    assert gate.check(root) == []


def test_comments_are_not_code(tmp_path):
    """This repo comments heavily, and several headers explain the split BY NAMING BOTH FILES."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp",
          "// editor-state.json is written with std::ofstream by the store, never here.\n"
          "/* session.json belongs to the daemon; std::rename( is its business. */\n")
    assert gate.check(root) == []


def test_an_include_is_not_a_write(tmp_path):
    """FOUND BY PLANTING: `#include <fstream>` matched CPP_WRITE's generous `\\bfstream\\b` token and
    reported `arbitration.cpp`'s include line as a second writer of a file it never opens."""
    root = make_tree(tmp_path)
    write(root, "src/cli/src/doctor_command.cpp",
          "#include <fstream>\n"
          'auto text = read_file(project / ".editor" / "editor-state.json");\n')
    assert gate.check(root) == []


def test_tests_and_smokes_may_author_fixtures(tmp_path):
    """Rules 1-3 are about what SHIPS. A test authoring a corrupt fixture is not a second writer, and
    FOUND BY PLANTING: the `*_smoke` ctest executables are tests too — `cef_shell_restore_smoke.cpp`
    writes a layout ORACLE beside the real file and was reported as a second writer of it."""
    root = make_tree(tmp_path)
    for rel in (
        "src/editor/shell/tests/test_editor_state.cpp",
        "src/tests/integration/test_e08a_daemon_session_state.cpp",
        "src/editor/shell/smoke/shell_smoke_main.cpp",
        "src/editor/shell/cef/src/cef_shell_restore_smoke.cpp",
    ):
        write(root, rel,
              'std::ofstream out(root / ".editor" / "editor-state.json");\n'
              'std::ofstream out2(root / ".editor" / "session.json");\n')
    assert gate.check(root) == []


# ---------------------------------------------------------------------------
# rule 3 — the SPLIT itself: each owner in its own process's subtree
# ---------------------------------------------------------------------------


def test_cross_process_write_is_named_as_such(tmp_path):
    """The daemon writing the Shell's file is a second writer AND a torn write across processes —
    the message must say so, because 'add a call to the other module' is the wrong fix here."""
    root = make_tree(tmp_path)
    write(root, "src/editor/editorkernel/src/kernel_server.cpp",
          'std::ofstream out(project / ".editor" / "editor-state.json");\n')
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "CROSS-PROCESS" in violations[0]


def test_the_reverse_cross_process_write_is_named_as_such(tmp_path):
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/src/welcome.cpp",
          'std::ofstream out(project / ".editor" / "session.json");\n')
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "CROSS-PROCESS" in violations[0]


def test_an_owner_moved_out_of_its_process_subtree_is_caught(tmp_path, monkeypatch):
    """Rules 1-2 would BOTH still pass with one writer per file if the Shell's writer moved into the
    daemon — one writer each, and the split dead. This is the rule that notices."""
    root = make_tree(tmp_path)
    moved = "src/editor/editorkernel/src/editor_state.cpp"
    write(root, moved, root.joinpath(gate.SOLE_WRITERS[EDITOR_STATE][0]).read_text(encoding="utf-8"))
    (root / gate.SOLE_WRITERS[EDITOR_STATE][0]).unlink()
    monkeypatch.setitem(gate.SOLE_WRITERS, EDITOR_STATE, (moved, "src/editor/shell/"))
    violations = gate.check(root)
    assert any("OWNING PROCESS" in v for v in violations), violations


# ---------------------------------------------------------------------------
# rule 4 — the in-process compose/filesync gateway stays off the live path
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "line",
    [
        'gui::viewport::ProjectOverrideWriteGateway gateway(project);',
        '#include "context/editor/gui/viewport/project_override_gateway.h"',
    ],
)
def test_in_process_gateway_named_from_product_code_is_caught(tmp_path, line):
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/app/editor_main.cpp", line + "\n")
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert "IN-PROCESS" in violations[0]


@pytest.mark.parametrize(
    "call",
    [
        "target_link_libraries(context_editor_shell PRIVATE context_gui_viewport_edit)",
        "target_link_libraries(context_editor PUBLIC context_gui_viewport_edit context_warnings)",
        # split across lines — the CMake formatting a real link block actually uses
        "target_link_libraries(context_editor_panels\n    PRIVATE\n      context_gui_viewport_edit\n"
        "      context_warnings)",
    ],
)
def test_a_shell_target_linking_the_gateway_library_is_caught(tmp_path, call):
    """Rule 4b — the LINK half. The `context_assert_shell_boundary` FORBIDDEN set in
    src/CMakeLists.txt is the stronger (transitive) mechanism, but this task's DoD requires that file
    to stay byte-identical, so the direct-link guard is scanned from the CMake sources here."""
    root = make_tree(tmp_path)
    write(root, "src/editor/shell/CMakeLists.txt", call + "\n")
    violations = gate.check(root)
    assert len(violations) == 1, violations
    assert gate.GATEWAY_LIBRARY in violations[0]
    assert "IN-PROCESS" in violations[0]


@pytest.mark.parametrize(
    "call",
    [
        # the a11y registry links it TODAY and legitimately — it is a headless harness, not the Shell
        "target_link_libraries(context_gui_a11y PRIVATE context_gui_viewport_edit)",
        # a Shell target linking something ELSE is not a hit
        "target_link_libraries(context_editor_shell PRIVATE context_gui_uitree context_warnings)",
        # a similarly-named library is a different library
        "target_link_libraries(context_editor_shell PRIVATE context_gui_viewport)",
    ],
)
def test_a_legitimate_link_is_not_a_hit(tmp_path, call):
    root = make_tree(tmp_path)
    write(root, "src/editor/gui/a11y/CMakeLists.txt", call + "\n")
    assert gate.check(root) == []


def test_in_process_gateway_is_reachable_from_tests(tmp_path):
    """"Unreachable outside T1 mocks" makes the tests the ONE legitimate caller — exempting them is
    not a loophole in the rule, it IS the rule."""
    root = make_tree(tmp_path)
    write(root, "src/tests/integration/test_viewport_override_parity.cpp",
          "vp::ProjectOverrideWriteGateway gateway(stage);\n")
    assert gate.check(root) == []


# ---------------------------------------------------------------------------
# rule 5 — the ANTI-VACUITY half: the prohibitions above are all trivially true of an empty tree
# ---------------------------------------------------------------------------


def test_a_sole_writer_that_stopped_writing_is_caught(tmp_path):
    """FOUND BY PLANTING (twice). The first revision checked this with the WIDE write pattern, which
    a writer TU satisfies through its OWN helper's definition (`atomic_write_text(`) even after every
    real write is gone; it now checks the PRIMITIVE spellings only."""
    root = make_tree(tmp_path)
    write(root, gate.SOLE_WRITERS[EDITOR_STATE][0],
          "fs::path editor_state_path(const fs::path& r) { return r /"
          ' ".editor" / "editor-state.json"; }\n'
          "bool write_state(const fs::path& r) { return atomic_write_text(editor_state_path(r)); }\n")
    violations = gate.check(root)
    assert any("no longer performs a file write" in v for v in violations), violations


def test_a_sole_writer_that_stopped_naming_its_file_is_caught(tmp_path):
    """FOUND BY PLANTING. The first revision checked this with FILE_NAMES, which also matches the
    file's own path HELPERS — and a writer TU always contains those, because it DEFINES them. So
    redirecting `session_state_path` at another filename left the gate green while the daemon's
    session file had silently moved. It now keys on the document LITERAL."""
    root = make_tree(tmp_path)
    write(root, gate.SOLE_WRITERS[SESSION][0],
          'fs::path session_state_path(const fs::path& r) { return r / ".editor" / "elsewhere.dat"; }\n'
          "bool persist_session_state(const fs::path& r) { std::ofstream o(session_state_path(r));"
          " return true; }\n")
    violations = gate.check(root)
    assert any("no longer names" in v for v in violations), violations


# ---------------------------------------------------------------------------
# rule 5b — a DOCUMENTED READER's two obligations
# ---------------------------------------------------------------------------


def test_a_stale_reader_exemption_is_reported(tmp_path):
    """An exemption that no longer applies is deleted, not left behind as a standing licence."""
    root = make_tree(tmp_path)
    reader = next(iter(gate.DOCUMENTED_READERS))
    write(root, reader, "std::ofstream out(focus_request_path(project));\n")
    violations = gate.check(root)
    assert any("stale" in v for v in violations), violations


def test_a_reader_that_reaches_the_owner_store_is_reported(tmp_path):
    """The one way an exemption could quietly become the second writer it was granted for not being."""
    root = make_tree(tmp_path)
    reader = next(iter(gate.DOCUMENTED_READERS))
    write(root, reader,
          'auto text = read_file(project / ".editor" / "editor-state.json");\n'
          "shell::EditorStateStore store(project);\n")
    violations = gate.check(root)
    assert any("OWNING WRITE API" in v for v in violations), violations


# ---------------------------------------------------------------------------
# the live repository
# ---------------------------------------------------------------------------


def test_the_live_repository_passes():
    """Last, and deliberately not first: a green live tree means nothing until the cases above have
    shown the gate can go red at all."""
    assert gate.check(REPO_ROOT) == []
    assert gate.main(["--repo-root", str(REPO_ROOT)]) == 0

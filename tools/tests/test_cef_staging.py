"""Tests for tools/check_cef_staging.py -- the issue-#360 CEF single-staging lint (R-QA-013).

Covers the happy path (one writer per staging destination), the exact defect the lint exists to stop
(FOUR targets POST_BUILD-copying the identical payload into one ${CEF_TARGET_OUT_DIR}, reconstructed
here as the real pre-fix shape), the subdirectory-inherits-the-parent's-destination rule that makes
that defect visible at all, the consumer-completeness check, every masking construct forbidden in the
staging implementation, the comment-stripping and non-CEF-destination edge cases, and the
configuration-error exit -- plus an integration pass over the LIVE tree so the lint stays honest
against the shipped build files.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from conftest import load_tool

check = load_tool("check_cef_staging")

_REPO = Path(__file__).resolve().parents[2]

# A minimal but faithful staging implementation: the shape check 3 requires (stamp-guarded OUTPUT
# form, no masking). Tests that exercise checks 1/2 use this so check 3 never colours their result.
_GOOD_IMPL = """
function(context_cef_stage_payload stage_target out_dir)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${stage_target}.stamp"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_RESOURCE_DIR}/locales" "${out_dir}/locales"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/${stage_target}.stamp"
        DEPENDS "${CEF_RESOURCE_DIR}/locales"
        VERBATIM)
    add_custom_target(${stage_target} DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${stage_target}.stamp")
endfunction()
"""


def _repo(tmp_path: Path, files: dict[str, str], *, impl: str | None = _GOOD_IMPL) -> Path:
    """Materialize a synthetic repo root: `files` maps a repo-relative path to its content."""
    for rel, body in files.items():
        path = tmp_path / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body, encoding="utf-8")
    if impl is not None:
        impl_path = tmp_path / "cmake" / "ContextCef.cmake"
        impl_path.parent.mkdir(parents=True, exist_ok=True)
        impl_path.write_text(impl, encoding="utf-8")
    return tmp_path


def _run(root: Path) -> int:
    return check.main(["--repo-root", str(root)])


# --- happy paths ----------------------------------------------------------------------------------


def test_single_copy_files_writer_is_clean(tmp_path: Path) -> None:
    """One acquiring directory, one executable staging into it -- the src/editor/cef/ shape."""
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                "add_executable(context_cef_boot_smoke src/boot.cpp)\n"
                "SET_EXECUTABLE_TARGET_PROPERTIES(context_cef_boot_smoke)\n"
                'COPY_FILES(context_cef_boot_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            )
        },
    )
    assert _run(root) == 0


def test_stage_target_with_all_consumers_is_clean(tmp_path: Path) -> None:
    """The post-fix shape: one stage target, every executable depends on it -- including one declared
    in a SUBDIRECTORY, which inherits the parent's ${CEF_TARGET_OUT_DIR}."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "add_subdirectory(cef)\n"
                "SET_EXECUTABLE_TARGET_PROPERTIES(context_editor)\n"
                "add_dependencies(context_editor libcef_dll_wrapper)\n"
                "add_dependencies(context_editor context_editor_cef_stage)\n"
            ),
            "src/editor/shell/cef/CMakeLists.txt": (
                "SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_cef_smoke)\n"
                "add_dependencies(context_editor_shell_cef_smoke context_editor_cef_stage)\n"
            ),
        },
    )
    assert _run(root) == 0


def test_no_cef_anywhere_is_clean(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"src/kernel/CMakeLists.txt": "add_library(context_kernel STATIC a.cpp)\n"})
    assert _run(root) == 0


# --- check 1: the issue-#360 defect itself ----------------------------------------------------------


def test_four_writers_into_one_destination_fail(tmp_path: Path) -> None:
    """The EXACT pre-fix shape of issue #360: context_editor plus the three live smokes each attach
    their own POST_BUILD copy of the identical payload into the SAME inherited destination."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                "add_subdirectory(cef)\n"
                'COPY_FILES(context_editor "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            ),
            "src/editor/shell/cef/CMakeLists.txt": (
                'COPY_FILES(context_editor_shell_cef_smoke "${CEF_BINARY_FILES}"'
                ' "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")\n'
                'COPY_FILES(context_editor_shell_restore_smoke "${CEF_RESOURCE_FILES}"'
                ' "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")\n'
                'COPY_FILES(context_editor_shell_palette_smoke "${CEF_BINARY_FILES}"'
                ' "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")\n'
            ),
        },
    )
    findings, _ = check.scan(root)
    assert len(findings) == 1
    assert "4 targets stage the CEF payload into the SAME" in findings[0]
    # Every colliding writer is named, wherever it was declared.
    for target in (
        "context_editor",
        "context_editor_shell_cef_smoke",
        "context_editor_shell_restore_smoke",
        "context_editor_shell_palette_smoke",
    ):
        assert target in findings[0]
    assert _run(root) == 1


def test_copy_files_alongside_a_stage_target_fails(tmp_path: Path) -> None:
    """A re-introduced per-target copy is a SECOND writer even when a stage target already serves the
    destination -- the regression path back into #360."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
            ),
            "src/editor/shell/cef/CMakeLists.txt": (
                "SET_EXECUTABLE_TARGET_PROPERTIES(context_new_smoke)\n"
                "add_dependencies(context_new_smoke context_editor_cef_stage)\n"
                'COPY_FILES(context_new_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            ),
        },
    )
    findings, _ = check.scan(root)
    assert any("2 targets stage the CEF payload" in f for f in findings)
    assert _run(root) == 1


def test_sibling_destinations_do_not_collide(tmp_path: Path) -> None:
    """Two directories that each acquire CEF have DIFFERENT ${CEF_TARGET_OUT_DIR} values, so one
    writer apiece is correct -- this is why src/editor/cef/ and src/editor/gui/host/ never raced."""
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                'COPY_FILES(context_cef_boot_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            ),
            "src/editor/gui/host/CMakeLists.txt": (
                "context_acquire_cef(context_gui_host editor-cef-smoke)\n"
                'COPY_FILES(context_gui_host "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            ),
        },
    )
    assert _run(root) == 0


def test_copy_files_without_an_acquiring_scope_fails(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/stray/CMakeLists.txt": (
                'COPY_FILES(context_stray "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            )
        },
    )
    findings, _ = check.scan(root)
    assert any("no context_acquire_cef()" in f for f in findings)
    assert _run(root) == 1


# --- check 2: consumer completeness -----------------------------------------------------------------


def test_executable_missing_the_stage_dependency_fails(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
            ),
            "src/editor/shell/cef/CMakeLists.txt": (
                "SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_new_smoke)\n"
                "add_dependencies(context_editor_shell_new_smoke libcef_dll_wrapper)\n"
            ),
        },
    )
    findings, _ = check.scan(root)
    assert len(findings) == 1
    assert "context_editor_shell_new_smoke" in findings[0]
    assert "does not" in findings[0]
    assert _run(root) == 1


def test_two_stage_targets_for_one_destination_fail(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(stage_a "${CEF_TARGET_OUT_DIR}")\n'
                'context_cef_stage_payload(stage_b "${CEF_TARGET_OUT_DIR}")\n'
            )
        },
    )
    findings, _ = check.scan(root)
    assert any("two stage targets" in f for f in findings)
    assert _run(root) == 1


# --- check 3: the staging implementation ------------------------------------------------------------


_STAGED_TREE = {
    "src/editor/shell/CMakeLists.txt": (
        "context_acquire_cef(context_editor editor-cef-smoke)\n"
        'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
    )
}


@pytest.mark.parametrize(
    ("injected", "expected"),
    [
        ('COMMAND cmd /c "copy a b || exit 0"', "`||` fallback"),
        ("COMMAND ${CMAKE_COMMAND} -E sleep 1", "a sleep"),
        ("COMMAND ${CMAKE_COMMAND} -P retry_copy.cmake", "a retry"),
        ("COMMAND timeout 5 cp a b", "a timeout"),
        ('COMMAND cmd /c "copy a b & exit 0"', "`exit 0`"),
        ("execute_process(COMMAND cp a b ERROR_QUIET)", "ERROR_QUIET"),
    ],
)
def test_masking_constructs_in_the_staging_body_fail(
    tmp_path: Path, injected: str, expected: str
) -> None:
    """Masking the race is explicitly forbidden: it leaves a partially staged output directory."""
    impl = _GOOD_IMPL.replace("    add_custom_command(", f"    {injected}\n    add_custom_command(")
    root = _repo(tmp_path, dict(_STAGED_TREE), impl=impl)
    findings, _ = check.scan(root)
    assert any(expected in f for f in findings), findings
    assert _run(root) == 1


def test_post_build_staging_fails(tmp_path: Path) -> None:
    """POST_BUILD is both per-target (the race) and unconditional (no incrementality)."""
    impl = """
function(context_cef_stage_payload stage_target out_dir)
    add_custom_command(TARGET ${stage_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_RESOURCE_DIR}" "${out_dir}")
endfunction()
"""
    root = _repo(tmp_path, dict(_STAGED_TREE), impl=impl)
    findings, _ = check.scan(root)
    assert any("POST_BUILD" in f for f in findings)
    assert _run(root) == 1


def test_staging_without_a_stamp_output_fails(tmp_path: Path) -> None:
    impl = """
function(context_cef_stage_payload stage_target out_dir)
    add_custom_target(${stage_target}
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CEF_RESOURCE_DIR}" "${out_dir}")
endfunction()
"""
    root = _repo(tmp_path, dict(_STAGED_TREE), impl=impl)
    findings, _ = check.scan(root)
    assert any("not incremental" in f for f in findings)
    assert _run(root) == 1


def test_missing_staging_implementation_fails(tmp_path: Path) -> None:
    root = _repo(tmp_path, dict(_STAGED_TREE), impl=None)
    findings, _ = check.scan(root)
    assert any("is missing" in f for f in findings)
    assert _run(root) == 1


def test_implementation_is_not_audited_when_no_stage_target_is_used(tmp_path: Path) -> None:
    """A tree that stages the old single-writer way must not be failed by the implementation checks."""
    impl = "function(context_cef_stage_payload stage_target out_dir)\n    # sleep\nendfunction()\n"
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                'COPY_FILES(context_cef_boot_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
            )
        },
        impl=impl,
    )
    assert _run(root) == 0


# --- parsing edge cases ------------------------------------------------------------------------------


def test_commented_out_calls_are_ignored(tmp_path: Path) -> None:
    """Prose and commented-out code must not count as call sites -- both CMakeLists in this repo
    mention COPY_FILES and SET_EXECUTABLE_TARGET_PROPERTIES inside comments."""
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                'COPY_FILES(context_cef_boot_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
                ' "${CEF_TARGET_OUT_DIR}")\n'
                "# COPY_FILES(context_other \"x\" \"y\" \"${CEF_TARGET_OUT_DIR}\") -- an old note\n"
                "# SET_EXECUTABLE_TARGET_PROPERTIES, COPY_FILES -- in the INCLUDING scope.\n"
            )
        },
    )
    assert _run(root) == 0


def test_hash_inside_a_quoted_string_is_not_a_comment(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                'message(STATUS "see issue #360") # a real comment\n'
                "context_acquire_cef(context_cef cef-substrate)\n"
                'COPY_FILES(a "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")\n'
                'COPY_FILES(b "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}")\n'
            )
        },
    )
    # Both COPY_FILES survive comment-stripping, so the collision is reported.
    assert _run(root) == 1


def test_copy_files_to_another_destination_is_ignored(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                'COPY_FILES(a "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${SOME_OTHER_DIR}")\n'
                'COPY_FILES(b "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${SOME_OTHER_DIR}")\n'
            )
        },
    )
    assert _run(root) == 0


def test_multiline_copy_files_call_is_parsed(tmp_path: Path) -> None:
    """The shell's calls wrapped the destination onto its own line -- the pre-fix formatting."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": "context_acquire_cef(context_editor editor-cef-smoke)\n",
            "src/editor/shell/cef/CMakeLists.txt": (
                'COPY_FILES(a "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"\n'
                '           "${CEF_TARGET_OUT_DIR}")\n'
                'COPY_FILES(b "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}"\n'
                '           "${CEF_TARGET_OUT_DIR}")\n'
            ),
        },
    )
    assert _run(root) == 1


# --- configuration errors -----------------------------------------------------------------------------


def test_missing_src_directory_is_a_configuration_error(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as excinfo:
        check.scan(tmp_path)
    assert "no src/ directory" in str(excinfo.value)


def test_empty_src_directory_is_a_configuration_error(tmp_path: Path) -> None:
    (tmp_path / "src").mkdir()
    with pytest.raises(SystemExit) as excinfo:
        check.scan(tmp_path)
    assert "no CMakeLists.txt" in str(excinfo.value)


# --- integration: the live tree ------------------------------------------------------------------------


def test_live_repository_is_clean() -> None:
    """The shipped build files must satisfy the invariant -- this is the tripwire, not a smoke test."""
    findings, scanned = check.scan(_REPO)
    assert findings == []
    assert scanned > 1


def _live_destinations() -> tuple[dict[Path, set[str]], set[Path]]:
    """(staging destination -> its writer targets, acquire dirs) over the live source tree."""
    src = _REPO / "src"
    texts = {
        p.parent: check.strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        for p in check.source_cmakelists(src)
    }
    acquire_dirs = {d for d, t in texts.items() if check._ACQUIRE.search(t)}
    writers: dict[Path, set[str]] = {d: set() for d in acquire_dirs}
    for directory, text in texts.items():
        root = check.stage_root(directory, acquire_dirs)
        if root is None:
            continue
        for m in check._COPY_FILES.finditer(text):
            if "CEF_TARGET_OUT_DIR" in m.group(2):
                writers[root].add(m.group(1))
        for m in check._STAGE_CALL.finditer(text):
            writers[root].add(m.group(1))
    return writers, acquire_dirs


def test_live_repository_enumerates_every_cef_staging_destination() -> None:
    """Pin the WHOLE ripple set, not just the one the issue named.

    A staging destination is one `context_acquire_cef()` directory (SET_CEF_TARGET_OUT_DIR uses
    CMAKE_CURRENT_BINARY_DIR, and subdirectories inherit the plain variable). There are exactly three,
    and they are DISTINCT paths -- which is why only the shell's, shared by four executables, ever
    raced. Measured, not assumed: build/dev/editor/cef/Release, build/dev/editor/gui/host/Release and
    build/dev/editor/shell/Release (the last one inherited by src/editor/shell/cef/). If a fourth CEF
    call site ever lands, this test fails and forces the single-writer decision to be made for it too.
    """
    writers, acquire_dirs = _live_destinations()
    src = _REPO / "src"
    assert acquire_dirs == {
        src / "editor" / "cef",
        src / "editor" / "gui" / "host",
        src / "editor" / "shell",
    }
    # Exactly one writer per destination -- the whole invariant, across the whole sweep. The two
    # single-executable directories still use COPY_FILES() and are CORRECT as they stand: one writer
    # into a destination nobody else writes cannot race itself. (src/editor/cef/ names its target
    # through a ${variable}; the writer identity is the call site either way.)
    assert writers[src / "editor" / "cef"] == {"${_cef_boot_target}"}
    assert writers[src / "editor" / "gui" / "host"] == {"${_host_target}"}
    assert writers[src / "editor" / "shell"] == {"context_editor_cef_stage"}


def test_live_repository_has_exactly_one_shell_stage_writer() -> None:
    """Non-vacuity: assert the lint actually SEES the shell destination and finds one writer there,
    so a future refactor that hides the call sites cannot turn `clean` into `nothing scanned`."""
    src = _REPO / "src"
    texts = {
        p.parent: check.strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        for p in src.rglob("CMakeLists.txt")
    }
    acquire_dirs = {d for d, t in texts.items() if check._ACQUIRE.search(t)}
    assert src / "editor" / "shell" in acquire_dirs

    shell_root = check.stage_root(src / "editor" / "shell" / "cef", acquire_dirs)
    assert shell_root == src / "editor" / "shell", "cef/ must inherit the shell's staging destination"

    stage_calls = [
        m.group(1) for t in texts.values() for m in check._STAGE_CALL.finditer(t)
    ]
    assert stage_calls == ["context_editor_cef_stage"]

    # All four consumers named in issue #360 take the dependency.
    deps: dict[str, set[str]] = {}
    for text in texts.values():
        for m in check._ADD_DEPENDENCIES.finditer(text):
            deps.setdefault(m.group(1), set()).update(m.group(2).split())
    for consumer in (
        "context_editor",
        "context_editor_shell_cef_smoke",
        "context_editor_shell_restore_smoke",
        "context_editor_shell_palette_smoke",
    ):
        assert "context_editor_cef_stage" in deps.get(consumer, set()), consumer

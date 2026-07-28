"""Tests for tools/check_cef_staging.py -- the issue-#360 CEF single-staging lint (R-QA-013).

Covers the happy path (one writer per staging destination), the exact defect the lint exists to stop
(FOUR targets POST_BUILD-copying the identical payload into one ${CEF_TARGET_OUT_DIR}, reconstructed
here as the real pre-fix shape), the subdirectory-inherits-the-parent's-destination rule that makes
that defect visible at all, the consumer-completeness check, every masking construct forbidden in the
staging implementation, the comment-stripping and non-CEF-destination edge cases, and the
configuration-error exit -- plus an integration pass over the LIVE tree so the lint stays honest
against the shipped build files.

M9 e12c-1 (issue #436) added three groups, each pinning something the lint got WRONG before macOS CEF
targets existed. Every case marked `# FOUND BY PLANTING` was produced by mutating the LIVE build files
and watching what the lint said (conventions.md, Coding conventions § "Authoring a SOURCE-SCAN gate"):

  * PLATFORM AWARENESS -- `context_cef_stage_payload()` is Windows/Linux-only, so a macOS CEF
    executable must NOT be required to depend on a stage target that does not exist on its platform.
    Reading the file as a flat list of call sites demanded exactly that and would have redded
    `editor-shell-cef-staging` on all three OS legs. Both directions are pinned: the macOS-only exe is
    CLEAN, and the Windows/Linux exe that drops its edge still FAILS.
  * `${variable}` NAME RESOLUTION -- the two older macOS branches name every target through a
    `${var}`, and the original literal-only pattern made them INVISIBLE rather than correct. A name
    that still cannot be resolved is now REPORTED where a check would have applied to it.
  * THE macOS APP-BUNDLE HALF (check 4) -- an .app's `Contents/Frameworks` is a staging destination in
    the same sense check 1 means, so it takes the same single-writer rule.
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


# --- platform awareness (M9 e12c-1) ------------------------------------------------------------------
# The stage target is created under `if(OS_WINDOWS OR OS_LINUX)`; a macOS CEF bundle embeds its payload
# instead. These pin BOTH directions, because the fix's whole risk is that it silences check 2.


_MAC_AND_DESKTOP_TREE = {
    "src/editor/shell/CMakeLists.txt": (
        "context_acquire_cef(context_editor editor-cef-smoke)\n"
        "if(OS_WINDOWS OR OS_LINUX)\n"
        '    context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
        "endif()\n"
        "add_subdirectory(cef)\n"
    ),
    # ⚠ THE macOS EXECUTABLE HAS ITS OWN NAME HERE, and that is the load-bearing part of the fixture.
    # A macOS branch that re-declares the SAME target as the Windows/Linux branch (which is what the
    # live tree does for the two shell smokes) is NOT a discriminating case: the shared name picks up
    # the Windows/Linux branch's own `add_dependencies` edge, so it reads clean whether or not the lint
    # understands platforms at all. MEASURED — the first version of this fixture reused one name and
    # stayed GREEN under a plant that disabled platform awareness entirely.
    "src/editor/shell/cef/CMakeLists.txt": (
        "if(OS_WINDOWS OR OS_LINUX)\n"
        "    SET_EXECUTABLE_TARGET_PROPERTIES(context_shell_smoke)\n"
        "    add_dependencies(context_shell_smoke context_editor_cef_stage)\n"
        "elseif(OS_MAC)\n"
        "    SET_EXECUTABLE_TARGET_PROPERTIES(context_shell_smoke_mac)\n"
        '    COPY_MAC_FRAMEWORK(context_shell_smoke_mac "${CEF_BINARY_DIR}"'
        ' "${CEF_TARGET_OUT_DIR}/context_shell_smoke_mac.app")\n'
        "endif()\n"
    ),
}


def test_macos_only_cef_exe_needs_no_windows_linux_stage_dependency(tmp_path: Path) -> None:
    """# FOUND BY PLANTING: this is the shape that redded all three legs before e12c-1.

    A macOS-only CEF executable carries its payload inside its own .app; the stage target does not
    exist on that platform, so requiring an `add_dependencies()` edge onto it is impossible to satisfy.
    """
    root = _repo(tmp_path, dict(_MAC_AND_DESKTOP_TREE))
    findings, _ = check.scan(root)
    assert findings == []
    assert _run(root) == 0


def test_windows_linux_cef_exe_still_needs_the_stage_dependency(tmp_path: Path) -> None:
    """Non-vacuity of the case above: dropping the Windows/Linux edge must STILL fail."""
    files = dict(_MAC_AND_DESKTOP_TREE)
    files["src/editor/shell/cef/CMakeLists.txt"] = files[
        "src/editor/shell/cef/CMakeLists.txt"
    ].replace("    add_dependencies(context_shell_smoke context_editor_cef_stage)\n", "")
    root = _repo(tmp_path, files)
    findings, _ = check.scan(root)
    assert len(findings) == 1
    assert "does not depend on it there" in findings[0]
    assert "linux/windows" in findings[0]
    assert _run(root) == 1


def test_a_macos_only_stage_target_is_demanded_of_macos_only_exes(tmp_path: Path) -> None:
    """The rule is symmetric: it is the INTERSECTION of platform sets that decides, not `mac`."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                "if(OS_MAC)\n"
                '    context_cef_stage_payload(context_mac_stage "${CEF_TARGET_OUT_DIR}")\n'
                "    SET_EXECUTABLE_TARGET_PROPERTIES(context_mac_exe)\n"
                "endif()\n"
            )
        },
    )
    findings, _ = check.scan(root)
    assert any("context_mac_exe" in f and "mac" in f for f in findings)
    assert _run(root) == 1


def test_a_non_platform_guard_does_not_narrow_anything(tmp_path: Path) -> None:
    """Behaviour preservation: `if(NOT CONTEXT_BUILD_GUI_CEF)`-style guards say nothing about the
    platform axis, so everything inside them keeps the full requirement it had before e12c-1."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "if(CONTEXT_BUILD_GUI_CEF)\n"
                "    SET_EXECUTABLE_TARGET_PROPERTIES(context_guarded_exe)\n"
                "endif()\n"
            )
        },
    )
    findings, _ = check.scan(root)
    assert any("context_guarded_exe" in f for f in findings)
    assert _run(root) == 1


@pytest.mark.parametrize(
    ("condition", "expected"),
    [
        ("APPLE", {"mac"}),
        ("OS_MAC", {"mac"}),
        ("OS_WINDOWS OR OS_LINUX", {"windows", "linux"}),
        ("CONTEXT_BUILD_GUI_CEF AND (OS_WINDOWS OR OS_LINUX)", {"windows", "linux"}),
        ("UNIX AND NOT APPLE", {"linux"}),
        ("NOT APPLE", {"windows", "linux"}),
        ("WIN32", {"windows"}),
        ("UNIX", {"linux", "mac"}),
        # A comparison is swallowed whole, so its right-hand side is never read as another atom.
        ('CMAKE_SYSTEM_NAME STREQUAL "Haiku" AND OS_MAC', {"mac"}),
        ("TARGET libcef_lib AND OS_MAC", {"mac"}),
    ],
)
def test_platform_conditions_evaluate_to_their_platform_set(condition: str, expected: set) -> None:
    assert check._eval_condition(condition) == frozenset(expected)


@pytest.mark.parametrize(
    "condition", ["CONTEXT_BUILD_GUI_CEF", "NOT CONTEXT_BUILD_GUI_CEF", "NOT TARGET libcef_lib"]
)
def test_conditions_naming_no_platform_narrow_nothing(condition: str) -> None:
    """None -- NOT the empty set. `if(NOT X)` must not evaluate to "no platform" and silently drop
    every call site inside it."""
    assert check._eval_condition(condition) is None


def test_else_keeps_the_enclosing_platform_set(tmp_path: Path) -> None:
    """`else()` deliberately does not invert: over-broad can only ADD a requirement, never hide one."""
    text = check.strip_comments(
        "if(OS_WINDOWS OR OS_LINUX)\nA\nelse()\nB\nendif()\nC\n"
    )
    offsets = check.platform_map(text)
    assert check.platform_at(offsets, text.index("A")) == frozenset({"windows", "linux"})
    assert check.platform_at(offsets, text.index("B")) == check.ALL_PLATFORMS
    assert check.platform_at(offsets, text.index("C")) == check.ALL_PLATFORMS


# --- ${variable} target names (M9 e12c-1) ------------------------------------------------------------


def test_a_set_resolved_target_name_is_matched_to_its_dependency(tmp_path: Path) -> None:
    """The two older macOS branches name their targets this way; before e12c-1 they were INVISIBLE."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "set(_exe context_named_by_variable)\n"
                "SET_EXECUTABLE_TARGET_PROPERTIES(${_exe})\n"
                "add_dependencies(${_exe} context_editor_cef_stage)\n"
            )
        },
    )
    assert _run(root) == 0


def test_a_set_resolved_target_name_missing_its_dependency_fails_under_its_real_name(
    tmp_path: Path,
) -> None:
    """Non-vacuity of the case above -- and the finding must name the RESOLVED target, which is the
    name a reader can act on."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "set(_exe context_named_by_variable)\n"
                "SET_EXECUTABLE_TARGET_PROPERTIES(${_exe})\n"
            )
        },
    )
    findings, _ = check.scan(root)
    assert len(findings) == 1
    assert "context_named_by_variable" in findings[0]
    assert "${" not in findings[0]
    assert _run(root) == 1


def test_an_unresolvable_target_name_at_a_staged_destination_is_reported_not_skipped(
    tmp_path: Path,
) -> None:
    """A foreach-composed name cannot be verified -- so say so, rather than dropping it. Silently
    dropping it is exactly how the macOS half went uncovered."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                'context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "set(_base context_helper)\n"
                'foreach(_suffix "_gpu" "_renderer")\n'
                '    SET_EXECUTABLE_TARGET_PROPERTIES(${_base}${_suffix})\n'
                "endforeach()\n"
            )
        },
    )
    findings, _ = check.scan(root)
    assert len(findings) == 1
    assert "cannot resolve" in findings[0]
    assert "context_helper${_suffix}" in findings[0]
    assert _run(root) == 1


def test_an_unresolvable_name_on_a_platform_with_no_stage_target_is_not_reported(
    tmp_path: Path,
) -> None:
    """The live macOS helper families are exactly this: unresolvable names, but on the platform that
    has no stage target at all -- so there is nothing to verify and nothing to report."""
    root = _repo(
        tmp_path,
        {
            "src/editor/shell/CMakeLists.txt": (
                "context_acquire_cef(context_editor editor-cef-smoke)\n"
                "if(OS_WINDOWS OR OS_LINUX)\n"
                '    context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")\n'
                "endif()\n"
                "if(OS_MAC)\n"
                "    set(_base context_helper)\n"
                '    foreach(_suffix "_gpu" "_renderer")\n'
                "        SET_EXECUTABLE_TARGET_PROPERTIES(${_base}${_suffix})\n"
                "    endforeach()\n"
                "endif()\n"
            )
        },
    )
    assert _run(root) == 0


def test_set_variables_reads_only_the_single_value_form() -> None:
    values = check.set_variables(
        'set(plain context_editor)\n'
        'set(quoted "context editor")\n'
        'set(a_list one two three)\n'
        'set(composed "${plain}_Helper")\n'
    )
    assert values["plain"] == "context_editor"
    assert values["quoted"] == "context editor"
    assert "a_list" not in values
    assert check.resolve("${composed}", values) == "context_editor_Helper"
    assert check.is_unresolved(check.resolve("${nope}_Helper", values))


# --- check 4: the macOS app-bundle payload (M9 e12c-1) -----------------------------------------------


_MAC_BUNDLE_TREE = {
    "src/editor/shell/CMakeLists.txt": (
        "context_acquire_cef(context_editor editor-cef-smoke)\n"
        "if(OS_MAC)\n"
        '    set(_app "${CEF_TARGET_OUT_DIR}/context_editor.app")\n'
        '    COPY_MAC_FRAMEWORK(context_editor "${CEF_BINARY_DIR}" "${_app}")\n'
        "    add_custom_command(TARGET context_editor POST_BUILD\n"
        "        COMMAND ${CMAKE_COMMAND} -E copy_directory\n"
        '                "$<TARGET_BUNDLE_DIR:context_editor_Helper>"\n'
        '                "${_app}/Contents/Frameworks/context_editor Helper.app"\n'
        "        VERBATIM)\n"
        "endif()\n"
    )
}


def test_one_owning_target_per_app_bundle_is_clean(tmp_path: Path) -> None:
    """The shipped shape: the framework embed and every helper copy hang off the app's OWN target, so
    they serialise inside one POST_BUILD chain."""
    root = _repo(tmp_path, dict(_MAC_BUNDLE_TREE))
    assert _run(root) == 0


def test_two_targets_embedding_into_one_app_bundle_fail(tmp_path: Path) -> None:
    """# FOUND BY PLANTING: the macOS form of issue #360 -- two POST_BUILD writers, one directory."""
    files = dict(_MAC_BUNDLE_TREE)
    files["src/editor/shell/CMakeLists.txt"] = files["src/editor/shell/CMakeLists.txt"].replace(
        '    COPY_MAC_FRAMEWORK(context_editor "${CEF_BINARY_DIR}" "${_app}")\n',
        '    COPY_MAC_FRAMEWORK(context_editor "${CEF_BINARY_DIR}" "${_app}")\n'
        '    COPY_MAC_FRAMEWORK(context_editor_cef "${CEF_BINARY_DIR}" "${_app}")\n',
    )
    root = _repo(tmp_path, files)
    findings, _ = check.scan(root)
    assert any("targets write into this bundle's Contents/Frameworks" in f for f in findings)
    # The platform OVERLAP is what decides this check, so the finding must NAME it, the way check 1's
    # sibling message does. Asserted on the rendered listing rather than on a bare "mac" substring,
    # which the message's own "the macOS form of issue #360" would satisfy vacuously.
    assert any("(framework, src/editor/shell, mac)" in f for f in findings)
    assert _run(root) == 1


def test_a_framework_embedded_into_someone_elses_bundle_fails(tmp_path: Path) -> None:
    """# FOUND BY PLANTING. The embed must be attached to the target that OWNS the .app; a helper or a
    sibling doing it is a second writer into a directory it does not own."""
    files = dict(_MAC_BUNDLE_TREE)
    files["src/editor/shell/CMakeLists.txt"] = files["src/editor/shell/CMakeLists.txt"].replace(
        '"${CEF_BINARY_DIR}" "${_app}")', '"${CEF_BINARY_DIR}" "${CEF_TARGET_OUT_DIR}/other.app")'
    )
    root = _repo(tmp_path, files)
    findings, _ = check.scan(root)
    assert any("is not context_editor's own .app" in f for f in findings)
    assert _run(root) == 1


def test_an_app_bundle_with_helpers_but_no_framework_fails(tmp_path: Path) -> None:
    """# FOUND BY PLANTING: an .app with helper bundles and no embedded framework cannot boot --
    CefScopedLibraryLoader finds nothing to load, at run time, on one OS."""
    files = dict(_MAC_BUNDLE_TREE)
    files["src/editor/shell/CMakeLists.txt"] = files["src/editor/shell/CMakeLists.txt"].replace(
        '    COPY_MAC_FRAMEWORK(context_editor "${CEF_BINARY_DIR}" "${_app}")\n', ""
    )
    root = _repo(tmp_path, files)
    findings, _ = check.scan(root)
    assert any("receives helper bundles but no COPY_MAC_FRAMEWORK" in f for f in findings)
    assert _run(root) == 1


def test_a_malformed_copy_mac_framework_call_is_reported(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "src/editor/cef/CMakeLists.txt": (
                "context_acquire_cef(context_cef cef-substrate)\n"
                "COPY_MAC_FRAMEWORK(context_cef_boot_smoke)\n"
            )
        },
    )
    findings, _ = check.scan(root)
    assert any("COPY_MAC_FRAMEWORK takes" in f for f in findings)
    assert _run(root) == 1


def test_balanced_paren_extraction_survives_a_nested_call(tmp_path: Path) -> None:
    """add_custom_command bodies are long and may carry parentheses; a `[^)]*` body would truncate."""
    body = next(
        args
        for _start, args in check.calls(
            'add_custom_command(TARGET t POST_BUILD\n'
            '    COMMAND sh -c "printf (x)"\n'
            '    COMMAND cmake -E copy_directory "a" "${_app}/Contents/Frameworks/h.app"\n'
            "    VERBATIM)\n",
            "add_custom_command",
        )
    )
    assert "Contents/Frameworks" in body


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
    """(staging destination -> its writer targets, acquire dirs) over the live source tree.

    Mirrors `scan()`, INCLUDING the `${var}` resolution e12c-1 added — otherwise this helper would
    still report the pre-e12c-1 raw names and quietly disagree with the lint it is checking.
    """
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
        values = check.set_variables(text)
        for m in check._COPY_FILES.finditer(text):
            if "CEF_TARGET_OUT_DIR" in m.group(2):
                writers[root].add(check.resolve(m.group(1), values))
        for m in check._STAGE_CALL.finditer(text):
            writers[root].add(check.resolve(m.group(1), values))
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
    # into a destination nobody else writes cannot race itself.
    #
    # ⚠ These two are the ones that used to read `${_cef_boot_target}` / `${_host_target}`, which is the
    # visible trace of the pre-e12c-1 hole: the lint saw an opaque STRING, not a target, so nothing it
    # checked applied to those directories' macOS halves. They now resolve to the real target names.
    assert writers[src / "editor" / "cef"] == {"context_cef_boot_smoke"}
    assert writers[src / "editor" / "gui" / "host"] == {"context_gui_host"}
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


# --- check 5: the hand-written CEF-hosting roster (M9 x11) -----------------------------------------
# `_ctx_cef_shell_executables` is what BOTH configure-time CEF audits iterate, so a target missing
# from it is not flagged -- it is SKIPPED. That has already happened twice
# (context_editor_shell_uimirror_smoke, context_editor_shell_iframe_smoke). Check 5 ties the literal
# roster to the literal stage-consumer edges; its graph-tier companion in
# src/editor/shell/CMakeLists.txt derives the same roster from target properties under a CEF-ON
# configure. Cases marked `# FOUND BY PLANTING` came from mutating the live build files.

_ROSTER_SHELL = """
context_acquire_cef(context_editor editor-cef-smoke)
context_cef_stage_payload(context_editor_cef_stage "${CEF_TARGET_OUT_DIR}")
add_executable(context_editor app/editor_main.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor)
add_dependencies(context_editor context_editor_cef_stage)
add_executable(context_editor_shell_cef_smoke src/cef_shell_smoke.cpp)
SET_EXECUTABLE_TARGET_PROPERTIES(context_editor_shell_cef_smoke)
add_dependencies(context_editor_shell_cef_smoke context_editor_cef_stage)
set(_ctx_cef_shell_executables
    context_editor
    context_editor_shell_cef_smoke)
"""


def _roster_tree(tmp_path: Path, body: str) -> Path:
    return _repo(tmp_path, {"src/editor/shell/CMakeLists.txt": body})


def test_roster_matching_the_stage_consumers_is_clean(tmp_path: Path) -> None:
    findings, _ = check.scan(_roster_tree(tmp_path, _ROSTER_SHELL))
    assert findings == []


def test_a_stage_consumer_missing_from_the_roster_is_reported(tmp_path: Path) -> None:
    """THE HISTORICAL DEFECT, reconstructed: uimirror and iframe both took the stage edge and were
    both absent from the roster, so both configure-time audits skipped them for two tasks."""
    body = _ROSTER_SHELL.replace(
        "set(_ctx_cef_shell_executables\n    context_editor\n    context_editor_shell_cef_smoke)",
        "set(_ctx_cef_shell_executables\n    context_editor)")
    findings, _ = check.scan(_roster_tree(tmp_path, body))
    assert any("context_editor_shell_cef_smoke takes the CEF stage dependency but is MISSING"
               in f for f in findings)


def test_a_roster_entry_with_no_stage_edge_is_reported(tmp_path: Path) -> None:
    """The other direction -- a renamed or removed target left rotting in the list."""
    body = _ROSTER_SHELL.replace(
        "    context_editor_shell_cef_smoke)",
        "    context_editor_shell_cef_smoke\n    context_editor_shell_ghost_smoke)")
    findings, _ = check.scan(_roster_tree(tmp_path, body))
    assert any("names context_editor_shell_ghost_smoke, but no" in f for f in findings)


def test_an_empty_roster_is_reported(tmp_path: Path) -> None:
    """The VACUOUS direction: an empty list makes 'every listed target is a consumer' trivially true
    while auditing nothing."""
    body = _ROSTER_SHELL.replace(
        "set(_ctx_cef_shell_executables\n    context_editor\n    context_editor_shell_cef_smoke)",
        "set(_ctx_cef_shell_executables)")
    findings, _ = check.scan(_roster_tree(tmp_path, body))
    assert any("is EMPTY" in f for f in findings)


def test_two_roster_declarations_are_reported(tmp_path: Path) -> None:
    """Two lists is how the pre-e12c-2 duplicates drifted apart; there is THE ONE roster."""
    findings, _ = check.scan(_roster_tree(
        tmp_path, _ROSTER_SHELL + "\nset(_ctx_cef_shell_executables context_editor)\n"))
    assert any("declarations of _ctx_cef_shell_executables" in f for f in findings)


def test_an_unresolvable_roster_entry_is_reported(tmp_path: Path) -> None:
    """FOUND BY PLANTING: a `${var}`-named roster member would be compared under its raw spelling and
    silently never match a consumer, so it is named as unresolvable instead."""
    body = _ROSTER_SHELL.replace(
        "    context_editor_shell_cef_smoke)", "    ${_some_loop_var}_smoke)")
    findings, _ = check.scan(_roster_tree(tmp_path, body))
    assert any("cannot resolve" in f for f in findings)


def test_a_commented_out_roster_declaration_does_not_count(tmp_path: Path) -> None:
    """Comment stripping applies here as everywhere else in this lint: prose about the roster is not
    a second declaration."""
    body = _ROSTER_SHELL + "\n# set(_ctx_cef_shell_executables context_editor)\n"
    findings, _ = check.scan(_roster_tree(tmp_path, body))
    assert findings == []


def test_a_tree_with_no_roster_is_not_penalised(tmp_path: Path) -> None:
    """Check 5 is generic over trees: a repo with no Shell has no roster to check, and the live-tree
    assertion below is what stops that from being how a RENAMED roster passes."""
    root = _repo(tmp_path, {
        "src/editor/cef/CMakeLists.txt": (
            "context_acquire_cef(context_cef cef-substrate)\n"
            "add_executable(context_cef_boot_smoke src/boot.cpp)\n"
            "SET_EXECUTABLE_TARGET_PROPERTIES(context_cef_boot_smoke)\n"
            'COPY_FILES(context_cef_boot_smoke "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}"'
            ' "${CEF_TARGET_OUT_DIR}")\n'
        )
    })
    findings, _ = check.scan(root)
    assert findings == []


def _live_roster() -> list[str]:
    """The live roster, or a clean ASSERTION failure. FOUND BY PLANTING: indexing the match list
    directly made the rename plant red through an `IndexError` instead — a crash reads as a red but
    names nothing, which is the weak half of clause (2b)'s three outcomes."""
    text = check.strip_comments(
        (_REPO / "src/editor/shell/CMakeLists.txt").read_text(encoding="utf-8"))
    matches = check._ROSTER_SET.findall(text)
    assert len(matches) == 1, (
        f"expected exactly one {check._ROSTER_VAR} declaration in src/editor/shell/CMakeLists.txt, "
        f"got {len(matches)} — a renamed or deleted roster would leave check 5 nothing to compare")
    return matches[0].split()


def test_live_repository_declares_exactly_one_non_empty_roster() -> None:
    """THE ANTI-VACUITY GUARD for check 5, held here rather than in the lint (which must stay generic
    over trees): if the roster is renamed or deleted, check 5 would find nothing to compare and pass.
    PLANT: rename `_ctx_cef_shell_executables` in src/editor/shell/CMakeLists.txt and this must RED."""
    roster = _live_roster()
    assert len(roster) >= 11, f"the live roster shrank to {len(roster)}: {roster}"
    assert all("${" not in name for name in roster)


def test_live_repository_still_carries_the_graph_tier_derivation() -> None:
    """THE STANDING EXISTENCE GUARD FOR THE GRAPH TIER — the direct mirror of
    `test_live_repository_declares_exactly_one_non_empty_roster` above, which exists so a RENAMED
    roster cannot become how check 5 passes.

    Without this, the derivation block is asserted by NOTHING: it lives only under
    CONTEXT_BUILD_GUI_CEF, no ctest or lint names any of its symbols, so changing its guard to
    `if(FALSE)` or deleting it outright reds nothing at all — the CEF-ON legs merely stop printing a
    STATUS line. The 17-plant round proved it worked at one moment; that is not a standing invariant.
    PLANT: delete any one of the anchors below (or flip the guard to `if(FALSE)`) and this must RED."""
    text = check.strip_comments(
        (_REPO / "src/editor/shell/CMakeLists.txt").read_text(encoding="utf-8"))
    for anchor, why in (
        ("function(context_shell_target_hosts_cef", "the link-closure predicate"),
        ("function(context_shell_collect_subtree_targets", "the subtree target enumeration"),
        ("if(CONTEXT_BUILD_GUI_CEF)", "the guard the derivation runs under"),
        ("if(NOT _ctx_cef_derived_executables)", "the empty-derived-set anti-vacuity error"),
        ("IN_LIST _ctx_cef_shell_executables", "the derived-but-UNLISTED direction"),
        ("IN_LIST _ctx_cef_derived_executables", "the listed-but-NOT-derived FLOOR direction"),
    ):
        assert anchor in text, (
            f"src/editor/shell/CMakeLists.txt no longer contains {anchor!r} ({why}). The graph tier "
            f"of the CEF roster audit is gone or disabled, and NOTHING else would have reported it.")


def test_live_repository_roster_equals_its_stage_consumers() -> None:
    """The live tie check 5 enforces, asserted directly so a regression names the roster rather than
    arriving as a generic lint finding."""
    findings, _ = check.scan(_REPO)
    assert findings == []
    roster = set(_live_roster())
    assert "context_editor" in roster
    assert "context_editor_shell_uimirror_smoke" in roster, "the e10d-drill2 omission must stay fixed"
    assert "context_editor_shell_iframe_smoke" in roster, "the e13a-2 omission must stay fixed"

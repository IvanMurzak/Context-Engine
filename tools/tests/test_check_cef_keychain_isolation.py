"""Tests for tools/check_cef_keychain_isolation.py — the #437 OSCrypt-keychain gate (R-QA-013).

MUTATION COVERAGE, not happy-path coverage: a gate whose only test is "the live tree passes" is
indistinguishable from a gate that always passes. Every rule is exercised by PLANTING the violation it
exists to catch, and the live repository is then checked once at the end.

The cases marked `# FOUND BY PLANTING` are the two that a planting round over a COPY of the real
`src/editor/` tree found the first revision MISSING (both reported GREEN, i.e. the dangerous
direction). Both were shapes the next task in this area could plausibly write, which is the whole
argument for the round:

  * a NEW CEF source in the Shell's smoke directory whose name does not end in `_smoke.cpp` — the
    first revision keyed rule 1 on the FILENAME, so `cef_shell_scenarios.cpp` was silently exempt;
  * a NEW standalone CEF app anywhere else under `src/editor/` — the first revision keyed rule 2 on a
    hardcoded two-entry list.

Both rules are now PREDICATES (constructs a `CefShellOptions` / defines
`OnBeforeCommandLineProcessing`). The round's other 10 plants were caught by the first revision and
are kept below as ordinary regression cases.
"""

from __future__ import annotations

from pathlib import Path

import pytest
from conftest import load_tool

gate = load_tool("check_cef_keychain_isolation")

REPO_ROOT = Path(__file__).resolve().parents[2]

# A Shell smoke that DOES isolate, in the tree's dominant spelling.
GOOD_SHELL_SMOKE = """\
int main()
{
    shell::cef::CefShellOptions cef_options;
    cef_options.windowless_frame_rate = 10;
    cef_options.use_mock_keychain = true;
    return 0;
}
"""

GOOD_STANDALONE = """\
class HostApp : public CefApp
{
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override
    {
        command_line->AppendSwitch("no-sandbox");
        command_line->AppendSwitch("use-mock-keychain");
    }
};
"""

GOOD_HEADER = """\
struct CefShellOptions
{
    bool verbose_logging = false;
    bool use_mock_keychain = false;
};
"""

# The Shell impl: it RECEIVES options by reference (so it is not a rule-1 subject), latches the
# option, and appends the switch behind it.
GOOD_IMPL = """\
bool g_use_mock_keychain = false;
void hook(CefRefPtr<CefCommandLine> command_line)
{
    if (g_use_mock_keychain)
    {
        command_line->AppendSwitch("use-mock-keychain");
    }
}
bool initialize(const CefShellOptions& options)
{
    g_use_mock_keychain = options.use_mock_keychain;
    return true;
}
"""


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_tree(tmp_path: Path, smokes: int = 2) -> Path:
    """A minimal tree carrying every anchor the gate requires, and nothing else."""
    for i in range(smokes):
        write(tmp_path, f"{gate.SHELL_SMOKE_DIR}/cef_shell_{i}_smoke.cpp", GOOD_SHELL_SMOKE)
    for rel in gate.STANDALONE_APPS:
        write(tmp_path, rel, GOOD_STANDALONE)
    write(tmp_path, gate.OPTIONS_HEADER, GOOD_HEADER)
    write(tmp_path, gate.SHELL_IMPL, GOOD_IMPL)
    return tmp_path


# ---------------------------------------------------------------------------
# baseline + the "cannot run" contract
# ---------------------------------------------------------------------------


def test_clean_tree_passes(tmp_path):
    assert gate.check(make_tree(tmp_path)) == []


def test_missing_tree_is_a_config_error_not_a_pass(tmp_path):
    """A moved anchor must FAIL LOUDLY (exit 2), never report OK."""
    (tmp_path / "src").mkdir()
    with pytest.raises(FileNotFoundError):
        gate.check(tmp_path)
    assert gate.main(["--repo-root", str(tmp_path)]) == 2


def test_no_options_constructor_anywhere_is_a_config_error(tmp_path):
    """A rename/move of the smoke sources must be exit 2, not a vacuous pass: with zero subjects the
    rule-1 loop would otherwise iterate zero times and report OK."""
    root = make_tree(tmp_path, smokes=0)
    with pytest.raises(FileNotFoundError):
        gate.check(root)


def test_no_command_line_hook_anywhere_is_a_config_error(tmp_path):
    """The rule-2 twin of the case above."""
    root = make_tree(tmp_path)
    for rel in gate.STANDALONE_APPS:
        write(root, rel, "class HostApp : public CefApp {};\n")
    write(root, gate.SHELL_IMPL, GOOD_IMPL.replace("void hook(", "void not_the_hook("))
    # The hook name is gone tree-wide, so the scan has no subject at all.
    with pytest.raises(FileNotFoundError):
        gate.check(root)


# ---------------------------------------------------------------------------
# rule 1 — every source that CONSTRUCTS CefShellOptions isolates the keychain
# ---------------------------------------------------------------------------


def test_shell_smoke_without_the_opt_in_is_caught(tmp_path):
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_forgot_smoke.cpp",
          GOOD_SHELL_SMOKE.replace("    cef_options.use_mock_keychain = true;\n", ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "cef_shell_forgot_smoke.cpp" in findings[0]


def test_a_new_cef_source_not_named_smoke_is_caught(tmp_path):
    """FOUND BY PLANTING — the first revision reported this GREEN.

    Rule 1 keyed on a `*_smoke.cpp` filename, so a new CEF source in the very same directory named
    anything else was exempt. This directory gains a source whenever a new live scenario lands (e12c-2
    fanned the EXISTING nine out to macOS without adding one, but e12c-3 and every later T2 scenario
    will), so the shape is not hypothetical. The predicate is now "constructs a CefShellOptions"."""
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_scenarios.cpp",
          GOOD_SHELL_SMOKE.replace("    cef_options.use_mock_keychain = true;\n", ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "cef_shell_scenarios.cpp" in findings[0]


def test_the_helper_shape_options_variable_is_accepted(tmp_path):
    """The tree writes BOTH `cef_options.` (inline) and `options.` (the make_cef_options helper);
    a pattern anchored on one spelling would report four real smokes as violations."""
    root = make_tree(tmp_path, smokes=0)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_helper_smoke.cpp",
          GOOD_SHELL_SMOKE.replace("cef_options", "options"))
    write(root, gate.SHELL_IMPL, GOOD_IMPL)
    assert gate.check(root) == []


def test_setting_the_option_to_false_is_caught(tmp_path):
    """`= false` reads as "the line is there" to a careless eye and isolates nothing."""
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_false_smoke.cpp",
          GOOD_SHELL_SMOKE.replace("use_mock_keychain = true", "use_mock_keychain = false"))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "cef_shell_false_smoke.cpp" in findings[0]


def test_a_wrapped_assignment_is_accepted(tmp_path):
    """clang-format wraps a long assignment; a line-anchored pattern would red a compliant smoke."""
    root = make_tree(tmp_path, smokes=0)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_wrapped_smoke.cpp",
          "int main() {\n    shell::cef::CefShellOptions opts_with_a_very_long_name;\n"
          "    opts_with_a_very_long_name\n        .use_mock_keychain = true;\n}\n")
    assert gate.check(root) == []


def test_a_brace_initialized_options_is_a_subject(tmp_path):
    """`CefShellOptions opts{};` CONSTRUCTS one exactly as `CefShellOptions opts;` does, and the brace
    form is idiomatic C++. A predicate keyed on the `;` alone exempts it ENTIRELY — not a false pass on
    a subject, but no subject at all — so a new scenario written that way would be reported
    GREEN while isolating nothing. Same silent-exemption class as the two plants above."""
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_braced_smoke.cpp",
          "int main()\n{\n    shell::cef::CefShellOptions opts{};\n    return 0;\n}\n")
    findings = gate.check(root)
    assert len(findings) == 1
    assert "cef_shell_braced_smoke.cpp" in findings[0]


def test_a_compliant_brace_initialized_smoke_passes(tmp_path):
    """The other half of the case above: widening the predicate must not red a compliant brace-init
    smoke, or the fix would trade a false GREEN for a false RED."""
    root = make_tree(tmp_path, smokes=0)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_braced_ok_smoke.cpp",
          "int main()\n{\n    shell::cef::CefShellOptions opts{};\n"
          "    opts.use_mock_keychain = true;\n    return 0;\n}\n")
    assert gate.check(root) == []


def test_a_by_value_return_type_is_not_a_subject(tmp_path):
    """`CefShellOptions make_cef_options(...)` is a RETURN type, not a construction — six real smokes
    declare exactly that helper, and treating the signature line as a subject would be harmless only
    by accident (the helper's own body constructs one anyway). Pinned so the `[;{]` widening is not
    mistaken for "anything after the type name"."""
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_factory_decl.cpp",
          "shell::cef::CefShellOptions make_cef_options(int rate);\n")
    assert gate.check(root) == []


def test_a_by_reference_parameter_is_not_a_subject(tmp_path):
    """`initialize(const CefShellOptions&)` receives options, it does not construct any — treating it
    as a subject would make `cef_shell.cpp` permanently unsatisfiable."""
    root = make_tree(tmp_path)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_consumer.cpp",
          "bool go(const shell::cef::CefShellOptions& options) { return options.verbose_logging; }\n")
    assert gate.check(root) == []


def test_a_commented_out_opt_in_still_passes_and_that_is_documented(tmp_path):
    """A deliberately RECORDED limitation, not an oversight. The gate is a syntactic registration
    anchor: it cannot see comments, so `// options.use_mock_keychain = true;` satisfies it. Acceptable
    because the smoke would then HANG on macOS and its own ctest would red — the gate exists to catch
    a FORGOTTEN line, not to defeat a deliberate bypass. Pinned so the limit is a decision rather than
    a surprise."""
    root = make_tree(tmp_path, smokes=0)
    write(root, f"{gate.SHELL_SMOKE_DIR}/cef_shell_commented_smoke.cpp",
          "int main() { shell::cef::CefShellOptions opts;\n"
          "    /* opts.use_mock_keychain = true; */ }\n")
    assert gate.check(root) == []


# ---------------------------------------------------------------------------
# rule 2 — every CEF app that owns the command-line hook appends the switch
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("rel", gate.STANDALONE_APPS)
def test_standalone_app_without_the_switch_is_caught(tmp_path, rel):
    root = make_tree(tmp_path)
    write(root, rel, GOOD_STANDALONE.replace(
        '        command_line->AppendSwitch("use-mock-keychain");\n', ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert rel in findings[0]


def test_a_new_standalone_cef_app_elsewhere_is_caught(tmp_path):
    """FOUND BY PLANTING — the first revision reported this GREEN.

    Rule 2 checked a hardcoded two-entry list, so a THIRD CEF app anywhere under src/editor/ was
    exempt. The predicate is now "defines OnBeforeCommandLineProcessing"."""
    root = make_tree(tmp_path)
    write(root, "src/editor/cef/src/cef_second_smoke.cpp", GOOD_STANDALONE.replace(
        '        command_line->AppendSwitch("use-mock-keychain");\n', ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "cef_second_smoke.cpp" in findings[0]


def test_the_hook_in_a_header_is_also_a_subject(tmp_path):
    """FOUND BY PLANTING (same round, same rule): a CefApp defined inline in a header would have been
    exempt from a `*.cpp`-only scan."""
    root = make_tree(tmp_path)
    write(root, "src/editor/gui/host/src/other_app.h", GOOD_STANDALONE.replace(
        '        command_line->AppendSwitch("use-mock-keychain");\n', ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "other_app.h" in findings[0]


# ---------------------------------------------------------------------------
# the ANTI-VACUITY half — deleting the option must not make the gate pass
# ---------------------------------------------------------------------------


def test_deleting_the_option_declaration_is_caught(tmp_path):
    root = make_tree(tmp_path)
    write(root, gate.OPTIONS_HEADER, GOOD_HEADER.replace(
        "    bool use_mock_keychain = false;\n", ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "is GONE" in findings[0]


def test_dropping_the_latch_is_caught(tmp_path):
    """The sharpest vacuity shape: every smoke still sets the field, and the field reaches nothing."""
    root = make_tree(tmp_path)
    write(root, gate.SHELL_IMPL, GOOD_IMPL.replace(
        "    g_use_mock_keychain = options.use_mock_keychain;\n", ""))
    findings = gate.check(root)
    assert len(findings) == 1
    assert "latches" in findings[0]


def test_dropping_the_switch_append_is_caught(tmp_path):
    """The latch alone is not enough: with the AppendSwitch gone the option is latched into a global
    nothing reads, and every per-smoke check still passes."""
    root = make_tree(tmp_path)
    write(root, gate.SHELL_IMPL, GOOD_IMPL.replace(
        '        command_line->AppendSwitch("use-mock-keychain");\n', ""))
    findings = gate.check(root)
    # Two findings: the impl is BOTH the latch/append anchor and a rule-2 hook subject.
    assert any("inert" in f for f in findings)


# ---------------------------------------------------------------------------
# the live repository
# ---------------------------------------------------------------------------


def test_live_repository_passes():
    assert gate.check(REPO_ROOT) == []


def test_live_repository_has_every_anchor_where_the_tool_believes():
    assert (REPO_ROOT / gate.OPTIONS_HEADER).is_file()
    assert (REPO_ROOT / gate.SHELL_IMPL).is_file()
    for rel in gate.STANDALONE_APPS:
        assert (REPO_ROOT / rel).is_file()
    subjects = [rel for rel, text in
                gate.iter_sources(REPO_ROOT, gate.SHELL_SMOKE_DIR, gate.SHELL_SOURCE_SUFFIXES)
                if gate.SHELL_OPTIONS_CONSTRUCTED.search(text)]
    # Nine as of M9 e12c-2, which fanned those same nine out to macOS rather than adding sources; a
    # later scenario adds more. A FLOOR keeps the check meaningful (the predicate found real subjects)
    # without making every new smoke edit this test.
    assert len(subjects) >= 9
    hooks = [rel for rel, text in gate.iter_sources(REPO_ROOT, gate.EDITOR_ROOT, gate.HOOK_SUFFIXES)
             if gate.COMMANDLINE_HOOK.search(text)]
    assert len(hooks) >= 3

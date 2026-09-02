"""Tests for tools/build_editor.py — the one-command editor build (R-QA-013).

The impure halves (vswhere discovery, the VsDevCmd import, the cmake subprocesses) are exercised by
actually building the editor; what pytest pins is the PURE decision layer, because each piece here
is one the field failure travelled through: the env-block parser (importing a `set` dump with
banner noise and multi-line values in it), the stale-cache compiler guard (the GCC-configured tree
that poisoned every later build), and the exact command lines the script drives cmake with.
"""

from __future__ import annotations

from pathlib import Path

from conftest import load_tool

be = load_tool("build_editor")


# ------------------------------------------------------------------------------- parse_env_block

def test_env_block_imports_assignments_and_skips_noise() -> None:
    dump = (
        "** some VsDevCmd banner line **\n"
        "INCLUDE=C:\\VS\\include;C:\\SDK\\include\n"
        "Path=C:\\VS\\bin;C:\\Windows\n"
        "PROMPT=$P$G\n"
        "a line that is not an assignment\n"
        "=C:=C:\\  (cmd's drive-letter pseudo-variable: no name, must be skipped)\n"
        "LIB=C:\\VS\\lib\n"
    )
    env = be.parse_env_block(dump)
    assert env["INCLUDE"] == "C:\\VS\\include;C:\\SDK\\include"
    assert env["LIB"] == "C:\\VS\\lib"
    assert env["Path"] == "C:\\VS\\bin;C:\\Windows"
    assert "a line that is not an assignment" not in env
    assert all(name for name in env)  # the nameless pseudo-variable was not imported


def test_env_block_keeps_equals_signs_inside_values() -> None:
    env = be.parse_env_block("FLAGS=a=b=c\n")
    assert env["FLAGS"] == "a=b=c"


def test_env_path_finds_windows_capitalization() -> None:
    # Windows spells the live variable `Path`; os.environ upper-cases it. Both must resolve —
    # looking up one fixed spelling is how the script's first version reported a healthy VS
    # install as broken (cl.exe searched on the PRE-VsDevCmd path).
    assert be.env_path({"Path": "C:\\vs\\bin"}) == "C:\\vs\\bin"
    assert be.env_path({"PATH": "/usr/bin"}) == "/usr/bin"
    assert be.env_path({"HOME": "/root"}) == ""


# ------------------------------------------------------------------------------- cache_compiler

def test_cache_compiler_reads_the_recorded_compiler() -> None:
    cache = (
        "# This is the CMakeCache file.\n"
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "CMAKE_CXX_COMPILER:FILEPATH=C:/PROGRA~1/MICROS~1/18/VC/bin/cl.exe\n"
    )
    assert be.cache_compiler(cache) == "C:/PROGRA~1/MICROS~1/18/VC/bin/cl.exe"


def test_cache_compiler_absent_is_none() -> None:
    assert be.cache_compiler("CMAKE_BUILD_TYPE:STRING=Release\n") is None
    assert be.cache_compiler("") is None


# ------------------------------------------------------------------------------- command shapes

def test_configure_command_carries_the_cef_toggle_and_the_gpu_toggle() -> None:
    cmd = be.configure_command("cmake", Path("bd"), "Ninja", gpu=False)
    assert cmd[0] == "cmake"
    assert "-G" in cmd and cmd[cmd.index("-G") + 1] == "Ninja"
    assert "-DCONTEXT_BUILD_GUI_CEF=ON" in cmd
    assert "-DCMAKE_BUILD_TYPE=Release" in cmd
    assert "-DCONTEXT_BUILD_RENDER_WGPU=ON" not in cmd

    gpu = be.configure_command("cmake", Path("bd"), "Ninja", gpu=True)
    assert "-DCONTEXT_BUILD_RENDER_WGPU=ON" in gpu


def test_the_gpu_toggle_is_stated_in_both_directions() -> None:
    # CMake CACHES this variable, so an omitted -D leaves whatever the previous configure wrote.
    # Emitting the OFF explicitly is the whole reason --no-gpu can undo an earlier GPU build; a
    # version that only ever appends the ON passes the assertions above and still cannot opt out.
    assert "-DCONTEXT_BUILD_RENDER_WGPU=OFF" in be.configure_command("cmake", Path("bd"), None,
                                                                     gpu=False)
    assert "-DCONTEXT_BUILD_RENDER_WGPU=OFF" not in be.configure_command("cmake", Path("bd"), None,
                                                                         gpu=True)


def test_gpu_defaults_on_and_no_gpu_opts_out() -> None:
    # The default is the thing a bare `python tools/build_editor.py` gets. It is ON because this
    # script already forces the CEF prebuilt unconditionally, so it is not a build that can run on
    # a toolchain too plain for wgpu anyway -- and a viewport with no adapter renders no scene.
    parser = be.build_parser()
    assert parser.parse_args([]).gpu is True
    assert parser.parse_args(["--gpu"]).gpu is True       # still accepted, so old commands survive
    assert parser.parse_args(["--no-gpu"]).gpu is False


def test_configure_command_without_a_generator_omits_the_flag() -> None:
    assert "-G" not in be.configure_command("cmake", Path("bd"), None, gpu=False)


def test_build_command_builds_all_three_editor_targets() -> None:
    cmd = be.build_command("cmake", Path("bd"))
    for target in ("context", "context_editor", "context_editor_webui"):
        assert target in cmd


def test_editor_binary_is_in_the_per_config_cef_layout() -> None:
    path = be.editor_binary(Path("bd"))
    # The CEF staging macro's per-config subdirectory — the same layout PR #479's locate fix and
    # the README's run command name; the three must not drift apart.
    assert path.parent == Path("bd") / "editor" / "shell" / "Release"
    assert path.name.startswith("context_editor")

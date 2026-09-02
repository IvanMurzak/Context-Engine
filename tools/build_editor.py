#!/usr/bin/env python3
"""Build the Context editor: the `context` CLI, `context_editor`, and the editor-core web bundle.

One command on every platform, from ANY shell:

    python tools/build_editor.py            # configure + build into src/build/editor, GPU present
    python tools/build_editor.py --no-gpu   # fall back to the CPU present path (no wgpu prebuilt)

WHY THIS SCRIPT EXISTS. The editor's CEF prebuilt is MSVC-ABI on Windows and cannot link under
GCC/MinGW — and MSVC's cl.exe itself works only inside a "Developer" environment (INCLUDE / LIB /
PATH). A README that says "run these two cmake lines from a Developer PowerShell" fails the moment
someone runs them from an ordinary shell, and it fails FAR from the cause: LNK1181 on kernel32.lib
at configure time, C1083 on <cstddef> at build time — both measured in the field, twice. So on
Windows this script locates Visual Studio via vswhere and imports the VsDevCmd environment ITSELF;
on Linux/macOS it selects clang (the CEF prebuilt is clang-ABI there) unless CC/CXX already say
otherwise. Nothing here is required for the headless engine — `cmake -S src --preset dev` stays
the plain path for that.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = REPO_ROOT / "src" / "build" / "editor"
EDITOR_TARGETS = ["context", "context_editor", "context_editor_webui"]

# One line of `cmd /c set` output: NAME=VALUE with a sane variable name. Anything else (a banner
# line VsDevCmd printed anyway, the continuation of a multi-line value) is skipped rather than
# imported as garbage.
_ENV_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_()#. -]*)=(.*)$")


def parse_env_block(text: str) -> dict[str, str]:
    """Parse `set`-style NAME=VALUE output into a dict, skipping non-assignment lines."""
    env: dict[str, str] = {}
    for line in text.splitlines():
        match = _ENV_LINE.match(line)
        if match is not None:
            env[match.group(1)] = match.group(2)
    return env


def env_path(env: dict[str, str]) -> str:
    """The PATH value under whichever capitalization the environment uses.

    Windows spells the live variable `Path` while `os.environ` upper-cases it to `PATH`; reading
    one fixed spelling out of a dict that mixes the two is how the first version of this script
    looked cl.exe up on the PRE-VsDevCmd path and reported a healthy VS install as broken.
    """
    for name, value in env.items():
        if name.upper() == "PATH":
            return value
    return ""


def cache_compiler(cache_text: str) -> str | None:
    """The CMAKE_CXX_COMPILER a CMakeCache.txt records, or None when it records none."""
    for line in cache_text.splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:"):
            _, _, value = line.partition("=")
            return value.strip() or None
    return None


def configure_command(cmake: str, build_dir: Path, generator: str | None, gpu: bool) -> list[str]:
    cmd = [cmake, "-S", str(REPO_ROOT / "src"), "-B", str(build_dir)]
    if generator is not None:
        cmd += ["-G", generator]
    cmd += ["-DCMAKE_BUILD_TYPE=Release", "-DCONTEXT_BUILD_GUI_CEF=ON"]
    # Stated in BOTH directions, deliberately. CMake caches this variable, so merely OMITTING the
    # -D on a --no-gpu run would leave a previously-configured ON in place and the flag would do
    # nothing on the second build. An explicit OFF is what makes the opt-out actually opt out.
    cmd.append(f"-DCONTEXT_BUILD_RENDER_WGPU={'ON' if gpu else 'OFF'}")
    return cmd


def build_command(cmake: str, build_dir: Path) -> list[str]:
    return [cmake, "--build", str(build_dir), "--target", *EDITOR_TARGETS]


def editor_binary(build_dir: Path) -> Path:
    exe = "context_editor.exe" if platform.system() == "Windows" else "context_editor"
    # The CEF staging macro parks the executable in a per-config subdirectory (issue #479's layout).
    return build_dir / "editor" / "shell" / "Release" / exe


def _fail(message: str) -> int:
    print(f"[build-editor] ERROR: {message}", file=sys.stderr)
    return 2


def _windows_dev_env() -> dict[str, str] | int:
    """os.environ + the VsDevCmd x64 environment, or an exit code with the reason printed."""
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        return _fail(
            "vswhere.exe not found — install Visual Studio (any edition) with the "
            "'Desktop development with C++' workload; the editor's CEF prebuilt needs MSVC."
        )
    located = subprocess.run(
        [
            str(vswhere), "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath",
        ],
        capture_output=True, text=True, check=False,
    )
    vs_root = located.stdout.strip().splitlines()[0] if located.stdout.strip() else ""
    if located.returncode != 0 or not vs_root:
        return _fail(
            "no Visual Studio with the C++ toolset (VC.Tools.x86.x64) was found — install the "
            "'Desktop development with C++' workload."
        )
    vsdevcmd = Path(vs_root) / "Common7" / "Tools" / "VsDevCmd.bat"
    if not vsdevcmd.is_file():
        return _fail(f"VsDevCmd.bat is missing from the located Visual Studio: {vsdevcmd}")
    print(f"[build-editor] importing the MSVC environment from {vs_root}")
    # `cmd /s /c` + the outer quote pair is the documented way to run a quoted .bat with arguments;
    # `&& set` dumps the resulting environment for us to adopt.
    dump = subprocess.run(
        f'cmd.exe /s /c ""{vsdevcmd}" -arch=x64 -no_logo && set"',
        capture_output=True, text=True, check=False,
    )
    if dump.returncode != 0:
        return _fail(f"VsDevCmd failed:\n{dump.stdout}\n{dump.stderr}")
    # The `set` dump IS the dev shell's complete environment (inherited + VS additions), so it is
    # adopted WHOLE rather than merged over os.environ: a merge would carry both `PATH` (Python's
    # upper-cased spelling) and `Path` (the dump's), and which of the two duplicate case-insensitive
    # names a child process then sees is anyone's guess.
    env = parse_env_block(dump.stdout)
    if len(env) < 10 or not env_path(env):
        return _fail(f"VsDevCmd produced no usable environment dump:\n{dump.stdout[:1000]}")
    if shutil.which("cl", path=env_path(env)) is None:
        return _fail("VsDevCmd ran but cl.exe is still not on PATH — the VS install looks broken.")
    return env


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Build the Context editor (CLI + shell + web UI).")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR,
                        help=f"build tree (default: {DEFAULT_BUILD_DIR})")
    parser.add_argument("--gpu", action=argparse.BooleanOptionalAction, default=True,
                        help="build the wgpu GPU present path (default: on). --no-gpu falls back to "
                             "the CPU present path, which needs no wgpu prebuilt but renders no "
                             "viewport scene (the Scene panel then reports viewport.adapter_absent)")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    build_dir: Path = args.build_dir

    windows = platform.system() == "Windows"
    if windows:
        env = _windows_dev_env()
        if isinstance(env, int):
            return env
    else:
        env = dict(os.environ)
        # The CEF prebuilt is clang-ABI on Linux/macOS. Respect an explicit CC/CXX; otherwise pick
        # clang, and refuse early if there is none — a GCC configure fails later and less legibly.
        if "CXX" not in env:
            if shutil.which("clang++", path=env_path(env)) is None:
                return _fail("clang++ not found — install clang; the CEF prebuilt is clang-ABI.")
            env["CC"] = env.get("CC", "clang")
            env["CXX"] = "clang++"

    # A tree configured with the WRONG compiler poisons every later build in it (the field failure:
    # a GCC-configured tree refused the CEF prebuilt forever after). Refuse loudly instead of
    # letting cmake mix caches.
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        recorded = cache_compiler(cache.read_text(encoding="utf-8", errors="replace")) or ""
        wrong = ("cl.exe" not in recorded.lower()) if windows else ("cl.exe" in recorded.lower())
        if recorded and wrong:
            return _fail(
                f"{build_dir} was configured with a different compiler ({recorded}); delete that "
                "directory and rerun this script."
            )

    cmake = shutil.which("cmake", path=env_path(env))
    if cmake is None:
        return _fail("cmake not found on PATH (CMake >= 3.25 is required).")
    generator = "Ninja" if shutil.which("ninja", path=env_path(env)) is not None else None
    if windows and generator is None:
        return _fail(
            "ninja not found on PATH — install Ninja (or the Visual Studio 'C++ CMake tools' "
            "component, which ships it)."
        )

    for cmd in (configure_command(cmake, build_dir, generator, args.gpu),
                build_command(cmake, build_dir)):
        print(f"[build-editor] {' '.join(cmd)}")
        result = subprocess.run(cmd, env=env, check=False)
        if result.returncode != 0:
            return result.returncode

    binary = editor_binary(build_dir)
    if not binary.is_file():
        return _fail(f"the build reported success but {binary} does not exist — please report this.")
    print(f"[build-editor] editor: {binary}")
    print(f"[build-editor] run:    {binary} --project samples/platformer-2d")
    return 0


if __name__ == "__main__":
    sys.exit(main())

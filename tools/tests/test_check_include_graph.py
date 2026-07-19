"""Tests for tools/check_include_graph.py — the D10 client-boundary gate (R-QA-013 coverage).

Covers the happy path plus every violation class the gate exists to catch: a kernel-internal include
in a published header, an include that resolves to no installed header (a package a consumer could
not compile), an installed header outside the published modules, and a consumer reaching past the
surface. Also the layout/usage failures (no install tree, empty include tree) that must exit 2 rather
than silently reporting "ok" — a gate that passes vacuously is worse than no gate.

The final test runs the checker against the REAL committed consumer sources, so the shipped consumer
cannot drift out of the allowlist without failing here.
"""

from __future__ import annotations

from pathlib import Path

from conftest import load_tool

check_include_graph = load_tool("check_include_graph")

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_CONSUMER = REPO_ROOT / "src" / "editor" / "client" / "consumer"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def make_install(root: Path, headers: dict[str, str], *, versioned: bool = True) -> Path:
    """Build a fake install prefix; returns the prefix to pass as --install-prefix."""
    include = root / "versions" / "0.0.1" / "include" if versioned else root / "include"
    for rel, text in headers.items():
        write(include / rel, text)
    return root


GOOD_HEADERS = {
    "context/editor/client/client.h": '#include "context/editor/contract/json.h"\n#include <string>\n',
    "context/editor/client/subscription.h": '#include "context/editor/client/client.h"\n',
    "context/editor/contract/json.h": "#include <vector>\n",
    "context/editor/contract/handshake.h": "#include <cstdint>\n",
}


def run(prefix: Path, consumer: Path | None = None, *, json: bool = False) -> int:
    argv = ["--install-prefix", str(prefix)]
    if consumer is not None:
        argv += ["--consumer", str(consumer)]
    if json:
        argv.append("--json")
    return check_include_graph.main(argv)


# --- happy path -------------------------------------------------------------------------------


def test_clean_surface_passes(tmp_path: Path) -> None:
    prefix = make_install(tmp_path, GOOD_HEADERS)
    assert run(prefix) == 0


def test_finds_a_non_versioned_include_root(tmp_path: Path) -> None:
    prefix = make_install(tmp_path, GOOD_HEADERS, versioned=False)
    assert run(prefix) == 0


def test_clean_consumer_passes(tmp_path: Path) -> None:
    prefix = make_install(tmp_path / "install", GOOD_HEADERS)
    consumer = tmp_path / "consumer"
    write(consumer / "main.cpp", '#include "context/editor/client/client.h"\n#include <iostream>\n')
    assert run(prefix, consumer) == 0


# --- violation classes ------------------------------------------------------------------------


def test_kernel_internal_include_in_a_public_header_fails(tmp_path: Path) -> None:
    headers = dict(GOOD_HEADERS)
    headers["context/editor/client/client.h"] = (
        '#include "context/editor/editorkernel/editor_kernel.h"\n'
    )
    prefix = make_install(tmp_path, headers)
    assert run(prefix) == 1


def test_include_resolving_to_no_installed_header_fails(tmp_path: Path) -> None:
    """An allowed-module include we did not ship: the package would not compile for a consumer."""
    headers = dict(GOOD_HEADERS)
    headers["context/editor/client/client.h"] = '#include "context/editor/contract/not_shipped.h"\n'
    prefix = make_install(tmp_path, headers)
    assert run(prefix) == 1


def test_transitive_leak_is_caught(tmp_path: Path) -> None:
    """The gate is transitive: reaching an internal via an intermediate public header still fails."""
    headers = dict(GOOD_HEADERS)
    headers["context/editor/client/client.h"] = '#include "context/editor/client/subscription.h"\n'
    headers["context/editor/client/subscription.h"] = (
        '#include "context/editor/filesync/native_file_store.h"\n'
    )
    prefix = make_install(tmp_path, headers)
    assert run(prefix) == 1


def test_bridge_and_kernel_are_off_the_published_surface(tmp_path: Path) -> None:
    """context_client is BUILT over bridge, but bridge/kernel headers are link payload, not surface.

    Publishing them would let a consumer instantiate bridge::Daemon or drive the microkernel
    in-process — the inversion D10 exists to prevent. They are installed as archives only.
    """
    for internal in ("context/editor/bridge/daemon.h", "context/kernel/world.h"):
        headers = dict(GOOD_HEADERS)
        headers["context/editor/client/client.h"] = f'#include "{internal}"\n'
        assert run(make_install(tmp_path / internal.replace("/", "_"), headers)) == 1


def test_relative_include_in_an_installed_header_fails(tmp_path: Path) -> None:
    """The bypass the transitive rule depends on closing.

    A relative quoted include names no `context/` module, so before this rule it was filtered out
    ahead of every check — no forbidden-prefix test, no installed-resolution test. That is the one
    shape a leak could take while the gate reported OK.
    """
    headers = dict(GOOD_HEADERS)
    headers["context/editor/client/client.h"] = (
        '#include "../../editorkernel/editor_kernel.h"\n'
    )
    prefix = make_install(tmp_path, headers)
    assert run(prefix) == 1


def test_consumer_may_use_its_own_local_headers(tmp_path: Path) -> None:
    """The relative-include rule is scoped to the INSTALLED tree — a consumer's own headers are fine."""
    prefix = make_install(tmp_path / "install", GOOD_HEADERS)
    consumer = tmp_path / "consumer"
    write(consumer / "helper.h", "#include <string>\n")
    write(consumer / "main.cpp", '#include "helper.h"\n#include "context/editor/client/client.h"\n')
    assert run(prefix, consumer) == 0


def test_installed_header_outside_the_published_modules_fails(tmp_path: Path) -> None:
    headers = dict(GOOD_HEADERS)
    headers["context/runtime/session/session.h"] = "#include <cstdint>\n"
    prefix = make_install(tmp_path, headers)
    assert run(prefix) == 1


def test_consumer_reaching_past_the_surface_fails(tmp_path: Path) -> None:
    prefix = make_install(tmp_path / "install", GOOD_HEADERS)
    consumer = tmp_path / "consumer"
    write(consumer / "main.cpp", '#include "context/editor/editorkernel/editor_kernel.h"\n')
    assert run(prefix, consumer) == 1


# --- usage / config failures (must be 2, never a vacuous 0) -------------------------------------


def test_missing_install_tree_is_a_config_error(tmp_path: Path) -> None:
    assert run(tmp_path / "nonexistent") == 2


def test_empty_include_tree_is_a_config_error(tmp_path: Path) -> None:
    (tmp_path / "include").mkdir(parents=True)
    assert run(tmp_path) == 2


def test_missing_consumer_dir_is_a_config_error(tmp_path: Path) -> None:
    prefix = make_install(tmp_path / "install", GOOD_HEADERS)
    assert run(prefix, tmp_path / "no-such-consumer") == 2


# --- unit-level helpers -------------------------------------------------------------------------


def test_project_includes_ignores_stdlib() -> None:
    text = '#include <string>\n#include "context/editor/client/client.h"\n#include <vector>\n'
    assert check_include_graph.project_includes(text) == ["context/editor/client/client.h"]


def test_project_includes_handles_angle_bracket_project_headers() -> None:
    assert check_include_graph.project_includes("#include <context/editor/client/wire.h>\n") == [
        "context/editor/client/wire.h"
    ]


def test_relative_includes_finds_only_quoted_non_context_paths() -> None:
    text = (
        '#include <string>\n'                                # stdlib, angle: not ours
        '#include "context/editor/client/client.h"\n'        # qualified: covered by project_includes
        '#include "../../editorkernel/editor_kernel.h"\n'    # the bypass
        '#include "local_helper.h"\n'                        # also unresolvable from the install root
    )
    assert check_include_graph.relative_includes(text) == [
        "../../editorkernel/editor_kernel.h",
        "local_helper.h",
    ]


def test_forbidden_prefix_identifies_the_module() -> None:
    assert (
        check_include_graph.forbidden_prefix("context/editor/filesync/x.h")
        == "context/editor/filesync/"
    )
    assert check_include_graph.forbidden_prefix("context/editor/client/client.h") is None


def test_is_allowed_module() -> None:
    assert check_include_graph.is_allowed_module("context/editor/client/client.h")
    assert check_include_graph.is_allowed_module("context/editor/contract/json.h")
    assert not check_include_graph.is_allowed_module("context/editor/gui/panel.h")
    # Link-only closure members: installed as archives, never as headers.
    assert not check_include_graph.is_allowed_module("context/editor/bridge/transport.h")
    assert not check_include_graph.is_allowed_module("context/kernel/event_bus.h")
    # A bare module name with no trailing path is not a header in that module.
    assert not check_include_graph.is_allowed_module("context/editor/client")


def test_json_report_is_emitted_and_agrees_with_the_exit_code(tmp_path: Path, capsys) -> None:
    """--json is the machine-readable surface; a flag nothing exercises is a flag that silently rots."""
    import json as jsonlib

    prefix = make_install(tmp_path / "ok", GOOD_HEADERS)
    assert run(prefix, json=True) == 0
    report = jsonlib.loads(capsys.readouterr().out)
    assert report["ok"] is True
    assert report["violations"] == []
    assert report["installedHeaders"] == len(GOOD_HEADERS)
    assert report["allowedModules"] == list(check_include_graph.ALLOWED_MODULES)

    headers = dict(GOOD_HEADERS)
    headers["context/editor/client/client.h"] = (
        '#include "context/editor/editorkernel/editor_kernel.h"\n'
    )
    assert run(make_install(tmp_path / "bad", headers), json=True) == 1
    bad = jsonlib.loads(capsys.readouterr().out)
    assert bad["ok"] is False
    assert any("editorkernel" in v["include"] for v in bad["violations"])


# --- the live committed consumer ----------------------------------------------------------------


def test_live_consumer_sources_stay_within_the_surface(tmp_path: Path) -> None:
    """The shipped consumer must include nothing outside the published modules.

    Uses a synthetic install tree carrying exactly the headers the consumer names, so the assertion
    is about the CONSUMER's include discipline and does not require a built/installed engine (which
    the pytest job has no reason to produce).
    """
    assert LIVE_CONSUMER.is_dir(), f"missing committed consumer at {LIVE_CONSUMER}"
    sources = list(LIVE_CONSUMER.rglob("*.cpp"))
    assert sources, "the consumer smoke has no sources"

    wanted: set[str] = set()
    for source in sources:
        wanted.update(check_include_graph.project_includes(source.read_text(encoding="utf-8")))
    assert wanted, "the consumer includes no context/ headers — it would not prove the boundary"

    headers = {rel: "" for rel in wanted}
    prefix = make_install(tmp_path, headers)
    assert run(prefix, LIVE_CONSUMER) == 0

"""Tests for tools/gen_client_typings.py — the registry -> TypeScript client-typings projection
(R-QA-013, M9 e05a).

Covers the happy path (a synthetic schema renders every section), the properties the drift gate
DEPENDS on (byte determinism, ASCII-only output, LF line endings — a byte-unstable generator would
make ``--check`` flap), every malformed-schema refusal, and the ``--check`` surface (match, drift,
missing file) with its exit codes.

The final integration block is the strongest assertion in the file: it re-projects the REAL
committed client schema and asserts it is byte-identical to the REAL committed
``src/editor/webui/core/src/generated/client-schema.ts``. That makes this pytest suite a SECOND
drift gate, running in the fast ``python-tests`` CI job — so a registry change that skips the
regeneration is caught in ~10s rather than only by the ``webui-client-typings-drift`` ctest deep
inside the 3-OS build matrix.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from conftest import load_tool

gen_client_typings = load_tool("gen_client_typings")

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REAL_SCHEMA = REPO_ROOT / "src" / "editor" / "client" / "schema" / "context-client-schema.json"
REAL_TYPINGS = (REPO_ROOT / "src" / "editor" / "webui" / "core" / "src" / "generated"
                / "client-schema.ts")


def _schema(**overrides) -> dict:
    """A minimal but structurally complete client schema."""
    base = {
        "schemaVersion": 1,
        "protocolMajor": 1,
        "protocol": {
            "protocolMajor": 1,
            "stable": True,
            "minClientProtocol": 1,
            "capabilities": ["describe", "dry-run"],
        },
        "rpcMethods": [
            {"method": "describe", "ns": "", "noun": "", "verb": "describe",
             "stability": "stable", "deprecated": False, "params": [], "flags": ["json"]},
            {"method": "asset.move", "ns": "asset", "noun": "asset", "verb": "move",
             "stability": "stable", "deprecated": False,
             "params": ["from", "to"], "flags": ["json", "dry-run"]},
        ],
        "eventTopics": [
            {"name": "files", "description": "File change facts.",
             "payloadSchema": {"fields": [
                 {"name": "path", "type": "path", "description": "The file."}]}},
            {"name": "clients", "description": "Attach/detach.", "payloadSchema": {"fields": []}},
        ],
        "eventEnvelope": {"fields": [
            {"name": "seq", "type": "number", "description": "Monotonic sequence."}]},
        "errorCatalog": [
            {"code": "usage.invalid", "message": "Bad parse.", "retriable": False,
             "exitCode": 2, "origin": "R-CLI-007"},
            {"code": "daemon.busy", "message": "Try again.", "retriable": True,
             "exitCode": 75, "origin": "R-BRIDGE-001"},
        ],
    }
    base.update(overrides)
    return base


def _write(tmp_path: Path, schema: dict) -> Path:
    path = tmp_path / "schema.json"
    path.write_text(json.dumps(schema), encoding="utf-8")
    return path


# --- happy path ----------------------------------------------------------------------------------

def test_render_emits_every_section() -> None:
    out = gen_client_typings.render(_schema())
    # Unions
    assert '| "describe"' in out
    assert '| "asset.move"' in out
    assert '| "files"' in out
    assert '| "usage.invalid"' in out
    # Descriptor tables
    assert "export const RPC_METHODS" in out
    assert "export const EVENT_TOPICS" in out
    assert "export const ERROR_CATALOG" in out
    assert "export const EVENT_ENVELOPE_FIELDS" in out
    # Constants
    assert "export const PROTOCOL_MAJOR = 1 as const;" in out
    assert "export const MIN_CLIENT_PROTOCOL = 1 as const;" in out
    assert "export const CLIENT_SCHEMA_VERSION = 1 as const;" in out
    # Interfaces the workspace imports as types
    for name in ("RpcMethodDescriptor", "EventTopicDescriptor", "ErrorDescriptor", "PayloadField"):
        assert f"export interface {name}" in out


def test_render_carries_method_params_and_flags() -> None:
    out = gen_client_typings.render(_schema())
    assert 'params: ["from", "to"]' in out
    assert 'flags: ["json", "dry-run"]' in out


def test_render_carries_error_retriability() -> None:
    out = gen_client_typings.render(_schema())
    # The retriable fact must survive the projection — isRetriable() reads it.
    assert "retriable: true" in out
    assert "retriable: false" in out


def test_render_handles_a_topic_with_no_payload_fields() -> None:
    out = gen_client_typings.render(_schema())
    assert "payloadFields: []," in out


def test_render_handles_empty_collections() -> None:
    """A registry with nothing published still renders a VALID module (`never` unions)."""
    out = gen_client_typings.render(
        _schema(rpcMethods=[], eventTopics=[], errorCatalog=[]))
    assert "export type RpcMethod =\nnever;" in out
    assert "export const RPC_METHOD_NAMES: readonly RpcMethod[] = [];" in out


# --- properties the drift gate depends on --------------------------------------------------------

def test_render_is_deterministic() -> None:
    schema = _schema()
    assert gen_client_typings.render(schema) == gen_client_typings.render(schema)


def test_render_is_pure_ascii() -> None:
    """Non-ASCII output would be host-encoding dependent and could flap the byte comparison."""
    schema = _schema()
    schema["errorCatalog"][0]["message"] = "em dash — and accents éè"
    out = gen_client_typings.render(schema)
    out.encode("ascii")  # raises UnicodeEncodeError on regression
    assert "\\u2014" in out


def test_generate_writes_lf_line_endings(tmp_path: Path) -> None:
    """LF on every platform — a CRLF write on Windows would red the drift gate everywhere else."""
    out = tmp_path / "client-schema.ts"
    assert gen_client_typings.generate(_write(tmp_path, _schema()), out) == 0
    assert b"\r\n" not in out.read_bytes()


def test_generate_creates_missing_parent_dirs(tmp_path: Path) -> None:
    out = tmp_path / "deep" / "nested" / "client-schema.ts"
    assert gen_client_typings.generate(_write(tmp_path, _schema()), out) == 0
    assert out.is_file()


# --- --check surface -----------------------------------------------------------------------------

def test_check_passes_on_a_freshly_generated_file(tmp_path: Path) -> None:
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    assert gen_client_typings.generate(schema_path, out) == 0
    assert gen_client_typings.generate(schema_path, out, check=True) == 0


def test_check_detects_drift(tmp_path: Path) -> None:
    """The core gate: the registry moved, the committed typings did not."""
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    gen_client_typings.generate(schema_path, out)

    moved = _schema()
    moved["rpcMethods"].append(
        {"method": "brand.new", "ns": "brand", "noun": "brand", "verb": "new",
         "stability": "stable", "deprecated": False, "params": [], "flags": []})
    assert gen_client_typings.generate(_write(tmp_path, moved), out, check=True) == 1


def test_check_detects_a_hand_edit(tmp_path: Path) -> None:
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    gen_client_typings.generate(schema_path, out)
    out.write_text(out.read_text(encoding="utf-8") + "\nexport const sneaky = 1;\n",
                   encoding="utf-8")
    assert gen_client_typings.generate(schema_path, out, check=True) == 1


def test_check_reports_drift_when_the_output_is_missing(tmp_path: Path) -> None:
    schema_path = _write(tmp_path, _schema())
    assert gen_client_typings.generate(schema_path, tmp_path / "absent.ts", check=True) == 1


def test_check_does_not_write(tmp_path: Path) -> None:
    """--check must never auto-heal drift — that would defeat the gate."""
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    out.write_text("stale\n", encoding="utf-8")
    assert gen_client_typings.generate(schema_path, out, check=True) == 1
    assert out.read_text(encoding="utf-8") == "stale\n"


# --- malformed-schema refusals -------------------------------------------------------------------

@pytest.mark.parametrize("drop", ["protocol", "rpcMethods", "eventTopics", "errorCatalog",
                                  "eventEnvelope"])
def test_missing_required_section_is_a_config_error(tmp_path: Path, drop: str) -> None:
    schema = _schema()
    del schema[drop]
    with pytest.raises(gen_client_typings.GenError):
        gen_client_typings.generate(_write(tmp_path, schema), tmp_path / "o.ts")


def test_duplicate_method_names_are_refused(tmp_path: Path) -> None:
    """A duplicate key would silently collapse an entry in the emitted mapped-type table."""
    schema = _schema()
    schema["rpcMethods"].append(dict(schema["rpcMethods"][0]))
    with pytest.raises(gen_client_typings.GenError, match="duplicate"):
        gen_client_typings.generate(_write(tmp_path, schema), tmp_path / "o.ts")


def test_duplicate_error_codes_are_refused(tmp_path: Path) -> None:
    schema = _schema()
    schema["errorCatalog"].append(dict(schema["errorCatalog"][0]))
    with pytest.raises(gen_client_typings.GenError, match="duplicate"):
        gen_client_typings.generate(_write(tmp_path, schema), tmp_path / "o.ts")


def test_entry_missing_its_key_is_refused(tmp_path: Path) -> None:
    schema = _schema()
    del schema["rpcMethods"][0]["method"]
    with pytest.raises(gen_client_typings.GenError, match="method"):
        gen_client_typings.generate(_write(tmp_path, schema), tmp_path / "o.ts")


def test_non_object_entry_is_refused(tmp_path: Path) -> None:
    schema = _schema()
    schema["eventTopics"].append("not-an-object")
    with pytest.raises(gen_client_typings.GenError, match="not an object"):
        gen_client_typings.generate(_write(tmp_path, schema), tmp_path / "o.ts")


def test_unreadable_schema_is_a_config_error(tmp_path: Path) -> None:
    with pytest.raises(gen_client_typings.GenError):
        gen_client_typings.generate(tmp_path / "absent.json", tmp_path / "o.ts")


def test_non_object_schema_is_a_config_error(tmp_path: Path) -> None:
    path = tmp_path / "schema.json"
    path.write_text("[1, 2, 3]", encoding="utf-8")
    with pytest.raises(gen_client_typings.GenError, match="not a JSON object"):
        gen_client_typings.generate(path, tmp_path / "o.ts")


# --- CLI exit-code surface -----------------------------------------------------------------------

def test_main_returns_0_on_success(tmp_path: Path, capsys) -> None:
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    assert gen_client_typings.main(["--schema", str(schema_path), "--out", str(out)]) == 0
    assert "wrote" in capsys.readouterr().out


def test_main_returns_1_on_drift(tmp_path: Path, capsys) -> None:
    schema_path = _write(tmp_path, _schema())
    out = tmp_path / "client-schema.ts"
    out.write_text("stale\n", encoding="utf-8")
    assert gen_client_typings.main(
        ["--schema", str(schema_path), "--out", str(out), "--check"]) == 1
    assert "DRIFT" in capsys.readouterr().err


def test_main_returns_2_on_config_error(tmp_path: Path, capsys) -> None:
    assert gen_client_typings.main(
        ["--schema", str(tmp_path / "absent.json"), "--out", str(tmp_path / "o.ts")]) == 2
    assert "ERROR" in capsys.readouterr().err


# --- integration with the REAL committed artifacts -----------------------------------------------

def test_real_schema_exists_and_is_the_e02_artifact() -> None:
    schema = json.loads(REAL_SCHEMA.read_text(encoding="utf-8"))
    assert schema["generatedFrom"] == "contract::Registry::describe"
    assert schema["protocolMajor"] == 1


def test_committed_typings_match_the_committed_schema() -> None:
    """The pytest-tier drift gate (see the module docstring).

    Deliberately compares against the COMMITTED schema — the fast `python-tests` CI job does not
    build C++, so the build-generated schema does not exist there. The `webui-client-typings-drift`
    ctest covers the build-generated side inside the build matrix; e02's own `client-test_schema`
    gate keeps the committed schema honest against the live registry. The three together close the
    loop with no gap.
    """
    assert REAL_TYPINGS.is_file(), (
        f"{REAL_TYPINGS} is missing — regenerate with tools/gen_client_typings.py")
    schema = json.loads(REAL_SCHEMA.read_text(encoding="utf-8"))
    with open(REAL_TYPINGS, "r", encoding="utf-8", newline="") as fh:
        committed = fh.read()
    assert committed == gen_client_typings.render(schema), (
        "The committed editor-core client typings have drifted from the client schema. "
        "Regenerate: python3 tools/gen_client_typings.py")


def test_real_typings_are_ascii_and_lf() -> None:
    raw = REAL_TYPINGS.read_bytes()
    raw.decode("ascii")
    assert b"\r\n" not in raw

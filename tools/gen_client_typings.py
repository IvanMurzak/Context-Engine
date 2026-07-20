#!/usr/bin/env python3
"""Project the build-generated client schema into the editor-core JS client's TypeScript typings.

Design 05 section 3 (JS client) / D10 / the R-CLI-009 spirit: **the contract registry is the single
source of truth, so hand-written client typings are prohibited.** ``src/editor/client`` already emits
``context-client-schema.json`` from ``contract::Registry::describe`` at build time (e02); this script
is the second projection in that chain — schema -> TypeScript — so the editor's JS client can never
drift from the verbs/topics/errors the daemon actually serves.

The chain, end to end::

    contract::Registry::describe            (the one source of truth)
      -> context_client_schema_gen          (e02, a build-time C++ tool)
      -> context-client-schema.json         (build artifact + committed copy, drift-gated by e02)
      -> gen_client_typings.py  (THIS)
      -> src/editor/webui/core/src/generated/client-schema.ts   (committed, drift-gated here)

Why the generated TS is COMMITTED as well as build-generated (the same rationale as e02's committed
schema copy): the npm workspace must typecheck as a self-contained unit — a fresh checkout, an
editor/IDE, and the ``webui-typecheck`` build step all read the committed file without first
requiring a full C++ build to materialise it. The ``--check`` mode is what keeps that copy honest: it
regenerates from the BUILD-generated schema and refuses any byte difference, so a registry change
that is not reflected in the committed typings reds CI instead of silently shipping stale types.

Determinism is a correctness requirement (a byte-unstable generator would make the drift gate flap):
the emitted text is pure ASCII (``ensure_ascii=True``), preserves the schema's own ordering, and
terminates every line with ``\\n`` regardless of host platform.

Exit codes (mirrors tools/fetch_dockview.py / tools/fetch_esbuild.py):
  * 0 — wrote the typings (default mode), or they are byte-identical (``--check``).
  * 1 — ``--check`` found DRIFT (the committed typings do not match the schema) — the refusal.
  * 2 — configuration / usage error (unreadable schema, malformed shape, unwritable output).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCHEMA = REPO_ROOT / "src" / "editor" / "client" / "schema" / "context-client-schema.json"
DEFAULT_OUT = (REPO_ROOT / "src" / "editor" / "webui" / "core" / "src" / "generated"
               / "client-schema.ts")

BANNER = """\
// GENERATED FILE - DO NOT EDIT BY HAND.
//
// Projected from the contract registry's client schema by tools/gen_client_typings.py:
//   contract::Registry::describe -> context_client_schema_gen (e02) -> context-client-schema.json
//   -> gen_client_typings.py -> this file.
//
// Hand-written client typings are prohibited (design 05 section 3, the R-CLI-009 spirit): the
// registry is the single source of truth. Regenerate with
//   python3 tools/gen_client_typings.py
// The `webui-client-typings-drift` ctest re-runs the generator against the BUILD-generated schema
// and fails on any byte difference, so this file can never go stale against the daemon's surface.
"""


class GenError(Exception):
    """Configuration / malformed-input problem (maps to exit 2)."""


def _lit(value: object) -> str:
    """Encode a Python value as a deterministic, ASCII-only TypeScript literal.

    JSON's string/number/boolean/array/object syntax is a subset of TypeScript's, so `json.dumps`
    is a correct TS literal emitter here. `ensure_ascii=True` keeps the output byte-identical on
    every platform (a non-ASCII description would otherwise depend on the host's encoding).
    """
    return json.dumps(value, ensure_ascii=True, sort_keys=False)


def _union(names: list[str]) -> str:
    """Render a TS string-literal union, one member per line (stable, review-friendly diffs)."""
    if not names:
        return "never"
    return "\n".join(f"    | {_lit(n)}" for n in names)


def _require(schema: dict, key: str, kind: type) -> object:
    value = schema.get(key)
    if not isinstance(value, kind):
        raise GenError(f"schema key '{key}' missing or not a {kind.__name__}")
    return value


def _names(entries: list, key: str, what: str) -> list[str]:
    out: list[str] = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise GenError(f"{what}[{index}] is not an object")
        name = entry.get(key)
        if not isinstance(name, str) or not name:
            raise GenError(f"{what}[{index}] missing a non-empty '{key}'")
        out.append(name)
    duplicates = {n for n in out if out.count(n) > 1}
    if duplicates:
        raise GenError(f"{what} has duplicate '{key}' values: {sorted(duplicates)}")
    return out


def render(schema: dict) -> str:
    """Render the whole typings module from a parsed client schema. Pure — no IO."""
    protocol = _require(schema, "protocol", dict)
    rpc_methods = _require(schema, "rpcMethods", list)
    event_topics = _require(schema, "eventTopics", list)
    error_catalog = _require(schema, "errorCatalog", list)
    envelope = _require(schema, "eventEnvelope", dict)

    method_names = _names(rpc_methods, "method", "rpcMethods")
    topic_names = _names(event_topics, "name", "eventTopics")
    error_codes = _names(error_catalog, "code", "errorCatalog")

    parts: list[str] = [BANNER, ""]

    # --- protocol constants ----------------------------------------------------------------
    parts.append("/** The frozen wire protocol major (R-CLI-004). */")
    parts.append(f"export const PROTOCOL_MAJOR = {_lit(protocol.get('protocolMajor'))} as const;")
    parts.append("")
    parts.append("/** Oldest client protocol the daemon still accepts. */")
    parts.append("export const MIN_CLIENT_PROTOCOL = "
                 f"{_lit(protocol.get('minClientProtocol'))} as const;")
    parts.append("")
    parts.append("/** Negotiated protocol capabilities advertised by the daemon. */")
    parts.append("export const PROTOCOL_CAPABILITIES = "
                 f"{_lit(protocol.get('capabilities', []))} as const;")
    parts.append("export type ProtocolCapability = (typeof PROTOCOL_CAPABILITIES)[number];")
    parts.append("")
    parts.append(f"/** Schema version this module was projected from. */\n"
                 f"export const CLIENT_SCHEMA_VERSION = "
                 f"{_lit(schema.get('schemaVersion'))} as const;")
    parts.append("")

    # --- RPC methods -----------------------------------------------------------------------
    parts.append("/** Every RPC method the contract registry publishes. */")
    parts.append("export type RpcMethod =")
    parts.append(_union(method_names) + ";")
    parts.append("")
    parts.append("""\
/** A registry-projected description of one RPC method. */
export interface RpcMethodDescriptor {
    readonly method: RpcMethod;
    /** Verb-grammar namespace (empty for top-level verbs). */
    readonly ns: string;
    /** Verb-grammar noun (empty for top-level verbs). */
    readonly noun: string;
    readonly verb: string;
    readonly stability: string;
    readonly deprecated: boolean;
    /** Positional parameter names, in grammar order. */
    readonly params: readonly string[];
    /** Flag names accepted by this method. */
    readonly flags: readonly string[];
}""")
    parts.append("")
    parts.append("export const RPC_METHODS: { readonly [M in RpcMethod]: RpcMethodDescriptor } = {")
    for entry in rpc_methods:
        parts.append(f"    {_lit(entry['method'])}: {{")
        parts.append(f"        method: {_lit(entry['method'])},")
        parts.append(f"        ns: {_lit(entry.get('ns', ''))},")
        parts.append(f"        noun: {_lit(entry.get('noun', ''))},")
        parts.append(f"        verb: {_lit(entry.get('verb', ''))},")
        parts.append(f"        stability: {_lit(entry.get('stability', ''))},")
        parts.append(f"        deprecated: {_lit(bool(entry.get('deprecated', False)))},")
        parts.append(f"        params: {_lit(entry.get('params', []))},")
        parts.append(f"        flags: {_lit(entry.get('flags', []))},")
        parts.append("    },")
    parts.append("};")
    parts.append("")
    parts.append("/** Every RPC method name, in registry order. */")
    parts.append("export const RPC_METHOD_NAMES: readonly RpcMethod[] = "
                 f"{_lit(method_names)};")
    parts.append("")

    # --- event topics ----------------------------------------------------------------------
    parts.append("/** Every subscribable event topic. */")
    parts.append("export type EventTopic =")
    parts.append(_union(topic_names) + ";")
    parts.append("")
    parts.append("""\
/** One field of a topic's payload, as described by the registry. */
export interface PayloadField {
    readonly name: string;
    readonly type: string;
    readonly description: string;
}

/** A registry-projected description of one event topic. */
export interface EventTopicDescriptor {
    readonly name: EventTopic;
    readonly description: string;
    readonly payloadFields: readonly PayloadField[];
}""")
    parts.append("")
    parts.append("export const EVENT_TOPICS: "
                 "{ readonly [T in EventTopic]: EventTopicDescriptor } = {")
    for entry in event_topics:
        payload = entry.get("payloadSchema") or {}
        fields = payload.get("fields") or []
        parts.append(f"    {_lit(entry['name'])}: {{")
        parts.append(f"        name: {_lit(entry['name'])},")
        parts.append(f"        description: {_lit(entry.get('description', ''))},")
        if not fields:
            parts.append("        payloadFields: [],")
        else:
            parts.append("        payloadFields: [")
            for field in fields:
                parts.append(
                    f"            {{ name: {_lit(field.get('name', ''))}, "
                    f"type: {_lit(field.get('type', ''))}, "
                    f"description: {_lit(field.get('description', ''))} }},")
            parts.append("        ],")
        parts.append("    },")
    parts.append("};")
    parts.append("")
    parts.append("/** Every topic name, in registry order. */")
    parts.append(f"export const EVENT_TOPIC_NAMES: readonly EventTopic[] = {_lit(topic_names)};")
    parts.append("")

    # --- event envelope --------------------------------------------------------------------
    envelope_fields = envelope.get("fields") or []
    parts.append("/** The envelope every event frame carries (field names as the registry "
                 "describes them). */")
    parts.append("export const EVENT_ENVELOPE_FIELDS: readonly PayloadField[] = [")
    for field in envelope_fields:
        parts.append(
            f"    {{ name: {_lit(field.get('name', ''))}, "
            f"type: {_lit(field.get('type', ''))}, "
            f"description: {_lit(field.get('description', ''))} }},")
    parts.append("];")
    parts.append("")

    # --- error catalog ---------------------------------------------------------------------
    parts.append("/** Every error code in the uniform error catalog (R-CLI-007). */")
    parts.append("export type ErrorCode =")
    parts.append(_union(error_codes) + ";")
    parts.append("")
    parts.append("""\
/** A registry-projected description of one catalog error. */
export interface ErrorDescriptor {
    readonly code: ErrorCode;
    readonly message: string;
    /** Whether a client may retry the same request unchanged. */
    readonly retriable: boolean;
    readonly exitCode: number;
    /** The requirement id this error originates from. */
    readonly origin: string;
}""")
    parts.append("")
    parts.append("export const ERROR_CATALOG: { readonly [C in ErrorCode]: ErrorDescriptor } = {")
    for entry in error_catalog:
        parts.append(f"    {_lit(entry['code'])}: {{")
        parts.append(f"        code: {_lit(entry['code'])},")
        parts.append(f"        message: {_lit(entry.get('message', ''))},")
        parts.append(f"        retriable: {_lit(bool(entry.get('retriable', False)))},")
        parts.append(f"        exitCode: {_lit(entry.get('exitCode', 0))},")
        parts.append(f"        origin: {_lit(entry.get('origin', ''))},")
        parts.append("    },")
    parts.append("};")
    parts.append("")
    parts.append("/** Every error code, in catalog order. */")
    parts.append(f"export const ERROR_CODES: readonly ErrorCode[] = {_lit(error_codes)};")
    parts.append("")

    # Join with explicit \n so the output is byte-identical on Windows and POSIX.
    return "\n".join(parts)


def generate(schema_path: Path, out_path: Path, *, check: bool = False) -> int:
    try:
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GenError(f"cannot read schema {schema_path}: {exc}") from exc
    if not isinstance(schema, dict):
        raise GenError(f"schema {schema_path} is not a JSON object")

    rendered = render(schema)

    if check:
        try:
            # newline="" so Python performs NO newline translation — we compare the real bytes.
            # (open(...) rather than Path.read_text(newline=...), which is Python 3.13+ only.)
            with open(out_path, "r", encoding="utf-8", newline="") as fh:
                current = fh.read()
        except OSError as exc:
            print(f"[gen_client_typings] DRIFT: cannot read {out_path}: {exc}", file=sys.stderr)
            return 1
        if current != rendered:
            print(f"[gen_client_typings] DRIFT: {out_path} does not match the schema at "
                  f"{schema_path}.\n"
                  f"  The contract registry moved but the committed client typings did not.\n"
                  f"  Regenerate and commit:  python3 tools/gen_client_typings.py",
                  file=sys.stderr)
            return 1
        print(f"[gen_client_typings] OK: {out_path.name} matches the registry schema")
        return 0

    try:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        # newline="\n" pins LF on every platform so the drift gate never trips on line endings.
        with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(rendered)
    except OSError as exc:
        raise GenError(f"cannot write {out_path}: {exc}") from exc
    print(f"[gen_client_typings] wrote {out_path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Project the client schema into the editor-core TypeScript client typings.")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA,
                        help="client schema JSON (default: the committed e02 artifact)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="generated .ts module (default: the committed editor-core copy)")
    parser.add_argument("--check", action="store_true",
                        help="verify --out matches --schema byte-for-byte; do not write (exit 1 "
                             "on drift)")
    args = parser.parse_args(argv)

    try:
        return generate(args.schema, args.out, check=args.check)
    except GenError as exc:
        print(f"[gen_client_typings] ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

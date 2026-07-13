#!/usr/bin/env python3
"""Generate the committed `.wasm` migration-fixture corpus for the sandboxed WASM runner (issue #71 PR4).

L-37 / L-62 / R-SEC-009. The sandboxed WASM package-migration tier (`src/runtime/wasm/`) executes
package-shipped migration guests built to the FROZEN zero-import guest ABI (`ctx_alloc` /
`ctx_migrate` / optional `ctx_map_path` over the module's exported linear memory; canonical JSON
bytes crossing the wire). PR3 drove its `wasm-runner-*` ctests with WAT guests assembled at runtime
via `wasmtime_wat2wasm`; PR4 adds a COMMITTED, byte-reproducible `.wasm` fixture so the cross-OS
determinism gate (`wasm-runner-test_wasm_determinism`) exercises a stable, checked-in artifact
rather than a runtime-assembled string.

Why a hand-rolled encoder instead of `wat2wasm`/`wasmtime`: the wasmtime C-API prebuilt is a CI-only
dependency path (MSVC/Clang-ABI; it does not link under the local Strawberry-GCC Windows dev host),
and neither WABT nor a wasmtime CLI is a guaranteed tool on the executor or the `python-tests` CI
runners. This module encodes the fixture's WebAssembly binary DIRECTLY from a small readable module
description using ONLY the standard library (matching the repo's "Python tooling: stdlib + pytest"
rule) — so regeneration is deterministic and reproducible on every OS with just Python, no external
assembler. The emitted bytes are plain core-WASM MVP (i32 ops, one memory, one global, a data
segment), accepted identically by wasmtime (the runner) and V8 (a convenient local sanity check).

Usage:
  python3 tools/gen_wasm_fixtures.py            # (re)write the .wat + .wasm fixtures
  python3 tools/gen_wasm_fixtures.py --check    # verify the committed bytes match (exit 1 on drift)

Exit codes: 0 = ok (written, or --check passed); 1 = --check drift (a committed fixture is stale).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# The fixture corpus lives beside the runner it feeds.
FIXTURE_DIR = Path(__file__).resolve().parents[1] / "src" / "runtime" / "wasm" / "fixtures"

# The canonical-JSON payload the `migrate_hp` guest emits: the phys:body v1 -> v2 migration
# ({"hp":1} -> {"hp":2}), the same "real transform" stand-in PR3's kWatFixedOutput used. Kept
# COMPACT (no spaces): the host re-parses + re-canonicalizes it, so these are just valid JSON bytes.
MIGRATE_HP_OUTPUT = b'{"hp":2}'


# ---------------------------------------------------------------------------------------------------
# Minimal WebAssembly binary encoder (core MVP subset) — LEB128 + sections + one module builder.
# ---------------------------------------------------------------------------------------------------

def uleb128(value: int) -> bytes:
    """Unsigned LEB128 (section sizes, vector counts, indices, memory limits)."""
    if value < 0:
        raise ValueError("uleb128 is for non-negative integers")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb128(value: int) -> bytes:
    """Signed LEB128 (i32.const operands, data/global offset init expressions)."""
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7  # Python's >> is arithmetic, so it sign-extends negatives correctly.
        if (value == 0 and not (byte & 0x40)) or (value == -1 and (byte & 0x40)):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def vec(items: list[bytes]) -> bytes:
    """A WASM vector: uleb128 count followed by the concatenated element encodings."""
    return uleb128(len(items)) + b"".join(items)


def section(section_id: int, payload: bytes) -> bytes:
    """One WASM section: id byte + uleb128 size + payload."""
    return bytes([section_id]) + uleb128(len(payload)) + payload


def name(text: str) -> bytes:
    """A WASM name: uleb128 length + UTF-8 bytes."""
    raw = text.encode("utf-8")
    return uleb128(len(raw)) + raw


# WASM opcodes / constants used below.
_VALTYPE_I32 = 0x7F
_OP_GLOBAL_GET = 0x23
_OP_GLOBAL_SET = 0x24
_OP_LOCAL_GET = 0x20
_OP_LOCAL_SET = 0x21
_OP_I32_CONST = 0x41
_OP_I32_ADD = 0x6A
_OP_I32_STORE = 0x36
_OP_END = 0x0B


def _i32_const(value: int) -> bytes:
    return bytes([_OP_I32_CONST]) + sleb128(value)


def _i32_store(offset: int = 0) -> bytes:
    # memarg = align (log2 of the access size; i32 => 2) then offset. Natural alignment, zero offset.
    return bytes([_OP_I32_STORE]) + uleb128(2) + uleb128(offset)


def build_migrate_hp_wasm() -> bytes:
    """Encode the `migrate_hp` guest: the zero-import ctx_alloc + ctx_migrate ABI, fixed output.

    Semantics (identical to PR3's proven kWatFixedOutput, encoded to committed bytes):
      (memory (export "memory") 4)                    ; 4 pages of linear memory
      (data (i32.const 4096) "{\"hp\":2}")            ; the canonical output payload
      (global $next (mut i32) (i32.const 8192))       ; bump-allocator cursor (0 is the ABI's
                                                      ;   allocation-failure sentinel, so start high)
      (func (export "ctx_alloc") (param $size i32) (result i32)   ; bump allocator
        (local $ptr i32)
        (local.set $ptr (global.get $next))
        (global.set $next (i32.add (global.get $next) (local.get $size)))
        (local.get $ptr))
      (func (export "ctx_migrate") (param $in i32) (param $inlen i32)
                                   (param $outpp i32) (param $outlp i32) (result i32)
        (i32.store (local.get $outpp) (i32.const 4096))   ; out offset = the data segment
        (i32.store (local.get $outlp) (i32.const 8))       ; out length = len("{\"hp\":2}")
        (i32.const 0))                                     ; 0 = success (frozen ABI)
    """
    assert len(MIGRATE_HP_OUTPUT) == 8, "the ctx_migrate stored length must match the payload size"
    output_offset = 4096
    alloc_cursor_init = 8192

    # Type section: type 0 = (i32)->i32 (ctx_alloc), type 1 = (i32,i32,i32,i32)->i32 (ctx_migrate).
    functype_alloc = bytes([0x60]) + vec([bytes([_VALTYPE_I32])]) + vec([bytes([_VALTYPE_I32])])
    functype_migrate = (
        bytes([0x60])
        + vec([bytes([_VALTYPE_I32])] * 4)
        + vec([bytes([_VALTYPE_I32])])
    )
    type_sec = section(1, vec([functype_alloc, functype_migrate]))

    # Function section: func 0 -> type 0, func 1 -> type 1.
    func_sec = section(3, vec([uleb128(0), uleb128(1)]))

    # Memory section: one memory, limits {min: 4} (flag 0x00 = min only).
    memory_sec = section(5, vec([bytes([0x00]) + uleb128(4)]))

    # Global section: one mutable i32 initialised to the bump cursor.
    globaltype = bytes([_VALTYPE_I32, 0x01])  # i32, mutable
    global_init = _i32_const(alloc_cursor_init) + bytes([_OP_END])
    global_sec = section(6, vec([globaltype + global_init]))

    # Export section: memory (0x02), ctx_alloc (func 0x00 idx 0), ctx_migrate (func 0x00 idx 1).
    export_sec = section(
        7,
        vec(
            [
                name("memory") + bytes([0x02]) + uleb128(0),
                name("ctx_alloc") + bytes([0x00]) + uleb128(0),
                name("ctx_migrate") + bytes([0x00]) + uleb128(1),
            ]
        ),
    )

    # Code section.
    # ctx_alloc: locals = one i32 ($ptr, local index 1; the $size param is local 0).
    alloc_locals = vec([uleb128(1) + bytes([_VALTYPE_I32])])
    alloc_body = (
        bytes([_OP_GLOBAL_GET]) + uleb128(0)   # global.get $next
        + bytes([_OP_LOCAL_SET]) + uleb128(1)  # local.set $ptr
        + bytes([_OP_GLOBAL_GET]) + uleb128(0)  # global.get $next
        + bytes([_OP_LOCAL_GET]) + uleb128(0)   # local.get $size
        + bytes([_OP_I32_ADD])                  # i32.add
        + bytes([_OP_GLOBAL_SET]) + uleb128(0)  # global.set $next
        + bytes([_OP_LOCAL_GET]) + uleb128(1)   # local.get $ptr
        + bytes([_OP_END])
    )
    alloc_func = alloc_locals + alloc_body

    # ctx_migrate: no locals; params in=0, inlen=1, outpp=2, outlp=3.
    migrate_locals = vec([])
    migrate_body = (
        bytes([_OP_LOCAL_GET]) + uleb128(2)   # local.get $outpp
        + _i32_const(output_offset)           # i32.const 4096
        + _i32_store()                        # i32.store  -> *outpp = 4096
        + bytes([_OP_LOCAL_GET]) + uleb128(3)  # local.get $outlp
        + _i32_const(len(MIGRATE_HP_OUTPUT))   # i32.const 8
        + _i32_store()                         # i32.store  -> *outlp = 8
        + _i32_const(0)                        # i32.const 0 (success)
        + bytes([_OP_END])
    )
    migrate_func = migrate_locals + migrate_body

    code_sec = section(
        10,
        vec([uleb128(len(alloc_func)) + alloc_func, uleb128(len(migrate_func)) + migrate_func]),
    )

    # Data section: one active segment (memidx 0) writing the payload at output_offset.
    data_offset_expr = _i32_const(output_offset) + bytes([_OP_END])
    data_segment = bytes([0x00]) + data_offset_expr + uleb128(len(MIGRATE_HP_OUTPUT)) + MIGRATE_HP_OUTPUT
    data_sec = section(11, vec([data_segment]))

    magic = b"\x00asm"
    version = b"\x01\x00\x00\x00"
    return (
        magic + version
        + type_sec + func_sec + memory_sec + global_sec + export_sec + code_sec + data_sec
    )


# The human-readable WAT mirror of build_migrate_hp_wasm() — committed as the fixture's SOURCE. The
# encoder above is the byte-authoritative generator; this .wat is the reviewable description (kept in
# lockstep by test_gen_wasm_fixtures.py, which regenerates + diffs both artifacts).
MIGRATE_HP_WAT = """\
;; migrate_hp — the committed sandboxed-WASM migration fixture (issue #71 PR4).
;;
;; Built to the frozen zero-import guest ABI (src/editor/migrate/migration_runner.h, protocolMajor 1):
;; exports "memory" + ctx_alloc (bump allocator) + ctx_migrate. Zero imports (no WASI/clock/IO/
;; randomness) — the sandbox is structural. ctx_migrate performs the phys:body v1 -> v2 migration by
;; emitting the fixed canonical payload {"hp":2}; the host re-parses + re-canonicalises + re-checks
;; every structural invariant around it (budget, id immutability). Regenerate the committed .wasm with
;;   python3 tools/gen_wasm_fixtures.py
;; and pin its reproducibility with tools/tests/test_gen_wasm_fixtures.py.
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "{\\22hp\\22:2}")
  (global $next (mut i32) (i32.const 8192))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 8))
    (i32.const 0)))
"""


# ---------------------------------------------------------------------------------------------------
# The corpus: (relative filename -> generator) plus the WAT source sidecars.
# ---------------------------------------------------------------------------------------------------

WASM_FIXTURES = {
    "migrate_hp.wasm": build_migrate_hp_wasm,
}
WAT_SOURCES = {
    "migrate_hp.wat": MIGRATE_HP_WAT,
}


def _wat_bytes(text: str) -> bytes:
    # Commit LF-normalised text so the fixture is byte-identical across OS checkouts.
    return text.replace("\r\n", "\n").encode("utf-8")


def write_fixtures(fixture_dir: Path = FIXTURE_DIR) -> list[Path]:
    """(Re)write every fixture artifact. Returns the paths written."""
    fixture_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for filename, source in WAT_SOURCES.items():
        path = fixture_dir / filename
        path.write_bytes(_wat_bytes(source))
        written.append(path)
    for filename, builder in WASM_FIXTURES.items():
        path = fixture_dir / filename
        path.write_bytes(builder())
        written.append(path)
    return written


def check_fixtures(fixture_dir: Path = FIXTURE_DIR) -> list[str]:
    """Return a list of drift descriptions ([] == the committed fixtures are byte-reproducible)."""
    drift: list[str] = []
    for filename, source in WAT_SOURCES.items():
        path = fixture_dir / filename
        expected = _wat_bytes(source)
        if not path.exists():
            drift.append(f"missing fixture {filename}")
        elif path.read_bytes() != expected:
            drift.append(f"{filename} differs from its generator output")
    for filename, builder in WASM_FIXTURES.items():
        path = fixture_dir / filename
        expected = builder()
        if not path.exists():
            drift.append(f"missing fixture {filename}")
        elif path.read_bytes() != expected:
            drift.append(f"{filename} differs from its generator output")
    return drift


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the committed fixtures match the generator output; exit 1 on drift",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=FIXTURE_DIR,
        help="fixture output directory (default: src/runtime/wasm/fixtures)",
    )
    args = parser.parse_args(argv)

    if args.check:
        drift = check_fixtures(args.dest)
        if drift:
            print("wasm fixture drift (run `python3 tools/gen_wasm_fixtures.py` to regenerate):")
            for d in drift:
                print(f"  - {d}")
            return 1
        print(f"wasm fixtures reproducible ({len(WASM_FIXTURES) + len(WAT_SOURCES)} artifacts)")
        return 0

    written = write_fixtures(args.dest)
    for path in written:
        print(f"wrote {path.relative_to(args.dest.parent) if args.dest == FIXTURE_DIR else path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

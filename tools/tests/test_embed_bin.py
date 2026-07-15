"""Tests for tools/embed_bin.py — the build-time bin2c that embeds vendored fonts as C++ arrays
(M7 a7 text substrate, R-QA-013).

The generator must be deterministic (stable bytes across runs/platforms) and round-trippable (the
emitted array reproduces the input exactly), because the shipped default fonts ARE these bytes.
"""

from __future__ import annotations

import re
from pathlib import Path

from conftest import load_tool

embed_bin = load_tool("embed_bin")


def _parse_array(src: str, symbol: str) -> bytes:
    """Extract the byte values of `symbol`'s initializer list from generated C++ source."""
    m = re.search(rf"{symbol}\[\] = \{{(.*?)\}};", src, re.DOTALL)
    assert m, f"array {symbol} not found in generated source"
    hexes = re.findall(r"0x([0-9a-f]{2})", m.group(1))
    return bytes(int(h, 16) for h in hexes)


def _parse_len(src: str, symbol: str) -> int:
    m = re.search(rf"{symbol}_len = (\d+)u;", src)
    assert m, f"length for {symbol} not found"
    return int(m.group(1))


def test_roundtrip_reproduces_input():
    data = bytes(range(256)) + b"\x00\xff\x10Noto"
    src = embed_bin.render(data, "kFont", "ns::a")
    assert _parse_array(src, "kFont") == data
    assert _parse_len(src, "kFont") == len(data)


def test_namespace_and_symbol_and_external_linkage():
    src = embed_bin.render(b"abc", "kMyFont", "context::packages::ui::text::detail")
    assert "namespace context::packages::ui::text::detail" in src
    # `extern` + initializer = external linkage (the header re-declares `extern const unsigned char`).
    assert "extern const unsigned char kMyFont[] = {" in src
    assert "extern const unsigned long kMyFont_len = 3u;" in src


def test_deterministic_across_runs():
    data = b"\xde\xad\xbe\xef" * 137
    assert embed_bin.render(data, "kX", "n") == embed_bin.render(data, "kX", "n")


def test_lowercase_hex_fixed_width():
    src = embed_bin.render(b"\x0a\xbc", "kX", "n")
    assert "0x0a" in src and "0xbc" in src
    assert "0xBC" not in src  # lowercase only


def test_empty_input_has_zero_length():
    src = embed_bin.render(b"", "kEmpty", "n")
    # Zero-length arrays are ill-formed in standard C++, so a single unreachable padding byte is
    # emitted while the recorded length stays 0 (consumers size from `_len`).
    assert _parse_len(src, "kEmpty") == 0
    assert "kEmpty[] = {" in src


def test_main_writes_file(tmp_path: Path):
    src_bin = tmp_path / "in.bin"
    payload = b"\x01\x02\x03hello\xff"
    src_bin.write_bytes(payload)
    out = tmp_path / "gen" / "out.cpp"
    rc = embed_bin.main([str(src_bin), str(out), "kBlob", "--namespace", "z"])
    assert rc == 0
    text = out.read_text(encoding="utf-8")
    assert _parse_array(text, "kBlob") == payload
    assert "namespace z" in text

"""Tests for tools/wgsl_tool_eval.py — the Tint-vs-Naga evaluation harness (R-QA-013).

The harness's corpus parsing / variant enumeration / stage-source assembly REPLICATE the
conventions of src/render/material (parse_shader, enumerate_variants) and
src/render/shadercc (assemble_stage_source) — these tests pin that replication (mirroring
the C++ white-box cases in src/render/shadercc/tests/test_roundtrip.cpp) plus an
integration block over the REAL corpus, so a corpus or convention change that would skew a
re-run of the evaluation fails here first. Subprocess-driving paths (glslang/naga/tint)
are exercised by the real evaluation runs, not here — no tools are touched.
"""

from __future__ import annotations

import pytest
from conftest import load_tool

wgsl_tool_eval = load_tool("wgsl_tool_eval")

CORPUS_TEXT = """\
# comment line
shader probe
keyword FOG off on
keyword QUALITY low med high
stage vertex vs_main
#version 450
void vs_main() {}
endstage
stage fragment fs_main
#version 450
void fs_main() {}
endstage
"""


def _probe():
    return wgsl_tool_eval.parse_shader(CORPUS_TEXT, "probe.shader")


# --- corpus parsing --------------------------------------------------------------------


def test_parse_shader() -> None:
    shader = _probe()
    assert shader.name == "probe"
    assert shader.keywords == [("FOG", ["off", "on"]), ("QUALITY", ["low", "med", "high"])]
    assert [(s.kind, s.entry) for s in shader.stages] == [("vertex", "vs_main"),
                                                          ("fragment", "fs_main")]
    assert shader.stages[0].source.startswith("#version 450")


def test_parse_shader_rejects_garbage() -> None:
    with pytest.raises(SystemExit):
        wgsl_tool_eval.parse_shader("shader x\nnot a directive\n", "bad.shader")


# --- variant enumeration (must mirror material/'s enumerate_variants) --------------------


def test_enumerate_variants_odometer() -> None:
    variants = wgsl_tool_eval.enumerate_variants(_probe())
    assert len(variants) == 6  # 2 x 3
    # Keywords sorted by name; the LAST keyword varies fastest (the C++ odometer).
    assert variants[0] == [("FOG", "off"), ("QUALITY", "low")]
    assert variants[1] == [("FOG", "off"), ("QUALITY", "med")]
    assert variants[3] == [("FOG", "on"), ("QUALITY", "low")]


def test_enumerate_variants_no_keywords_is_single_default() -> None:
    shader = wgsl_tool_eval.parse_shader(
        "shader plain\nstage vertex main\n#version 450\nvoid main() {}\nendstage\n", "p.shader")
    assert wgsl_tool_eval.enumerate_variants(shader) == [[]]


# --- stage-source assembly (must mirror shadercc's assemble_stage_source) ----------------


def test_assemble_boolean_and_multivalue() -> None:
    shader = _probe()
    stage = shader.stages[0]
    src = wgsl_tool_eval.assemble_stage_source(stage, shader,
                                               [("FOG", "on"), ("QUALITY", "high")])
    assert src.startswith("#version 450")            # #version stays the first line
    assert "#define FOG 1" in src                     # boolean enabled -> defined to 1
    assert "#define low 0" in src                     # value tokens numbered by ordinal
    assert "#define med 1" in src
    assert "#define high 2" in src
    assert "#define QUALITY high" in src              # keyword bound to its selected token
    assert "#define vs_main main" in src              # entry-point trampoline


def test_assemble_boolean_disabled_left_undefined() -> None:
    shader = _probe()
    src = wgsl_tool_eval.assemble_stage_source(shader.stages[0], shader,
                                               [("FOG", "off"), ("QUALITY", "low")])
    assert "#define FOG" not in src                   # the `#ifdef KW` idiom


def test_assemble_main_entry_needs_no_trampoline() -> None:
    shader = wgsl_tool_eval.parse_shader(
        "shader plain\nstage fragment main\n#version 450\nvoid main() {}\nendstage\n", "p.shader")
    src = wgsl_tool_eval.assemble_stage_source(shader.stages[0], shader, [])
    assert "main main" not in src


# --- the real corpus stays parseable by the harness --------------------------------------


def test_real_corpus_parses_and_enumerates() -> None:
    corpus = wgsl_tool_eval.Path(__file__).resolve().parents[2] / \
        "src" / "render" / "material" / "corpus"
    files = sorted(corpus.glob("*.shader"))
    assert files, f"no corpus under {corpus}"
    total = 0
    for f in files:
        shader = wgsl_tool_eval.parse_shader(f.read_text(encoding="utf-8"), str(f))
        variants = wgsl_tool_eval.enumerate_variants(shader)
        assert shader.stages and variants
        total += len(variants) * len(shader.stages)
    # The evaluation's module count (36 as of the 2026-07-09 ruling) only ever grows.
    assert total >= 36

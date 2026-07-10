#!/usr/bin/env python3
"""Tint-vs-Naga SPIR-V->WGSL evaluation harness (R-REND-005; issue #133, sub-task D of #119).

Drives BOTH candidate WGSL tools over the SPIR-V produced from the engine's REAL authored
shader corpus (``src/render/material/corpus/*.shader``, every enumerated variant, every
stage) and measures, per tool:

  * **corpus coverage** — how many (shader, variant, stage) SPIR-V modules translate to
    WGSL at all (failures recorded with the tool's own diagnostic);
  * **validity** — every emitted WGSL module is fed back through the tool's own validator
    AND cross-validated through the OTHER tool's validator (the emitted WGSL must be
    consumable by BOTH consumers: Naga inside the in-tree wgpu-native on native, Tint
    inside Chrome on web — spikes/webgpu/FINDINGS.md §WGSL toolchain);
  * **determinism** — N repeated emissions (separate process invocations) must be
    byte-identical, the property the R-FILE-010 content-addressed shader cache rests on;
  * **cost** — wall-clock per translation (median of the N runs).

The measured results + the ruling live in ``docs/wgsl-tool-decision.md`` (the committed M4
deliverable). The harness is kept in-repo so the decision is re-measurable when either
tool moves (both are pinned there: naga-cli via ``tools/naga-toolchain.json``, Tint by the
Dawn release tag recorded in the decision doc).

Fidelity note (stated honestly): corpus parsing, variant enumeration, and the
keyword-``#define`` assembly below REPLICATE the conventions of
``src/render/material`` (``parse_shader`` / ``enumerate_variants``) and
``src/render/shadercc`` (``assemble_stage_source`` — boolean ``#ifdef`` axes vs
multi-value ``#if KW == token`` axes, entry-point trampoline, ``#version`` stays first),
and the GLSL->SPIR-V lowering uses glslangValidator with the same Vulkan 1.0 / SPIR-V 1.0
target the backend sets via the glslang C++ API. The wired ctest
(``shader-crosscompile-roundtrip``) exercises the REAL C++ seam with the chosen tool; this
harness exists to compare the two candidate tools under identical conditions.

Usage (see docs/wgsl-tool-decision.md for the reproducible container recipe):
  python3 tools/wgsl_tool_eval.py \
      --corpus src/render/material/corpus \
      --glslang glslangValidator --naga naga --tint /dawn-build/tint \
      --runs 3 --out-json report.json --out-md report.md

Exit codes: 0 = evaluation ran (regardless of per-module failures — those are FINDINGS);
2 = configuration / usage error (missing tool, unparsable corpus).
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

STAGE_EXT = {"vertex": "vert", "fragment": "frag", "compute": "comp"}

# The boolean-axis token sets — MUST mirror src/render/shadercc/src/cross_compiler.cpp
# (is_boolean_token / is_enabled_token) so the assembled sources match the real backend's.
BOOLEAN_TOKENS = {"off", "on", "0", "1", "false", "true", "no", "yes",
                  "disable", "enable", "disabled", "enabled"}
ENABLED_TOKENS = {"on", "1", "true", "yes", "enable", "enabled"}


@dataclass
class Stage:
    kind: str
    entry: str
    source: str


@dataclass
class Shader:
    name: str
    keywords: list[tuple[str, list[str]]] = field(default_factory=list)
    stages: list[Stage] = field(default_factory=list)


def parse_shader(text: str, path: str) -> Shader:
    """Parse one authored ``.shader`` corpus file (mirrors material/'s parse_shader)."""
    shader: Shader | None = None
    stage: Stage | None = None
    body: list[str] = []
    for raw in text.splitlines():
        if stage is not None:
            if raw.strip() == "endstage":
                stage.source = "\n".join(body) + "\n"
                assert shader is not None
                shader.stages.append(stage)
                stage, body = None, []
            else:
                body.append(raw)
            continue
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        tok = line.split()
        if tok[0] == "shader" and len(tok) == 2:
            shader = Shader(name=tok[1])
        elif tok[0] == "keyword" and len(tok) >= 3 and shader is not None:
            shader.keywords.append((tok[1], tok[2:]))
        elif tok[0] == "stage" and len(tok) == 3 and shader is not None:
            stage = Stage(kind=tok[1], entry=tok[2], source="")
        elif tok[0] in ("param", "texture") and shader is not None:
            # The M4 material contract (typed params + semantic texture slots, incl. the R-REND-006
            # lightmap input hook). Irrelevant to SPIR-V->WGSL translation — the contract never
            # reaches the compiled stages — so the eval skips it (material_ir.cpp validates it).
            continue
        else:
            raise SystemExit(f"[wgsl-eval] ERROR: unrecognized line in {path}: {line!r}")
    if shader is None or stage is not None:
        raise SystemExit(f"[wgsl-eval] ERROR: malformed corpus file {path}")
    return shader


def enumerate_variants(shader: Shader) -> list[list[tuple[str, str]]]:
    """All keyword-value combinations; keywords sorted by name, last varying fastest
    (mirrors material/'s enumerate_variants odometer)."""
    kws = sorted((k for k in shader.keywords if k[1]), key=lambda k: k[0])
    if not kws:
        return [[]]
    variants: list[list[tuple[str, str]]] = []
    idx = [0] * len(kws)
    while True:
        variants.append([(kws[i][0], kws[i][1][idx[i]]) for i in range(len(kws))])
        for i in reversed(range(len(kws))):
            idx[i] += 1
            if idx[i] < len(kws[i][1]):
                break
            idx[i] = 0
        else:
            return variants


def assemble_stage_source(stage: Stage, shader: Shader,
                          defines: list[tuple[str, str]]) -> str:
    """Mirror shadercc's assemble_stage_source: boolean axes -> ``#ifdef KW`` (define to 1
    only when enabled); multi-value axes -> tokens numbered + ``#define KW <token>``;
    entry-point trampoline; ``#version`` stays the first line."""
    out: list[str] = []
    defined_tokens: dict[str, int] = {}
    values_of = dict(shader.keywords)
    for name, value in defines:
        values = values_of.get(name)
        boolean = (values is not None and len(values) == 2 and
                   all(v.lower() in BOOLEAN_TOKENS for v in values))
        if boolean:
            if value.lower() in ENABLED_TOKENS:
                out.append(f"#define {name} 1")
        else:
            for i, token in enumerate(values or []):
                if defined_tokens.setdefault(token, i) != i:
                    raise SystemExit(f"[wgsl-eval] ERROR: token ordinal collision on "
                                     f"{token!r} in {shader.name}")
                if defined_tokens[token] == i and out.count(f"#define {token} {i}") == 0:
                    out.append(f"#define {token} {i}")
            out.append(f"#define {name} {value}")
    if stage.entry and stage.entry != "main":
        out.append(f"#define {stage.entry} main")

    lines = stage.source.splitlines()
    if lines and lines[0].startswith("#version"):
        return "\n".join([lines[0], *out, *lines[1:]]) + "\n"
    return "\n".join(["#version 450", *out, *lines]) + "\n"


def run(cmd: list[str], timeout: int = 120) -> tuple[int, str]:
    """Run a tool; returns (exit_code, combined diagnostics)."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stderr or p.stdout or "").strip()
    except FileNotFoundError:
        raise SystemExit(f"[wgsl-eval] ERROR: tool not found: {cmd[0]}")
    except subprocess.TimeoutExpired:
        return -1, f"timeout after {timeout}s"


def emit_cmd(tool: str, exe: str, spv: Path, wgsl: Path) -> list[str]:
    if tool == "naga":
        return [exe, str(spv), str(wgsl)]
    return [exe, "--format", "wgsl", "-o", str(wgsl), str(spv)]  # tint


def validate_cmd(tool: str, exe: str, wgsl: Path) -> list[str]:
    if tool == "naga":
        return [exe, str(wgsl)]  # parse+validate, no output
    # tint has no validate-only switch (`--format none` rejects); parse+validate+re-emit
    # to the null device is the working equivalent (probed against Dawn v20260624.223603).
    return [exe, "--format", "wgsl", "-o", os.devnull, str(wgsl)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--corpus", action="append", required=True,
                    help="corpus directory of .shader files (repeatable)")
    ap.add_argument("--glslang", default="glslangValidator")
    ap.add_argument("--naga", default="naga")
    ap.add_argument("--tint", default="tint")
    ap.add_argument("--runs", type=int, default=3,
                    help="repeated emissions for the determinism check")
    ap.add_argument("--out-json", type=Path, default=None)
    ap.add_argument("--out-md", type=Path, default=None)
    args = ap.parse_args()

    shaders: list[Shader] = []
    for d in args.corpus:
        files = sorted(Path(d).glob("*.shader"))
        if not files:
            raise SystemExit(f"[wgsl-eval] ERROR: no .shader files under {d}")
        shaders.extend(parse_shader(f.read_text(encoding="utf-8"), str(f)) for f in files)

    tools = {"naga": args.naga, "tint": args.tint}
    rows: list[dict] = []
    tmp = Path(tempfile.mkdtemp(prefix="wgsl-eval-"))

    for shader in shaders:
        for vi, defines in enumerate(enumerate_variants(shader)):
            vkey = ";".join(f"{k}={v}" for k, v in defines) or "<default>"
            for stage in shader.stages:
                base = tmp / f"{shader.name}-v{vi}-{stage.kind}"
                src = base.with_suffix("." + STAGE_EXT[stage.kind])
                spv = base.with_suffix(".spv")
                src.write_text(assemble_stage_source(stage, shader, defines),
                               encoding="utf-8")
                # Same target the C++ backend sets via the glslang API: Vulkan 1.0 / SPIR-V 1.0.
                rc, diag = run([args.glslang, "-V", "--target-env", "vulkan1.0",
                                str(src), "-o", str(spv)])
                if rc != 0:
                    raise SystemExit(f"[wgsl-eval] ERROR: glslang failed on {base.name}: {diag}")

                row = {"shader": shader.name, "variant": vkey, "stage": stage.kind}
                for tool, exe in tools.items():
                    outputs: list[bytes] = []
                    times: list[float] = []
                    rc, diag = 0, ""
                    for r in range(args.runs):
                        wgsl = base.with_suffix(f".{tool}.{r}.wgsl")
                        t0 = time.perf_counter()
                        rc, diag = run(emit_cmd(tool, exe, spv, wgsl))
                        times.append(time.perf_counter() - t0)
                        if rc != 0:
                            break
                        outputs.append(wgsl.read_bytes())
                    ok = rc == 0 and len(outputs) == args.runs
                    result = {
                        "emit_ok": ok,
                        "deterministic": ok and len(set(outputs)) == 1,
                        "median_ms": round(statistics.median(times) * 1000, 2),
                        "wgsl_bytes": len(outputs[0]) if outputs else 0,
                        "diag": "" if ok else diag,
                    }
                    if ok:
                        wgsl0 = base.with_suffix(f".{tool}.0.wgsl")
                        for vtool, vexe in tools.items():
                            vrc, vdiag = run(validate_cmd(vtool, vexe, wgsl0))
                            result[f"valid_{vtool}"] = vrc == 0
                            if vrc != 0:
                                result[f"valid_{vtool}_diag"] = vdiag
                    row[tool] = result
                rows.append(row)
                print(f"[wgsl-eval] {shader.name} {vkey} {stage.kind}: " +
                      " ".join(f"{t}={'ok' if row[t]['emit_ok'] else 'FAIL'}"
                               for t in tools), flush=True)

    report = {"runs": args.runs, "modules": len(rows), "rows": rows}
    for tool in tools:
        ok = [r for r in rows if r[tool]["emit_ok"]]
        report[f"{tool}_summary"] = {
            "emitted": f"{len(ok)}/{len(rows)}",
            "deterministic": all(r[tool]["deterministic"] for r in ok),
            "self_valid": all(r[tool].get(f"valid_{tool}", False) for r in ok),
            "cross_valid": all(all(r[tool].get(f"valid_{v}", False) for v in tools)
                               for r in ok),
            "median_ms": round(statistics.median(
                [r[tool]["median_ms"] for r in rows] or [0]), 2),
        }

    if args.out_json:
        args.out_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.out_md:
        lines = ["| shader | variant | stage | " +
                 " | ".join(f"{t} emit/det/self/cross" for t in tools) + " |",
                 "| --- | --- | --- | " + " | ".join("---" for _ in tools) + " |"]
        for r in rows:
            cells = []
            for t in tools:
                d = r[t]
                if d["emit_ok"]:
                    cells.append("/".join([
                        "ok",
                        "yes" if d["deterministic"] else "NO",
                        "yes" if d.get(f"valid_{t}") else "NO",
                        "yes" if all(d.get(f"valid_{v}") for v in tools) else "NO",
                    ]))
                elif d["diag"]:
                    cells.append("FAIL: " + d["diag"].splitlines()[0][:80])
                else:
                    cells.append("FAIL")
            lines.append(f"| {r['shader']} | {r['variant']} | {r['stage']} | " +
                         " | ".join(cells) + " |")
        args.out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(json.dumps({k: v for k, v in report.items() if k != "rows"}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

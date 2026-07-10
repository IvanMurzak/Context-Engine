"""Tests for tools/golden_compare.py — the M4 golden-scene SSIM gate (R-QA-013 coverage).

Covers the PPM reader (happy path, comments, malformed headers, wrong payload size, bad maxval),
the block-SSIM metric (identical -> 1.0, noise lowers it, structural change breaks it, ordering),
the compare verdict (pass / below-tolerance / shape mismatch), and the CLI against a synthetic
corpus manifest (exit codes 0 / 1 / 2). The LIVE goldens/ corpus is validated at the end: every
scene in the committed manifest resolves to a committed baseline that self-compares at SSIM 1.0.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import pytest
from conftest import load_tool

golden_compare = load_tool("golden_compare")

REPO_ROOT = Path(__file__).resolve().parents[2]
LIVE_CORPUS_MANIFEST = REPO_ROOT / "goldens" / "manifest.json"


def make_ppm(path: Path, width: int, height: int, pixel_fn) -> None:
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    body = bytearray()
    for y in range(height):
        for x in range(width):
            body.extend(pixel_fn(x, y))
    path.write_bytes(header + bytes(body))


def flat(r: int, g: int, b: int):
    return lambda x, y: bytes((r, g, b))


# ---------------------------------------------------------------------------
# PPM reader
# ---------------------------------------------------------------------------


def test_read_ppm_happy(tmp_path):
    p = tmp_path / "a.ppm"
    make_ppm(p, 16, 8, flat(10, 20, 30))
    w, h, rgb = golden_compare.read_ppm(p)
    assert (w, h) == (16, 8)
    assert rgb[:3] == bytes((10, 20, 30))
    assert len(rgb) == 16 * 8 * 3


def test_read_ppm_tolerates_header_comment(tmp_path):
    p = tmp_path / "c.ppm"
    p.write_bytes(b"P6\n# a comment\n2 1\n255\n" + bytes(6))
    w, h, rgb = golden_compare.read_ppm(p)
    assert (w, h) == (2, 1)
    assert len(rgb) == 6


@pytest.mark.parametrize("payload", [
    b"P5\n2 1\n255\n" + bytes(6),          # wrong magic
    b"P6\n2 1\n65535\n" + bytes(12),       # unsupported maxval
    b"P6\n2 1\n255\n" + bytes(5),          # short payload
    b"P6\n2 x\n255\n" + bytes(6),          # non-numeric header
    b"P6\n2 1\n",                           # truncated header
])
def test_read_ppm_rejects_malformed(tmp_path, payload):
    p = tmp_path / "bad.ppm"
    p.write_bytes(payload)
    with pytest.raises(ValueError):
        golden_compare.read_ppm(p)


# ---------------------------------------------------------------------------
# Block-SSIM metric
# ---------------------------------------------------------------------------


def gradient_pixels(width, height):
    return [float((x * 7 + y * 13) % 256) for y in range(height) for x in range(width)]


def test_ssim_identical_is_one():
    lum = gradient_pixels(32, 32)
    mean, worst, _ = golden_compare.block_ssim(lum, list(lum), 32, 32)
    assert mean == pytest.approx(1.0)
    assert worst == pytest.approx(1.0)


def test_ssim_noise_lowers_but_structural_change_breaks():
    rng = random.Random(42)
    lum = gradient_pixels(32, 32)
    noisy = [v + rng.uniform(-2.0, 2.0) for v in lum]
    inverted = [255.0 - v for v in lum]
    mean_noisy, _, _ = golden_compare.block_ssim(lum, noisy, 32, 32)
    mean_inv, _, _ = golden_compare.block_ssim(lum, inverted, 32, 32)
    assert 0.9 < mean_noisy < 1.0
    assert mean_inv < mean_noisy  # a structural change scores far below sensor-level noise


def test_ssim_reports_worst_window_location():
    lum = [100.0] * (32 * 32)
    damaged = list(lum)
    for dy in range(8):  # ruin exactly the window at (24, 24)
        for dx in range(8):
            damaged[(24 + dy) * 32 + (24 + dx)] = 255.0 if (dx + dy) % 2 else 0.0
    mean, worst, worst_at = golden_compare.block_ssim(lum, damaged, 32, 32)
    assert worst_at == (24, 24)
    assert worst < 0.5 < mean


def test_ssim_covers_non_multiple_edges():
    # 20x12 with an 8-window: clamped edge windows must still be sampled (no crash, full range).
    lum = gradient_pixels(20, 12)
    mean, _, _ = golden_compare.block_ssim(lum, list(lum), 20, 12)
    assert mean == pytest.approx(1.0)


# ---------------------------------------------------------------------------
# compare() verdicts
# ---------------------------------------------------------------------------


def test_compare_pass_and_shape_mismatch(tmp_path):
    a = tmp_path / "a.ppm"
    b = tmp_path / "b.ppm"
    make_ppm(a, 16, 16, flat(50, 100, 150))
    make_ppm(b, 16, 16, flat(50, 100, 150))
    verdict = golden_compare.compare(a, b, 0.99)
    assert verdict["pass"] and verdict["ssim_mean"] == pytest.approx(1.0)
    assert verdict["max_abs_channel_delta"] == 0

    c = tmp_path / "c.ppm"
    make_ppm(c, 8, 8, flat(50, 100, 150))
    verdict = golden_compare.compare(a, c, 0.99)
    assert not verdict["pass"] and "shape mismatch" in verdict["reason"]


def test_compare_below_tolerance(tmp_path):
    a = tmp_path / "a.ppm"
    b = tmp_path / "b.ppm"
    make_ppm(a, 16, 16, lambda x, y: bytes((x * 8 % 256, y * 8 % 256, 0)))
    make_ppm(b, 16, 16, lambda x, y: bytes((255 - x * 8 % 256, 255 - y * 8 % 256, 255)))
    verdict = golden_compare.compare(a, b, 0.999)
    assert not verdict["pass"]


# ---------------------------------------------------------------------------
# CLI (manifest-driven)
# ---------------------------------------------------------------------------


def write_corpus(tmp_path: Path, min_ssim: float = 0.99) -> Path:
    goldens = tmp_path / "goldens"
    goldens.mkdir()
    make_ppm(goldens / "scene.ppm", 16, 16, flat(10, 200, 30))
    manifest = goldens / "manifest.json"
    manifest.write_text(json.dumps({
        "scenes": {"scene": {"file": "scene.ppm", "min_ssim": min_ssim}}}), encoding="utf-8")
    return manifest


def test_main_pass(tmp_path, capsys):
    manifest = write_corpus(tmp_path)
    candidate = tmp_path / "candidate.ppm"
    make_ppm(candidate, 16, 16, flat(10, 200, 30))
    rc = golden_compare.main(["--scene", "scene", "--candidate", str(candidate),
                              "--manifest", str(manifest)])
    assert rc == 0
    assert '"pass": true' in capsys.readouterr().out


def test_main_fail_below_tolerance(tmp_path):
    manifest = write_corpus(tmp_path, min_ssim=0.9999)
    candidate = tmp_path / "candidate.ppm"
    make_ppm(candidate, 16, 16, lambda x, y: bytes((255 - x * 16 % 256, 0, y * 16 % 256)))
    rc = golden_compare.main(["--scene", "scene", "--candidate", str(candidate),
                              "--manifest", str(manifest)])
    assert rc == 1


def test_main_config_errors(tmp_path):
    manifest = write_corpus(tmp_path)
    candidate = tmp_path / "candidate.ppm"
    make_ppm(candidate, 16, 16, flat(10, 200, 30))
    # unknown scene
    assert golden_compare.main(["--scene", "ghost", "--candidate", str(candidate),
                                "--manifest", str(manifest)]) == 2
    # missing manifest
    assert golden_compare.main(["--scene", "scene", "--candidate", str(candidate),
                                "--manifest", str(tmp_path / "nope.json")]) == 2
    # missing candidate file
    assert golden_compare.main(["--scene", "scene", "--candidate", str(tmp_path / "nope.ppm"),
                                "--manifest", str(manifest)]) == 2


# ---------------------------------------------------------------------------
# The LIVE committed corpus
# ---------------------------------------------------------------------------


def test_live_corpus_manifest_and_baselines_are_consistent():
    manifest = json.loads(LIVE_CORPUS_MANIFEST.read_text(encoding="utf-8"))
    scenes = manifest["scenes"]
    assert {"triangle3d", "sprite2d", "lit3d"} <= set(scenes)
    for scene_id, scene in scenes.items():
        golden = LIVE_CORPUS_MANIFEST.parent / scene["file"]
        assert golden.is_file(), f"{scene_id}: committed baseline missing"
        assert 0.9 <= scene["min_ssim"] <= 1.0
        verdict = golden_compare.compare(golden, golden, scene["min_ssim"])
        assert verdict["pass"] and verdict["ssim_mean"] == pytest.approx(1.0)


def test_live_corpus_gates_via_cli():
    scenes = json.loads(LIVE_CORPUS_MANIFEST.read_text(encoding="utf-8"))["scenes"]
    for scene_id, scene in scenes.items():
        golden = LIVE_CORPUS_MANIFEST.parent / scene["file"]
        rc = golden_compare.main(["--scene", scene_id, "--candidate", str(golden),
                                  "--manifest", str(LIVE_CORPUS_MANIFEST)])
        assert rc == 0

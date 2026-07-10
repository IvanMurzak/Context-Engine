#!/usr/bin/env python3
"""Golden-scene visual-equivalence gate (M4 T7, issue #141 — ROADMAP §1 M4 exit; R-REND-002).

Compares one rendered corpus frame (a binary PPM dumped by `context_render_wgpu_offscreen golden`
natively, or collected from the browser harness by tools/web_golden_run.py) against its committed
baseline under goldens/, using the NAMED perceptual metric the corpus manifest declares:

  mean block-SSIM — SSIM (Wang et al.) computed per 8x8 non-overlapping window on Rec.709 luma
  (K1=0.01, K2=0.03, L=255), averaged over all windows. 1.0 = identical; per-scene minima live in
  goldens/manifest.json ("identical within the T1 feature set" = same semantics, NOT bit-identical
  frames — float->unorm rounding legally differs per backend).

Pure stdlib (the corpus interchange format is PPM precisely so no image dependency exists anywhere
in the chain). Rebaselines are REVIEWED changes: this tool only compares — to rebaseline, replace
the committed goldens/*.ppm in a PR that explains the visual change (goldens/README.md).

Exit code 0 = within tolerance; 1 = below tolerance or shape mismatch; 2 = configuration error.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# SSIM constants (Wang et al. defaults over 8-bit luma).
SSIM_K1 = 0.01
SSIM_K2 = 0.03
SSIM_L = 255.0
SSIM_WINDOW = 8


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    """Read a binary P6 PPM (maxval 255). Returns (width, height, rgb_bytes).

    Raises ValueError on any malformed input. Header comments (#) and arbitrary whitespace are
    tolerated per the PPM spec; only maxval 255 is accepted (the corpus is 8-bit by contract).
    """
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError(f"{path}: not a binary PPM (missing P6 magic)")

    # Tokenize the header: magic, width, height, maxval — skipping comments/whitespace.
    tokens: list[bytes] = []
    pos = 2
    while len(tokens) < 3 and pos < len(data):
        ch = data[pos : pos + 1]
        if ch == b"#":
            while pos < len(data) and data[pos : pos + 1] != b"\n":
                pos += 1
        elif ch.isspace():
            pos += 1
        else:
            start = pos
            while pos < len(data) and not data[pos : pos + 1].isspace():
                pos += 1
            tokens.append(data[start:pos])
    if len(tokens) != 3:
        raise ValueError(f"{path}: truncated PPM header")
    pos += 1  # exactly ONE whitespace byte separates the header from the payload

    try:
        width, height, maxval = (int(t) for t in tokens)
    except ValueError as exc:
        raise ValueError(f"{path}: non-numeric PPM header token") from exc
    if width <= 0 or height <= 0:
        raise ValueError(f"{path}: bad dimensions {width}x{height}")
    if maxval != 255:
        raise ValueError(f"{path}: unsupported maxval {maxval} (corpus contract is 255)")

    payload = data[pos:]
    expected = width * height * 3
    if len(payload) != expected:
        raise ValueError(f"{path}: payload is {len(payload)} bytes, want {expected}")
    return width, height, payload


def luma(rgb: bytes) -> list[float]:
    """Rec.709 luma per pixel from packed RGB bytes."""
    out: list[float] = []
    for i in range(0, len(rgb), 3):
        out.append(0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2])
    return out


def block_ssim(luma_a: list[float], luma_b: list[float], width: int, height: int,
               window: int = SSIM_WINDOW) -> tuple[float, float, tuple[int, int]]:
    """Mean block-SSIM over non-overlapping `window`-square luma windows.

    Returns (mean_ssim, worst_window_ssim, worst_window_origin_xy). Edge remainders (when the
    image is not a window multiple) are covered by clamped windows anchored to the far edge.
    """
    c1 = (SSIM_K1 * SSIM_L) ** 2
    c2 = (SSIM_K2 * SSIM_L) ** 2

    xs = list(range(0, width - window + 1, window))
    ys = list(range(0, height - window + 1, window))
    if not xs or xs[-1] != width - window:
        xs.append(max(0, width - window))
    if not ys or ys[-1] != height - window:
        ys.append(max(0, height - window))

    total = 0.0
    count = 0
    worst = 1.0
    worst_at = (0, 0)
    n = float(window * window)
    for y0 in ys:
        for x0 in xs:
            sum_a = sum_b = sum_aa = sum_bb = sum_ab = 0.0
            for dy in range(window):
                row = (y0 + dy) * width + x0
                for dx in range(window):
                    a = luma_a[row + dx]
                    b = luma_b[row + dx]
                    sum_a += a
                    sum_b += b
                    sum_aa += a * a
                    sum_bb += b * b
                    sum_ab += a * b
            mu_a = sum_a / n
            mu_b = sum_b / n
            var_a = sum_aa / n - mu_a * mu_a
            var_b = sum_bb / n - mu_b * mu_b
            cov = sum_ab / n - mu_a * mu_b
            ssim = ((2.0 * mu_a * mu_b + c1) * (2.0 * cov + c2)) / (
                (mu_a * mu_a + mu_b * mu_b + c1) * (var_a + var_b + c2))
            total += ssim
            count += 1
            if ssim < worst:
                worst = ssim
                worst_at = (x0, y0)
    return total / count, worst, worst_at


def compare(golden_path: Path, candidate_path: Path, min_ssim: float) -> dict:
    """Compare two PPMs; returns the verdict document (raises ValueError on malformed input)."""
    gw, gh, golden_rgb = read_ppm(golden_path)
    cw, ch, candidate_rgb = read_ppm(candidate_path)
    if (gw, gh) != (cw, ch):
        return {
            "pass": False,
            "reason": f"shape mismatch: golden {gw}x{gh} vs candidate {cw}x{ch}",
            "min_ssim": min_ssim,
        }
    mean, worst, worst_at = block_ssim(luma(golden_rgb), luma(candidate_rgb), gw, gh)
    max_abs_delta = max(
        (abs(a - b) for a, b in zip(golden_rgb, candidate_rgb)), default=0)
    return {
        "pass": mean >= min_ssim,
        "ssim_mean": round(mean, 6),
        "ssim_worst_window": round(worst, 6),
        "ssim_worst_window_at": list(worst_at),
        "max_abs_channel_delta": max_abs_delta,
        "min_ssim": min_ssim,
        "width": gw,
        "height": gh,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--scene", required=True, help="corpus scene id (a goldens/manifest.json key)")
    ap.add_argument("--candidate", required=True, help="the rendered frame to gate (binary PPM)")
    ap.add_argument("--manifest", default="goldens/manifest.json",
                    help="corpus manifest (scenes, per-scene tolerances, metric definition)")
    ap.add_argument("--goldens-dir", default=None,
                    help="baseline directory (default: the manifest's own directory)")
    args = ap.parse_args(argv)

    manifest_path = Path(args.manifest)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[golden-compare] ERROR: cannot load manifest {manifest_path}: {exc}",
              file=sys.stderr)
        return 2

    scene = manifest.get("scenes", {}).get(args.scene)
    if not isinstance(scene, dict):
        print(f"[golden-compare] ERROR: scene {args.scene!r} not in {manifest_path}",
              file=sys.stderr)
        return 2
    min_ssim = scene.get("min_ssim")
    golden_file = scene.get("file")
    if not isinstance(min_ssim, (int, float)) or not isinstance(golden_file, str):
        print(f"[golden-compare] ERROR: scene {args.scene!r} needs numeric 'min_ssim' + 'file'",
              file=sys.stderr)
        return 2

    goldens_dir = Path(args.goldens_dir) if args.goldens_dir else manifest_path.parent
    golden_path = goldens_dir / golden_file
    candidate_path = Path(args.candidate)

    try:
        verdict = compare(golden_path, candidate_path, float(min_ssim))
    except (OSError, ValueError) as exc:
        print(f"[golden-compare] ERROR: {exc}", file=sys.stderr)
        return 2

    verdict["scene"] = args.scene
    verdict["golden"] = str(golden_path)
    verdict["candidate"] = str(candidate_path)
    print(f"[golden-compare] {json.dumps(verdict, sort_keys=True)}")
    if verdict["pass"]:
        return 0
    print(f"[golden-compare] FAIL: scene {args.scene!r} below tolerance "
          f"(see goldens/README.md — rebaselines are reviewed changes, never automatic)",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

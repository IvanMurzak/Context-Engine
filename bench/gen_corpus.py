#!/usr/bin/env python3
"""Deterministic seeded corpus generator for the R-FILE-011 scale benchmark.

Emits synthetic Context projects matching the LOCKED authored file format:

  * canonical-JSON scene files (L-32) with `$schema` + `version` + `componentVersions`,
    stable intra-file ids (>= 64-bit random, file-scoped; L-33) and id-keyed child
    collections (entities carry an `id` field; components are keyed by type);
  * scene composition/instancing with id-path-addressed per-field overrides (L-35);
  * `.meta.json` GUID sidecars for every asset (L-36), with the reserved `platforms` block;
  * dual-form cross-file references `{"$ref": "<guid>", "path": "<hint>"}` (L-34) and
    same-file entity references `{"$entity": "<id>"}`;
  * binary sidecar payloads referenced as `{"$sidecar": "<relpath>", "hash": "..."}` (L-33),
    owned satellites living next to their owning scene;
  * a dense-reference variant (`--variant dense`) whose ref-edge count is much larger than
    its file count (R-FILE-011(e): edges are O(refs) > O(files)).

Determinism: every file's content is derived from (seed, file index) alone — cross-file
facts (a target scene's entity/instance ids, an asset's GUID) are RE-DERIVED from the seed
rather than looked up, so generation is order-independent and parallelizes with identical
output for any worker count.

Canonical form emitted (mirrors the C++ canonicalizer in spikes/parse-bench):
  UTF-8, LF, 2-space indent, `": "` key separator, keys sorted lexicographically by UTF-8
  bytes, ECMAScript shortest-round-trip number formatting (`-0` -> `0`, no NaN/Inf),
  NFC-normalized strings (the generator only emits NFC), trailing newline.

File mix (fractions of the total file count N; documented in bench/README.md):
  36% scene JSON files + 36% scene meta sidecars
   9% binary assets      +  9% binary-asset meta sidecars
  10% binary sidecar payloads (owned satellites of the first scenes; no meta of their own)

Usage:
  python bench/gen_corpus.py --size 1000   --out bench/corpora/corpus-1k
  python bench/gen_corpus.py --size 10000  --out bench/corpora/corpus-10k
  python bench/gen_corpus.py --size 100000 --out bench/corpora/corpus-100k
  python bench/gen_corpus.py --size 10000 --variant dense --out bench/corpora/corpus-10k-dense
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import math
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

# ---------------------------------------------------------------------------
# Deterministic derivation primitives
# ---------------------------------------------------------------------------


def _h(*parts: object) -> bytes:
    """Stable 32-byte digest of the given parts (the corpus' derivation root)."""
    m = hashlib.sha256()
    for p in parts:
        b = str(p).encode("utf-8")
        m.update(struct.pack("<I", len(b)))
        m.update(b)
    return m.digest()


def rng_u64(*parts: object) -> int:
    return struct.unpack("<Q", _h(*parts)[:8])[0]


def rng_float(*parts: object) -> float:
    """Uniform [0, 1)."""
    return rng_u64(*parts) / 2**64


def rng_int(lo: int, hi: int, *parts: object) -> int:
    """Uniform integer in [lo, hi]."""
    return lo + rng_u64(*parts) % (hi - lo + 1)


def entity_id(seed: int, scene_idx: int, k: int) -> str:
    """Stable intra-file id: 64-bit collision-resistant random, file-scoped (L-33)."""
    return f"{rng_u64(seed, 'scene', scene_idx, 'ent', k):016x}"


def instance_id(seed: int, scene_idx: int, m: int) -> str:
    return f"{rng_u64(seed, 'scene', scene_idx, 'inst', m):016x}"


def guid_of(seed: int, kind: str, idx: int) -> str:
    """Asset GUID (L-36): UUID-shaped, derived from the seed."""
    d = _h(seed, "guid", kind, idx)
    hx = d.hex()
    return f"{hx[0:8]}-{hx[8:12]}-{hx[12:16]}-{hx[16:20]}-{hx[20:32]}"


# ---------------------------------------------------------------------------
# Canonical JSON writer (mirrors the C++ canonicalizer; ECMAScript numbers)
# ---------------------------------------------------------------------------

_ESCAPES = {
    '"': '\\"', "\\": "\\\\", "\b": "\\b", "\f": "\\f",
    "\n": "\\n", "\r": "\\r", "\t": "\\t",
}


def ecma_number(v: float | int) -> str:
    """ECMAScript Number::toString for the JSON number domain (R-FILE-001).

    Integral doubles print without a decimal point; fixed notation for decimal
    exponents in [-6, 20]; otherwise exponent notation ("1e+21", "1e-7").
    -0 serializes as 0.
    """
    if isinstance(v, int):
        return str(v)
    if v != v or v in (float("inf"), float("-inf")):
        raise ValueError("NaN/Infinity are banned in authored files (R-FILE-001)")
    if v == 0.0:
        return "0"  # -0 is defined: serialized as 0
    # repr() gives the shortest round-trip digits; re-shape to ECMAScript notation.
    r = repr(v)
    neg = r.startswith("-")
    if neg:
        r = r[1:]
    if "e" in r or "E" in r:
        mant, _, exp = r.partition("e")
        k10 = int(exp)
    else:
        mant, k10 = r, 0
    if "." in mant:
        int_part, _, frac = mant.partition(".")
    else:
        int_part, frac = mant, ""
    digits = (int_part + frac).lstrip("0")
    # decimal exponent n: value = 0.digits * 10^n
    n = len(int_part.lstrip("0")) + k10 if int_part.lstrip("0") else k10 - (
        len(frac) - len(frac.lstrip("0")))
    digits = digits.rstrip("0") or "0"
    k = len(digits)
    if k <= n <= 21:
        s = digits + "0" * (n - k)
    elif 0 < n <= 21:
        s = digits[:n] + "." + digits[n:]
    elif -6 < n <= 0:
        s = "0." + "0" * (-n) + digits
    else:
        e = n - 1
        mant_s = digits[0] + ("." + digits[1:] if k > 1 else "")
        s = f"{mant_s}e{'+' if e >= 0 else '-'}{abs(e)}"
    return ("-" + s) if neg else s


def _canon_string(s: str) -> str:
    out = ['"']
    for ch in s:
        esc = _ESCAPES.get(ch)
        if esc:
            out.append(esc)
        elif ord(ch) < 0x20:
            out.append(f"\\u{ord(ch):04x}")
        else:
            out.append(ch)  # non-ASCII passes through as raw UTF-8 (NFC by construction)
    out.append('"')
    return "".join(out)


# Keys recur constantly (component/field names) while values are unbounded — memoize
# escaping for keys only.
_canon_key = functools.lru_cache(maxsize=None)(_canon_string)


def canonical_json(value, indent: int = 0) -> str:
    """Canonical serialization: sorted keys, 2-space indent, LF (R-FILE-001/L-32)."""
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, (int, float)):
        return ecma_number(value)
    if isinstance(value, str):
        return _canon_string(value)
    if isinstance(value, list):
        if not value:
            return "[]"
        pad = "  " * indent
        pad_in = "  " * (indent + 1)
        items = ",\n".join(pad_in + canonical_json(v, indent + 1) for v in value)
        return "[\n" + items + "\n" + pad + "]"
    if isinstance(value, dict):
        if not value:
            return "{}"
        pad = "  " * indent
        pad_in = "  " * (indent + 1)
        # UTF-8 byte order == code-point order for valid strings, so a plain sort
        # implements the R-FILE-001 "sorted lexicographically by UTF-8 bytes" rule.
        keys = sorted(value.keys())
        items = ",\n".join(
            f"{pad_in}{_canon_key(k)}: {canonical_json(value[k], indent + 1)}"
            for k in keys)
        return "{\n" + items + "\n" + pad + "}"
    raise TypeError(f"unsupported type: {type(value)!r}")


def write_canonical(path: Path, value) -> int:
    data = (canonical_json(value) + "\n").encode("utf-8")
    path.write_bytes(data)
    return len(data)


# ---------------------------------------------------------------------------
# Corpus shape parameters
# ---------------------------------------------------------------------------

SCHEMA_BASE = "https://schemas.context-engine.dev/m0"

COMPONENT_VERSIONS = {
    "ctx:Transform": 3, "ctx:MeshRenderer": 2, "ctx:RigidBody": 1, "ctx:Collider": 2,
    "ctx:Script": 1, "ctx:Light": 2, "ctx:Camera": 1, "ctx:AudioSource": 1,
    "ctx:SceneSettings": 1, "ctx:EnvironmentLighting": 1, "ctx:BakedCurves": 1,
    "game:Health": 1, "game:Inventory": 1, "game:AIBehavior": 2,
}

NAME_WORDS = [
    "crate", "barrel", "torch", "pillar", "door", "lever", "guard", "turret", "spawn",
    "trigger", "platform", "bridge", "gate", "beacon", "camp", "tower", "wall", "rock",
    "tree", "bush", "lamp", "sign", "chest", "portal", "shrine", "fountain", "статуя",
    "階段", "colonne", "übergang",  # NFC unicode minority share
]

# File-mix fractions (of total corpus file count). Only the fractions plan_counts()
# reads are listed; scenes + scene metas (36% each, per the module docstring) are the
# remainder.
MIX = {"bin_asset": 0.09, "sidecar": 0.10}


def plan_counts(total: int) -> dict[str, int]:
    """Exact integer counts summing to `total`, honoring pairings (asset<->meta)."""
    n_bin = max(1, round(total * MIX["bin_asset"]))
    n_sidecar = max(1, round(total * MIX["sidecar"]))
    # scenes + scene metas take the remainder; scenes == scene metas.
    n_scene = (total - 2 * n_bin - n_sidecar) // 2
    n_sidecar = total - 2 * n_scene - 2 * n_bin  # absorb rounding into sidecars
    assert 2 * n_scene + 2 * n_bin + n_sidecar == total
    assert n_sidecar <= n_scene, "each sidecar is owned by a distinct scene"
    return {"scene": n_scene, "bin": n_bin, "sidecar": n_sidecar}


def scene_relpath(i: int) -> str:
    return f"project/scenes/{i % 256:02x}/scene-{i:06d}.scene.json"


def bin_relpath(i: int) -> str:
    ext = ["mesh.bin", "tex.bin", "anim.bin"][i % 3]
    return f"project/assets/{i % 256:02x}/asset-{i:06d}.{ext}"


def sidecar_relpath(scene_idx: int) -> str:
    # Owned satellite: lives next to its owning scene (L-33).
    return f"project/scenes/{scene_idx % 256:02x}/scene-{scene_idx:06d}.curves.bin"


def scene_entity_count(seed: int, i: int, n_scene: int) -> int:
    """Log-normal-ish entity count in [4, 1000] (L-33 soft ceiling ~1k entities).

    A deterministic handful of "ceiling" scenes sit at the ~1 MB / ~1k-entity soft cap.
    """
    if n_scene >= 100 and i % (n_scene // 50) == 0 and i > 0:
        return 1000  # ceiling scene
    u = rng_float(seed, "entcount", i)
    v = math.exp(3.0 + 1.1 * _inv_norm(u))  # median ~e^3 ≈ 20, long tail
    return max(4, min(1000, int(v)))


def _inv_norm(u: float) -> float:
    """Acklam-style inverse normal CDF approximation (deterministic, stdlib-only)."""
    if u <= 0.0:
        return -8.0
    if u >= 1.0:
        return 8.0
    # Peter Acklam's rational approximation
    a = [-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00]
    b = [-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01]
    c = [-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00]
    d = [7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00]
    plow, phigh = 0.02425, 1 - 0.02425
    if u < plow:
        q = math.sqrt(-2 * math.log(u))
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1)
    if u > phigh:
        q = math.sqrt(-2 * math.log(1 - u))
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1)
    q = u - 0.5
    r = q * q
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / \
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1)


# ---------------------------------------------------------------------------
# Content builders
# ---------------------------------------------------------------------------


def fnum(seed: int, *parts: object, lo: float = -100.0, hi: float = 100.0) -> float:
    """A float rounded to 4 decimals (keeps authored files diff-friendly and small)."""
    return round(lo + (hi - lo) * rng_float(seed, *parts), 4)


def make_ref(seed: int, kind: str, idx: int, path: str) -> dict:
    """Dual-form reference (L-34)."""
    return {"$ref": guid_of(seed, kind, idx), "path": path}


def build_component(seed: int, i: int, k: int, ctype: str, n_bin: int, n_ent: int) -> dict:
    s = (seed, "comp", i, k, ctype)
    if ctype == "ctx:Transform":
        return {
            "position": {"x": fnum(*s, "px"), "y": fnum(*s, "py"), "z": fnum(*s, "pz")},
            "rotation": {"x": fnum(*s, "rx", lo=-1, hi=1), "y": fnum(*s, "ry", lo=-1, hi=1),
                         "z": fnum(*s, "rz", lo=-1, hi=1), "w": fnum(*s, "rw", lo=-1, hi=1)},
            "scale": {"x": fnum(*s, "sx", lo=0.1, hi=10), "y": fnum(*s, "sy", lo=0.1, hi=10),
                      "z": fnum(*s, "sz", lo=0.1, hi=10)},
        }
    if ctype == "ctx:MeshRenderer":
        mesh_idx = rng_int(0, n_bin - 1, *s, "mesh")
        mats = [make_ref(seed, "bin", (mat_idx := rng_int(0, n_bin - 1, *s, "mat", j)),
                         bin_relpath(mat_idx))
                for j in range(rng_int(1, 3, *s, "nmat"))]
        return {"mesh": make_ref(seed, "bin", mesh_idx, bin_relpath(mesh_idx)),
                "materials": mats,
                "castShadows": rng_u64(*s, "cast") % 2 == 0}
    if ctype == "ctx:RigidBody":
        return {"mass": fnum(*s, "mass", lo=0.1, hi=500), "kinematic": rng_u64(*s, "kin") % 4 == 0,
                "drag": fnum(*s, "drag", lo=0, hi=2)}
    if ctype == "ctx:Collider":
        shape = ["box", "sphere", "capsule"][rng_int(0, 2, *s, "shape")]
        if shape == "box":
            geo = {"type": "box", "halfExtents": {"x": fnum(*s, "hx", lo=0.1, hi=8),
                                                  "y": fnum(*s, "hy", lo=0.1, hi=8),
                                                  "z": fnum(*s, "hz", lo=0.1, hi=8)}}
        elif shape == "sphere":
            geo = {"type": "sphere", "radius": fnum(*s, "rad", lo=0.1, hi=8)}
        else:
            geo = {"type": "capsule", "radius": fnum(*s, "rad", lo=0.1, hi=4),
                   "height": fnum(*s, "hgt", lo=0.5, hi=6)}
        return {"geometry": geo, "isTrigger": rng_u64(*s, "trig") % 8 == 0}
    if ctype == "ctx:Script":
        src_idx = rng_int(0, n_bin - 1, *s, "src")
        return {"source": make_ref(seed, "bin", src_idx, bin_relpath(src_idx)),
                "properties": {"speed": fnum(*s, "speed", lo=0, hi=30),
                               "label": NAME_WORDS[rng_int(0, len(NAME_WORDS) - 1, *s, "lbl")],
                               "enabled": rng_u64(*s, "en") % 2 == 0}}
    if ctype == "ctx:Light":
        return {"type": ["point", "spot", "directional"][rng_int(0, 2, *s, "lt")],
                "color": {"r": fnum(*s, "cr", lo=0, hi=1), "g": fnum(*s, "cg", lo=0, hi=1),
                          "b": fnum(*s, "cb", lo=0, hi=1)},
                "intensity": fnum(*s, "in", lo=0, hi=20), "range": fnum(*s, "rg", lo=1, hi=100)}
    if ctype == "ctx:Camera":
        return {"fov": fnum(*s, "fov", lo=0.5, hi=2.6), "near": 0.1, "far": 1000,
                "projection": "perspective"}
    if ctype == "ctx:AudioSource":
        clip_idx = rng_int(0, n_bin - 1, *s, "clip")
        return {"clip": make_ref(seed, "bin", clip_idx, bin_relpath(clip_idx)),
                "volume": fnum(*s, "vol", lo=0, hi=1), "loop": rng_u64(*s, "loop") % 2 == 0,
                "spatial": rng_u64(*s, "sp") % 2 == 0}
    if ctype == "game:Health":
        return {"max": rng_int(10, 500, *s, "max"), "regenPerSecond": fnum(*s, "regen", lo=0, hi=5)}
    if ctype == "game:Inventory":
        return {"slots": rng_int(4, 40, *s, "slots"),
                "items": [{"id": f"{rng_u64(*s, 'item', j):016x}",
                           "kind": NAME_WORDS[rng_int(0, len(NAME_WORDS) - 1, *s, 'ik', j)],
                           "count": rng_int(1, 99, *s, "ic", j)}
                          for j in range(rng_int(0, 5, *s, "nitems"))]}
    if ctype == "game:AIBehavior":
        # Same-file entity ref (L-34 sibling form).
        target = entity_id(seed, i, rng_int(0, max(0, n_ent - 1), *s, "target"))
        return {"mode": ["patrol", "guard", "chase"][rng_int(0, 2, *s, "mode")],
                "target": {"$entity": target},
                "aggroRange": fnum(*s, "aggro", lo=1, hi=50),
                "notes": "authored annotation lives in schema-blessed notes fields (L-32)"}
    raise ValueError(ctype)


OPTIONAL_COMPONENTS = ["ctx:MeshRenderer", "ctx:RigidBody", "ctx:Collider", "ctx:Script",
                       "ctx:Light", "ctx:Camera", "ctx:AudioSource", "game:Health",
                       "game:Inventory", "game:AIBehavior"]


def build_scene(seed: int, i: int, counts: dict, dense: bool,
                sidecar_payload: bytes | None = None) -> dict:
    """Build scene `i`. For sidecar-owning scenes (i < counts["sidecar"]) the caller may
    pass the materialized sidecar payload so its bytes are generated only once."""
    n_scene, n_bin, n_sidecar = counts["scene"], counts["bin"], counts["sidecar"]
    n_ent = scene_entity_count(seed, i, n_scene)
    used_versions: dict[str, int] = {"ctx:Transform": COMPONENT_VERSIONS["ctx:Transform"],
                                     "ctx:SceneSettings": COMPONENT_VERSIONS["ctx:SceneSettings"]}

    entities = []
    for k in range(n_ent):
        comps: dict[str, object] = {
            "ctx:Transform": build_component(seed, i, k, "ctx:Transform", n_bin, n_ent)}
        # Dense variant: many more ref-carrying components per entity (R-FILE-011(e)).
        n_opt = rng_int(2, 6, seed, "nopt", i, k) if dense else rng_int(1, 3, seed, "nopt", i, k)
        for j in range(n_opt):
            ctype = OPTIONAL_COMPONENTS[rng_int(0, len(OPTIONAL_COMPONENTS) - 1,
                                                seed, "opt", i, k, j)]
            if ctype not in comps:
                comps[ctype] = build_component(seed, i, k, ctype, n_bin, n_ent)
                used_versions[ctype] = COMPONENT_VERSIONS[ctype]
        parent = None
        if k > 0 and rng_u64(seed, "parent", i, k) % 3 != 0:
            parent = entity_id(seed, i, rng_int(0, k - 1, seed, "pidx", i, k))
        ent = {"id": entity_id(seed, i, k),
               "name": f"{NAME_WORDS[rng_int(0, len(NAME_WORDS) - 1, seed, 'nm', i, k)]}-{k}",
               "components": comps}
        if parent is not None:
            ent["parent"] = parent
        entities.append(ent)

    # Scene composition (L-35): instance earlier scenes (DAG by construction: target < i).
    instances = []
    if i > 0:
        max_inst = rng_int(4, 10, seed, "ninst", i) if dense else rng_int(0, 3, seed, "ninst", i)
        for m in range(max_inst):
            target = rng_int(0, i - 1, seed, "itgt", i, m)
            overrides = []
            t_ents = scene_entity_count(seed, target, n_scene)
            for o in range(rng_int(0, 4, seed, "nov", i, m)):
                ent_k = rng_int(0, t_ents - 1, seed, "ovent", i, m, o)
                # id-path: [instanceId, ..., entityId] — one instancing level here;
                # deeper paths occur when `target` itself instances scenes.
                id_path = [instance_id(seed, i, m), entity_id(seed, target, ent_k)]
                field = ["/position/x", "/position/y", "/position/z",
                         "/scale/x", "/rotation/w"][rng_int(0, 4, seed, "ovf", i, m, o)]
                overrides.append({
                    "at": id_path,
                    "component": "ctx:Transform",
                    "fieldPath": field,
                    "value": fnum(seed, "ovv", i, m, o),
                })
            inst = {"id": instance_id(seed, i, m),
                    "scene": make_ref(seed, "scene", target, scene_relpath(target)),
                    "overrides": overrides}
            instances.append(inst)

    root_comps: dict[str, object] = {
        "ctx:SceneSettings": {
            "ambientColor": {"r": fnum(seed, "amb", i, "r", lo=0, hi=1),
                             "g": fnum(seed, "amb", i, "g", lo=0, hi=1),
                             "b": fnum(seed, "amb", i, "b", lo=0, hi=1)},
            "gravity": {"x": 0, "y": -9.81, "z": 0},
            "timeScale": 1,
        }}
    if i < n_sidecar:
        # This scene owns a binary sidecar payload (L-33).
        if sidecar_payload is None:
            sidecar_payload = sidecar_bytes(seed, i)
        root_comps["ctx:BakedCurves"] = {
            "$sidecar": os.path.basename(sidecar_relpath(i)),
            "hash": sidecar_hash(sidecar_payload),
        }
        used_versions["ctx:BakedCurves"] = COMPONENT_VERSIONS["ctx:BakedCurves"]

    return {
        "$schema": f"{SCHEMA_BASE}/scene.schema.json",
        "version": 1,
        "componentVersions": used_versions,
        "root": {"id": entity_id(seed, i, 1_000_000), "name": "SceneRoot",
                 "components": root_comps},
        "entities": entities,
        "instances": instances,
    }


def sidecar_bytes(seed: int, i: int) -> bytes:
    """Deterministic pseudo-random binary payload, 4–64 KiB (a few at ~1 MiB)."""
    if i % 97 == 0:
        size = 1 << 20
    else:
        size = 4096 + rng_u64(seed, "scsize", i) % (60 * 1024)
    return _pseudo_bytes(_h(seed, "sidecar", i), size)


def bin_asset_bytes(seed: int, i: int) -> bytes:
    """Binary asset payload, 16 KiB–512 KiB (a few at ~2 MiB)."""
    if i % 89 == 0:
        size = 2 << 20
    else:
        size = 16 * 1024 + rng_u64(seed, "binsize", i) % (496 * 1024)
    return _pseudo_bytes(_h(seed, "bin", i), size)


def _pseudo_bytes(key: bytes, size: int) -> bytes:
    """Fast deterministic byte stream (BLAKE2 in counter mode)."""
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.blake2b(key + struct.pack("<Q", counter), digest_size=64).digest() * 64
        counter += 1
    return bytes(out[:size])


def sidecar_hash(payload: bytes) -> str:
    """Raw-byte hash recorded in the owning scene's $sidecar ref (R-FILE-001).

    sha256 stands in for the engine's content hash in the M0 corpus.
    """
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def build_meta(seed: int, kind: str, idx: int, asset_rel: str) -> dict:
    s = (seed, "meta", kind, idx)
    if kind == "scene":
        importer = {"name": "ctx.scene", "version": 1}
    elif kind == "bin":
        importer = {"name": ["ctx.mesh", "ctx.texture", "ctx.animation"][idx % 3],
                    "version": 2,
                    "settings": {"compression": ["none", "lz4", "zstd"][rng_int(0, 2, *s, "cmp")],
                                 "quality": fnum(*s, "q", lo=0, hi=1),
                                 "generateMips": rng_u64(*s, "mips") % 2 == 0}}
    else:
        raise ValueError(kind)
    return {
        "$schema": f"{SCHEMA_BASE}/meta.schema.json",
        "version": 1,
        "guid": guid_of(seed, kind, idx),
        "importer": importer,
        "platforms": {},  # reserved per-platform import-setting overrides (L-36)
    }


# ---------------------------------------------------------------------------
# Generation driver
# ---------------------------------------------------------------------------


def count_edges(doc: dict) -> int:
    """Count cross-file reference edges ($ref / $sidecar) in a document."""
    n = 0
    stack = [doc]
    while stack:
        v = stack.pop()
        if isinstance(v, dict):
            if "$ref" in v or "$sidecar" in v:
                n += 1
            stack.extend(v.values())
        elif isinstance(v, list):
            stack.extend(v)
    return n


def _new_stats() -> dict:
    """Zeroed per-worker stats accumulator (merged with _merge_stats)."""
    return {"files": 0, "bytes": 0, "json_bytes": 0, "bin_bytes": 0, "edges": 0,
            "scenes": 0, "metas": 0, "bins": 0, "sidecars": 0}


def _merge_stats(totals: dict, part: dict) -> None:
    for k, v in part.items():
        totals[k] = totals.get(k, 0) + v


def _gen_range(args_tuple) -> dict:
    """Worker: generate files for scene indices [lo, hi). Returns stats."""
    seed, lo, hi, out, counts, dense = args_tuple
    out = Path(out)
    stats = _new_stats()
    for i in range(lo, hi):
        # The owned sidecar payload (first n_sidecar scenes) is materialized ONCE; the
        # scene's embedded hash and the payload file both derive from the same bytes.
        payload = sidecar_bytes(seed, i) if i < counts["sidecar"] else None
        # Scene + its meta
        doc = build_scene(seed, i, counts, dense, sidecar_payload=payload)
        p = out / scene_relpath(i)
        n = write_canonical(p, doc)
        stats["files"] += 1
        stats["scenes"] += 1
        stats["bytes"] += n
        stats["json_bytes"] += n
        stats["edges"] += count_edges(doc)
        meta = build_meta(seed, "scene", i, scene_relpath(i))
        n = write_canonical(p.with_name(p.name + ".meta.json"), meta)
        stats["files"] += 1
        stats["metas"] += 1
        stats["bytes"] += n
        stats["json_bytes"] += n
        if payload is not None:
            (out / sidecar_relpath(i)).write_bytes(payload)
            stats["files"] += 1
            stats["sidecars"] += 1
            stats["bytes"] += len(payload)
            stats["bin_bytes"] += len(payload)
    return stats


def _gen_bin_range(args_tuple) -> dict:
    seed, lo, hi, out = args_tuple
    out = Path(out)
    stats = _new_stats()
    for i in range(lo, hi):
        b = bin_asset_bytes(seed, i)
        p = out / bin_relpath(i)
        p.write_bytes(b)
        stats["files"] += 1
        stats["bins"] += 1
        stats["bytes"] += len(b)
        stats["bin_bytes"] += len(b)
        meta = build_meta(seed, "bin", i, bin_relpath(i))
        n = write_canonical(p.with_name(p.name + ".meta.json"), meta)
        stats["files"] += 1
        stats["metas"] += 1
        stats["bytes"] += n
        stats["json_bytes"] += n
    return stats


def generate(size: int, out: Path | str, seed: int = 20260702,
             variant: str = "standard", jobs: int | None = None) -> dict:
    """Importable generation API (the CLI in main() is a thin wrapper around this).

    Generates a corpus of `size` files under `out` and returns the manifest dict
    (also written to <out>/corpus-manifest.json). Raises FileExistsError when `out`
    exists and is not empty, ValueError for an unknown variant.
    """
    out = Path(out)
    if out.exists() and any(out.iterdir()):
        raise FileExistsError(f"output dir {out} exists and is not empty")
    if variant not in ("standard", "dense"):
        raise ValueError(f"unknown variant: {variant!r}")
    if jobs is None:
        jobs = os.cpu_count() or 4
    dense = variant == "dense"
    counts = plan_counts(size)

    # Pre-create shard directories (workers then never race on mkdir).
    for shard in range(256):
        (out / f"project/scenes/{shard:02x}").mkdir(parents=True, exist_ok=True)
        (out / f"project/assets/{shard:02x}").mkdir(parents=True, exist_ok=True)

    t0 = time.perf_counter()
    tasks = []
    chunk = max(64, counts["scene"] // (jobs * 8) or 1)
    for lo in range(0, counts["scene"], chunk):
        tasks.append(("scene", (seed, lo, min(lo + chunk, counts["scene"]),
                                str(out), counts, dense)))
    bchunk = max(64, counts["bin"] // (jobs * 4) or 1)
    for lo in range(0, counts["bin"], bchunk):
        tasks.append(("bin", (seed, lo, min(lo + bchunk, counts["bin"]), str(out))))

    totals: dict[str, int] = {}
    if jobs > 1:
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            futs = [ex.submit(_gen_range if k == "scene" else _gen_bin_range, a)
                    for k, a in tasks]
            for f in futs:
                _merge_stats(totals, f.result())
    else:
        for k, a in tasks:
            _merge_stats(totals, (_gen_range if k == "scene" else _gen_bin_range)(a))
    dt = time.perf_counter() - t0

    manifest = {
        "generator": "bench/gen_corpus.py",
        "seed": seed,
        "variant": variant,
        "requested_size": size,
        "counts": {
            "total_files": totals["files"],
            "scenes": totals["scenes"],
            "metas": totals["metas"],
            "binary_assets": totals["bins"],
            "binary_sidecars": totals["sidecars"],
        },
        "bytes": {"total": totals["bytes"], "json": totals["json_bytes"],
                  "binary": totals["bin_bytes"]},
        "ref_edges": totals["edges"],
        "edges_per_file": round(totals["edges"] / max(1, totals["files"]), 2),
    }
    write_canonical(out / "corpus-manifest.json", manifest)
    print(f"[gen_corpus] wrote {totals['files']} files "
          f"({totals['bytes'] / 1e6:.1f} MB) in {dt:.1f}s", file=sys.stderr)
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--size", type=int, required=True,
                    help="total corpus file count (e.g. 1000, 10000, 100000)")
    ap.add_argument("--out", required=True, help="output corpus directory")
    ap.add_argument("--seed", type=int, default=20260702, help="deterministic seed")
    ap.add_argument("--variant", choices=["standard", "dense"], default="standard",
                    help="dense = high ref-fan-out corpus (edges O(refs) > O(files))")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                    help="parallel workers (output is identical for any worker count)")
    args = ap.parse_args()

    try:
        manifest = generate(size=args.size, out=Path(args.out), seed=args.seed,
                            variant=args.variant, jobs=args.jobs)
    except FileExistsError:
        print(f"error: output dir {Path(args.out)} exists and is not empty", file=sys.stderr)
        return 2
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Tests for bench/gen_corpus.py — the seeded R-FILE-011 corpus generator.

Retroactive R-QA-013 coverage: seed determinism (byte-identical trees), corpus shape
against the locked authored-format spec (L-32…L-36, as documented in bench/README.md),
scale-parameter honesty, and the dense-ref variant's edges>files property.

Corpora are generated into session-scoped tmp dirs; the largest fixture is 1k files.
"""

from __future__ import annotations

import hashlib
import importlib
import json
import re
import sys
from pathlib import Path

import pytest

BENCH_DIR = Path(__file__).resolve().parents[1]
if str(BENCH_DIR) not in sys.path:
    sys.path.insert(0, str(BENCH_DIR))  # real sys.path entry so ProcessPool workers can unpickle
gen_corpus = importlib.import_module("gen_corpus")

GUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
ID_RE = re.compile(r"^[0-9a-f]{16}$")

SMALL, SEED_A, SEED_B = 200, 12345, 54321


# ---------------------------------------------------------------------------
# Helpers + fixtures
# ---------------------------------------------------------------------------


def tree_hash(root: Path) -> str:
    """SHA-256 over (sorted relpath, content) of every file — byte-identity of a tree."""
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        if p.is_file():
            h.update(p.relative_to(root).as_posix().encode())
            h.update(b"\0")
            h.update(p.read_bytes())
            h.update(b"\0")
    return h.hexdigest()


def json_docs(root: Path):
    for p in sorted(root.rglob("*.json")):
        yield p, json.loads(p.read_text(encoding="utf-8"))


def iter_ref_dicts(doc):
    """Yield every dict node in a parsed JSON document."""
    stack = [doc]
    while stack:
        v = stack.pop()
        if isinstance(v, dict):
            yield v
            stack.extend(v.values())
        elif isinstance(v, list):
            stack.extend(v)


@pytest.fixture(scope="session")
def corpus_small(tmp_path_factory):
    out = tmp_path_factory.mktemp("corpus") / "small"
    manifest = gen_corpus.generate(size=SMALL, out=out, seed=SEED_A, jobs=1)
    return out, manifest


@pytest.fixture(scope="session")
def corpus_small_repeat(tmp_path_factory):
    out = tmp_path_factory.mktemp("corpus") / "small-repeat"
    manifest = gen_corpus.generate(size=SMALL, out=out, seed=SEED_A, jobs=1)
    return out, manifest


@pytest.fixture(scope="session")
def corpus_other_seed(tmp_path_factory):
    out = tmp_path_factory.mktemp("corpus") / "other-seed"
    manifest = gen_corpus.generate(size=SMALL, out=out, seed=SEED_B, jobs=1)
    return out, manifest


@pytest.fixture(scope="session")
def corpus_dense(tmp_path_factory):
    out = tmp_path_factory.mktemp("corpus") / "dense"
    manifest = gen_corpus.generate(size=SMALL, out=out, seed=SEED_A, variant="dense", jobs=1)
    return out, manifest


@pytest.fixture(scope="session")
def corpus_1k(tmp_path_factory):
    out = tmp_path_factory.mktemp("corpus") / "corpus-1k"
    manifest = gen_corpus.generate(size=1000, out=out, seed=20260702, jobs=1)
    return out, manifest


# ---------------------------------------------------------------------------
# Seed determinism
# ---------------------------------------------------------------------------


def test_same_seed_is_byte_identical(corpus_small, corpus_small_repeat):
    (a, ma), (b, mb) = corpus_small, corpus_small_repeat
    assert tree_hash(a) == tree_hash(b)
    assert ma == mb


def test_different_seed_differs(corpus_small, corpus_other_seed):
    (a, _), (b, _) = corpus_small, corpus_other_seed
    assert tree_hash(a) != tree_hash(b)


def test_output_independent_of_worker_count(tmp_path_factory):
    """Documented invariant: identical output for any --jobs value (order-independent)."""
    base = tmp_path_factory.mktemp("jobs")
    gen_corpus.generate(size=128, out=base / "j1", seed=SEED_A, jobs=1)
    gen_corpus.generate(size=128, out=base / "j2", seed=SEED_A, jobs=2)
    assert tree_hash(base / "j1") == tree_hash(base / "j2")


# ---------------------------------------------------------------------------
# Corpus shape (L-32…L-36)
# ---------------------------------------------------------------------------


def test_all_json_is_canonical_fixpoint(corpus_small):
    """Every emitted JSON file is canonical: re-serializing the parse is the identity."""
    root, _ = corpus_small
    checked = 0
    for path, doc in json_docs(root):
        raw = path.read_text(encoding="utf-8")
        assert gen_corpus.canonical_json(doc) + "\n" == raw, f"non-canonical: {path}"
        checked += 1
    assert checked > 100  # 200-file corpus carries >100 JSON files


def test_scene_shape_and_stable_intra_file_ids(corpus_small):
    root, _ = corpus_small
    scenes = sorted(root.glob("project/scenes/*/*.scene.json"))
    assert scenes
    for path in scenes:
        doc = json.loads(path.read_text(encoding="utf-8"))
        assert doc["$schema"].endswith("scene.schema.json")
        assert doc["version"] == 1
        assert isinstance(doc["componentVersions"], dict) and doc["componentVersions"]
        # id-keyed child collections are ARRAYS of objects carrying an `id` member (L-33).
        assert isinstance(doc["entities"], list)
        assert isinstance(doc["instances"], list)
        ids = set()
        for ent in doc["entities"]:
            assert isinstance(ent, dict)
            assert ID_RE.match(ent["id"]), f"bad entity id in {path}"
            assert isinstance(ent["components"], dict) and "ctx:Transform" in ent["components"]
            ids.add(ent["id"])
        assert len(ids) == len(doc["entities"]), f"duplicate entity ids in {path}"
        # parent references resolve within the same file
        for ent in doc["entities"]:
            if "parent" in ent:
                assert ent["parent"] in ids
        inst_ids = set()
        for inst in doc["instances"]:
            assert ID_RE.match(inst["id"])
            inst_ids.add(inst["id"])
            assert set(inst["scene"]) == {"$ref", "path"}  # dual-form scene ref
            for ov in inst["overrides"]:
                assert isinstance(ov["at"], list) and ov["at"]
                assert ov["at"][0] == inst["id"]  # id-path starts at this instance (L-35)
                assert all(ID_RE.match(x) for x in ov["at"])
                assert ov["component"] in doc["componentVersions"]
                assert ov["fieldPath"].startswith("/")
        assert len(inst_ids) == len(doc["instances"])


def test_meta_sidecars_pair_one_to_one(corpus_small):
    root, _ = corpus_small
    project = root / "project"
    files = [p for p in project.rglob("*") if p.is_file()]
    metas = {p for p in files if p.name.endswith(".meta.json")}
    assets = {p for p in files if not p.name.endswith(".meta.json")
              and not p.name.endswith(".curves.bin")}
    sidecars = {p for p in files if p.name.endswith(".curves.bin")}
    # every scene/asset has exactly one meta; every meta points at an existing asset
    assert {p.with_name(p.name + ".meta.json") for p in assets} == metas
    # binary sidecar payloads are owned satellites: NO meta of their own (L-33/L-36)
    assert sidecars
    for p in sidecars:
        assert not p.with_name(p.name + ".meta.json").exists()


def test_meta_shape(corpus_small):
    root, _ = corpus_small
    for path in sorted(root.rglob("*.meta.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        assert doc["$schema"].endswith("meta.schema.json")
        assert doc["version"] == 1
        assert GUID_RE.match(doc["guid"]), f"bad guid in {path}"
        assert doc["importer"]["name"].startswith("ctx.")
        assert doc["platforms"] == {}  # reserved per-platform block (L-36)


def test_dual_form_refs_wellformed_and_resolvable(corpus_small):
    """Every {"$ref"} is dual-form (guid + path hint), and both forms agree:
    the path exists in the corpus and its .meta.json carries the same guid."""
    root, _ = corpus_small
    guid_by_path: dict[str, str] = {}
    for path in root.rglob("*.meta.json"):
        rel = path.relative_to(root).as_posix()[: -len(".meta.json")]
        guid_by_path[rel] = json.loads(path.read_text(encoding="utf-8"))["guid"]

    checked = 0
    for path, doc in json_docs(root):
        for node in iter_ref_dicts(doc):
            if "$ref" in node:
                assert isinstance(node["path"], str) and node["path"], \
                    f"$ref without path hint in {path}"
                assert GUID_RE.match(node["$ref"])
                assert node["path"] in guid_by_path, \
                    f"$ref path {node['path']} not in corpus ({path})"
                assert guid_by_path[node["path"]] == node["$ref"], \
                    f"$ref guid does not match target meta guid ({path})"
                checked += 1
            if "$entity" in node:
                assert ID_RE.match(node["$entity"])
    assert checked > SMALL  # plenty of cross-file edges even in the standard variant


def test_sidecar_refs_hash_verified(corpus_small):
    """{"$sidecar"} refs: payload exists next to the owning scene, hash matches bytes."""
    root, _ = corpus_small
    checked = 0
    for path in sorted(root.glob("project/scenes/*/*.scene.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        for node in iter_ref_dicts(doc):
            if "$sidecar" in node:
                payload = path.with_name(node["$sidecar"])  # owned satellite, same dir
                assert payload.is_file(), f"missing sidecar {node['$sidecar']} for {path}"
                digest = "sha256:" + hashlib.sha256(payload.read_bytes()).hexdigest()
                assert node["hash"] == digest, f"sidecar hash mismatch for {path}"
                checked += 1
    _, manifest = corpus_small
    assert checked == manifest["counts"]["binary_sidecars"]


def test_manifest_matches_disk(corpus_small):
    root, manifest = corpus_small
    files = [p for p in (root / "project").rglob("*") if p.is_file()]
    assert len(files) == manifest["counts"]["total_files"]
    c = manifest["counts"]
    assert c["scenes"] == len(list(root.glob("project/scenes/*/*.scene.json")))
    assert c["metas"] == len([p for p in files if p.name.endswith(".meta.json")])
    assert c["binary_sidecars"] == len([p for p in files if p.name.endswith(".curves.bin")])
    assert c["total_files"] == c["scenes"] + c["metas"] + c["binary_assets"] + c["binary_sidecars"]
    # manifest ref_edges equals an independent recount over all JSON docs
    edges = 0
    for path, doc in json_docs(root):
        if path.name == "corpus-manifest.json":
            continue
        edges += sum(1 for node in iter_ref_dicts(doc) if "$ref" in node or "$sidecar" in node)
    assert edges == manifest["ref_edges"]


# ---------------------------------------------------------------------------
# Scale parameters
# ---------------------------------------------------------------------------


def test_scale_1k_corpus_has_exactly_1000_files(corpus_1k):
    root, manifest = corpus_1k
    assert manifest["requested_size"] == 1000
    assert manifest["counts"]["total_files"] == 1000  # exact, not approximate
    on_disk = sum(1 for p in (root / "project").rglob("*") if p.is_file())
    assert on_disk == 1000
    # documented file mix (bench/README.md): 36/36/9/9/10 within integer rounding
    c = manifest["counts"]
    assert c["scenes"] == c["metas"] - c["binary_assets"]  # scene metas == scenes
    assert abs(c["scenes"] - 360) <= 2
    assert abs(c["binary_assets"] - 90) <= 2
    assert abs(c["binary_sidecars"] - 100) <= 2


# ---------------------------------------------------------------------------
# Dense-ref variant (R-FILE-011(e))
# ---------------------------------------------------------------------------


def test_dense_variant_has_more_edges_than_files(corpus_dense):
    _, manifest = corpus_dense
    assert manifest["variant"] == "dense"
    assert manifest["ref_edges"] > manifest["counts"]["total_files"]


def test_dense_variant_denser_than_standard(corpus_small, corpus_dense):
    (_, std), (_, dense) = corpus_small, corpus_dense
    assert dense["counts"]["total_files"] == std["counts"]["total_files"]
    assert dense["ref_edges"] > std["ref_edges"]


# ---------------------------------------------------------------------------
# API / CLI error paths
# ---------------------------------------------------------------------------


def test_generate_refuses_nonempty_output_dir(tmp_path):
    out = tmp_path / "occupied"
    out.mkdir()
    (out / "leftover.txt").write_text("x", encoding="utf-8")
    with pytest.raises(FileExistsError):
        gen_corpus.generate(size=SMALL, out=out, seed=SEED_A, jobs=1)


def test_generate_rejects_unknown_variant(tmp_path):
    with pytest.raises(ValueError):
        gen_corpus.generate(size=SMALL, out=tmp_path / "x", seed=SEED_A,
                            variant="bogus", jobs=1)


def test_cli_nonempty_output_dir_exits_2(tmp_path, monkeypatch, capsys):
    out = tmp_path / "occupied"
    out.mkdir()
    (out / "leftover.txt").write_text("x", encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["gen_corpus.py", "--size", "64",
                                      "--out", str(out), "--jobs", "1"])
    assert gen_corpus.main() == 2
    assert "exists and is not empty" in capsys.readouterr().err

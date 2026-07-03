"""Tests for tools/verify_artifact.py — the R-SEC-009 verify-before-use gate (L-58).

R-QA-013 coverage: the happy path (a valid detached signature by a trusted key passes)
AND the full fail-closed surface (bad / missing / tampered / untrusted-key / wrong-namespace
/ wrong-identity signatures, a missing pinned trust root, and an absent ssh-keygen all
REFUSE). Nothing is ever "verified with a warning".

Two fixture styles are used deliberately:
  * COMMITTED fixtures (tools/tests/fixtures/) — a TEST-ONLY public key, its pinned
    allowed_signers, a sample artifact, and a pre-made detached signature. Verifying a
    signature the test did NOT just mint mirrors the real deployment shape (an artifact
    fetched together with a signature produced elsewhere) and guards the exact on-disk
    byte layout / signature format.
  * EPHEMERAL keys minted at test time via ssh-keygen — needed only where a test must
    control the PRIVATE half (e.g. to forge a valid-but-untrusted signature). No private
    key is ever committed to the repo.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import load_tool

verify_artifact = load_tool("verify_artifact")

FIXTURES = Path(__file__).resolve().parent / "fixtures"
COMMITTED_TRUST_ROOT = FIXTURES / "allowed_signers"
SAMPLE_ARTIFACT = FIXTURES / "sample-artifact.bin"
SAMPLE_SIG = FIXTURES / "sample-artifact.bin.sig"

TEST_IDENTITY = "context-engine-test"
NAMESPACE = "context-engine-artifact"

SSH_KEYGEN = shutil.which("ssh-keygen")

# The gate is a thin, deliberate wrapper over ssh-keygen; without it there is nothing to
# exercise. ssh-keygen ships with OpenSSH and is present on every CI runner, so this skip
# never triggers in CI — it only guards a pathological dev box with no OpenSSH.
pytestmark = pytest.mark.skipif(SSH_KEYGEN is None, reason="ssh-keygen (OpenSSH) not on PATH")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def mint_key(dirpath: Path, name: str = "signer") -> Path:
    """Mint an ephemeral ed25519 keypair; return the PRIVATE key path (never committed)."""
    key = dirpath / name
    subprocess.run(
        [SSH_KEYGEN, "-t", "ed25519", "-f", str(key), "-N", "", "-C", f"ephemeral-{name}"],
        check=True, capture_output=True,
    )
    return key


def sign(private_key: Path, artifact: Path, namespace: str = NAMESPACE) -> Path:
    """Sign `artifact` with `private_key`, returning the detached signature path."""
    subprocess.run(
        [SSH_KEYGEN, "-Y", "sign", "-f", str(private_key), "-n", namespace, str(artifact)],
        check=True, capture_output=True,
    )
    return artifact.with_suffix(artifact.suffix + ".sig")


def allowed_signers_for(pubkey: Path, dest: Path, *, principal: str = TEST_IDENTITY,
                        namespace: str = NAMESPACE) -> Path:
    """Write an allowed_signers trust root that trusts `pubkey` under `principal`."""
    key_field = " ".join(pubkey.read_text(encoding="utf-8").split()[:2])
    dest.write_text(f'{principal} namespaces="{namespace}" {key_field} ephemeral\n',
                    encoding="utf-8")
    return dest


# ---------------------------------------------------------------------------
# Happy path — a valid signature by the pinned key verifies
# ---------------------------------------------------------------------------


def test_valid_committed_signature_verifies():
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG,
        trust_root=COMMITTED_TRUST_ROOT, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.ok
    assert result.code == verify_artifact.OK


def test_cli_returns_zero_on_valid_signature(capsys):
    code = verify_artifact.main([
        "--artifact", str(SAMPLE_ARTIFACT), "--signature", str(SAMPLE_SIG),
        "--trust-root", str(COMMITTED_TRUST_ROOT), "--identity", TEST_IDENTITY,
        "--namespace", NAMESPACE])
    assert code == verify_artifact.OK
    assert "OK:" in capsys.readouterr().out


def test_freshly_minted_signature_verifies(tmp_path):
    key = mint_key(tmp_path)
    artifact = tmp_path / "payload.bin"
    artifact.write_bytes(b"engine payload \x00\x01\x02")
    sig = sign(key, artifact)
    trust = allowed_signers_for(key.with_suffix(".pub"), tmp_path / "allowed_signers")
    result = verify_artifact.verify_artifact(
        artifact, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.ok


# ---------------------------------------------------------------------------
# Fail closed — the refusal surface
# ---------------------------------------------------------------------------


def test_tampered_artifact_is_refused(tmp_path):
    tampered = tmp_path / "sample-artifact.bin"
    tampered.write_bytes(SAMPLE_ARTIFACT.read_bytes() + b"\nInjected trailer\n")
    result = verify_artifact.verify_artifact(
        tampered, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not result.ok
    assert result.code == verify_artifact.REFUSED


def test_corrupted_signature_is_refused(tmp_path):
    good = SAMPLE_SIG.read_text(encoding="utf-8")
    # Flip a chunk of the armored base64 body while keeping the PEM envelope intact.
    lines = good.splitlines()
    assert len(lines) > 2, f"signature fixture too short to tamper meaningfully: {len(lines)} lines"
    lines[1] = "A" * len(lines[1])
    bad_sig = tmp_path / "bad.sig"
    bad_sig.write_text("\n".join(lines) + "\n", encoding="utf-8")
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, bad_sig, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.REFUSED


def test_missing_signature_is_refused_fail_closed(tmp_path):
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, tmp_path / "does-not-exist.sig",
        trust_root=COMMITTED_TRUST_ROOT, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.REFUSED
    assert "refused" in result.detail.lower()


def test_untrusted_key_signature_is_refused(tmp_path):
    """A VALID signature by a key that is NOT in the trust root must fail closed."""
    attacker = mint_key(tmp_path, "attacker")
    artifact = tmp_path / "payload.bin"
    artifact.write_bytes(SAMPLE_ARTIFACT.read_bytes())
    forged = sign(attacker, artifact)
    # Verify the attacker's real signature against the COMMITTED trust root (which pins the
    # test signer, not the attacker) — the key is unknown to the root, so it is refused.
    result = verify_artifact.verify_artifact(
        artifact, forged, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.REFUSED


def test_wrong_namespace_is_refused():
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace="some-other-namespace")
    assert result.code == verify_artifact.REFUSED


def test_wrong_identity_is_refused():
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity="not-a-pinned-principal", namespace=NAMESPACE)
    assert result.code == verify_artifact.REFUSED


# ---------------------------------------------------------------------------
# Configuration failures — still fail closed, surfaced as CONFIG_ERROR
# ---------------------------------------------------------------------------


def test_missing_trust_root_is_config_error(tmp_path):
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=tmp_path / "nope" / "allowed_signers",
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.CONFIG_ERROR
    assert "trust root" in result.detail.lower()


def test_missing_artifact_is_config_error(tmp_path):
    result = verify_artifact.verify_artifact(
        tmp_path / "ghost.bin", SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.CONFIG_ERROR
    assert "artifact not found" in result.detail.lower()


def test_missing_ssh_keygen_fails_closed(monkeypatch):
    monkeypatch.setattr(verify_artifact, "find_ssh_keygen", lambda: None)
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert result.code == verify_artifact.CONFIG_ERROR
    assert "ssh-keygen" in result.detail.lower()


def test_verify_times_out_fails_closed(monkeypatch):
    """A wedged ssh-keygen (TimeoutExpired) must fail closed as CONFIG_ERROR, never pass."""
    def _hang(*args, **kwargs):
        raise subprocess.TimeoutExpired(cmd="ssh-keygen", timeout=kwargs.get("timeout", 0))
    monkeypatch.setattr(verify_artifact.subprocess, "run", _hang)
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not result.ok
    assert result.code == verify_artifact.CONFIG_ERROR
    assert "did not complete" in result.detail.lower()


def test_cli_refused_returns_one_and_writes_stderr(tmp_path, capsys):
    tampered = tmp_path / "sample-artifact.bin"
    tampered.write_bytes(SAMPLE_ARTIFACT.read_bytes() + b"x")
    code = verify_artifact.main([
        "--artifact", str(tampered), "--signature", str(SAMPLE_SIG),
        "--trust-root", str(COMMITTED_TRUST_ROOT), "--identity", TEST_IDENTITY,
        "--namespace", NAMESPACE])
    assert code == verify_artifact.REFUSED
    assert "REFUSED" in capsys.readouterr().err


# ---------------------------------------------------------------------------
# The pinned PRODUCTION trust root is empty in v1 — the default gate refuses everything
# ---------------------------------------------------------------------------


def test_production_trust_root_is_empty_until_first_release():
    """No active (non-comment) signer line: the day-one fail-closed default (R-SEC-009 v1)."""
    active = [ln for ln in verify_artifact.DEFAULT_TRUST_ROOT.read_text(encoding="utf-8").splitlines()
              if ln.strip() and not ln.lstrip().startswith("#")]
    assert active == [], f"production trust root must be empty until first release, found: {active}"


def test_default_production_gate_refuses_a_valid_test_signature():
    """Even a signature valid under the TEST root is refused by the empty PRODUCTION root."""
    result = verify_artifact.verify_artifact(
        SAMPLE_ARTIFACT, SAMPLE_SIG,  # trust_root defaults to the production root
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not result.ok

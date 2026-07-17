"""Tests for tools/versioned_fetch.py — the R-VER-004 versioned-fetch VERIFY SEAM (R-SEC-009 / L-58,
task a08). Extends the verify_artifact fail-closed coverage to the versioned-fetch consumer: a fetched
engine version verifies before execute ONLY when its detached signature is authentic under the pinned
trust root; a tampered / missing / untrusted-key / wrong-namespace signature and a missing trust root
all REFUSE (verify-before-execute, fail closed), and the require_verified_before_execute guard RAISES
on any refusal so an unverified version can never reach an unpack/exec call.

Reuses the committed TEST-ONLY fixtures (tools/tests/fixtures/) + ephemeral ssh-keygen keys where a
test must control the private half. No private key is ever committed.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import load_tool

versioned_fetch = load_tool("versioned_fetch")
verify_artifact = load_tool("verify_artifact")

FIXTURES = Path(__file__).resolve().parent / "fixtures"
COMMITTED_TRUST_ROOT = FIXTURES / "allowed_signers"
SAMPLE_ARTIFACT = FIXTURES / "sample-artifact.bin"
SAMPLE_SIG = FIXTURES / "sample-artifact.bin.sig"

TEST_IDENTITY = "context-engine-test"
NAMESPACE = "context-engine-artifact"

SSH_KEYGEN = shutil.which("ssh-keygen")
pytestmark = pytest.mark.skipif(SSH_KEYGEN is None, reason="ssh-keygen (OpenSSH) not on PATH")


def mint_key(dirpath: Path, name: str = "signer") -> Path:
    key = dirpath / name
    subprocess.run(
        [SSH_KEYGEN, "-t", "ed25519", "-f", str(key), "-N", "", "-C", f"ephemeral-{name}"],
        check=True, capture_output=True,
    )
    return key


def sign(private_key: Path, artifact: Path, namespace: str = NAMESPACE) -> Path:
    subprocess.run(
        [SSH_KEYGEN, "-Y", "sign", "-f", str(private_key), "-n", namespace, str(artifact)],
        check=True, capture_output=True,
    )
    return artifact.with_suffix(artifact.suffix + ".sig")


def allowed_signers_for(pubkey: Path, dest: Path, *, principal: str = TEST_IDENTITY,
                        namespace: str = NAMESPACE) -> Path:
    key_field = " ".join(pubkey.read_text(encoding="utf-8").split()[:2])
    dest.write_text(f'{principal} namespaces="{namespace}" {key_field} ephemeral\n', encoding="utf-8")
    return dest


# --- happy path: a fetched version whose archive verifies is safe to execute ---------------------


def test_verified_fetch_is_safe_to_execute():
    verdict = versioned_fetch.verify_fetched_version(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.ok
    assert verdict.code == verify_artifact.OK


def test_freshly_signed_version_verifies(tmp_path):
    key = mint_key(tmp_path)
    archive = tmp_path / "context-0.1.0.tar"
    archive.write_bytes(b"pretend engine version payload \x00\x01")
    sig = sign(key, archive)
    trust = allowed_signers_for(key.with_suffix(".pub"), tmp_path / "allowed_signers")
    verdict = versioned_fetch.verify_fetched_version(
        archive, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.ok


# --- fail closed: the refusal surface (verify-before-execute) ------------------------------------


def test_tampered_version_archive_is_refused(tmp_path):
    tampered = tmp_path / "sample-artifact.bin"
    tampered.write_bytes(SAMPLE_ARTIFACT.read_bytes() + b"\ntrojaned payload\n")
    verdict = versioned_fetch.verify_fetched_version(
        tampered, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not verdict.ok
    assert verdict.code == verify_artifact.REFUSED


def test_unsigned_version_is_refused(tmp_path):
    verdict = versioned_fetch.verify_fetched_version(
        SAMPLE_ARTIFACT, tmp_path / "missing.sig", trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.code == verify_artifact.REFUSED


def test_untrusted_key_version_is_refused(tmp_path):
    attacker = mint_key(tmp_path, "attacker")
    archive = tmp_path / "context-0.1.0.tar"
    archive.write_bytes(SAMPLE_ARTIFACT.read_bytes())
    forged = sign(attacker, archive)
    verdict = versioned_fetch.verify_fetched_version(
        archive, forged, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.code == verify_artifact.REFUSED


def test_wrong_namespace_version_is_refused():
    verdict = versioned_fetch.verify_fetched_version(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
        identity=TEST_IDENTITY, namespace="some-other-namespace")
    assert verdict.code == verify_artifact.REFUSED


def test_missing_trust_root_is_config_error(tmp_path):
    verdict = versioned_fetch.verify_fetched_version(
        SAMPLE_ARTIFACT, SAMPLE_SIG, trust_root=tmp_path / "nope" / "allowed_signers",
        identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.code == verify_artifact.CONFIG_ERROR


def test_default_gate_uses_the_pinned_production_root(tmp_path):
    """With no explicit trust root, the seam defaults to the PINNED PRODUCTION root — which pins only
    the production key, so a test-key signature is refused (the day-one fail-closed default)."""
    verdict = versioned_fetch.verify_fetched_version(
        SAMPLE_ARTIFACT, SAMPLE_SIG, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not verdict.ok


# --- the execute-boundary guard RAISES on refusal (defense against a forgotten check) ------------


def test_require_verified_before_execute_raises_on_refusal(tmp_path):
    tampered = tmp_path / "sample-artifact.bin"
    tampered.write_bytes(SAMPLE_ARTIFACT.read_bytes() + b"x")
    with pytest.raises(PermissionError):
        versioned_fetch.require_verified_before_execute(
            tampered, SAMPLE_SIG, trust_root=COMMITTED_TRUST_ROOT,
            identity=TEST_IDENTITY, namespace=NAMESPACE)


def test_require_verified_before_execute_passes_on_valid(tmp_path):
    key = mint_key(tmp_path)
    archive = tmp_path / "context-0.1.0.tar"
    archive.write_bytes(b"payload")
    sig = sign(key, archive)
    trust = allowed_signers_for(key.with_suffix(".pub"), tmp_path / "allowed_signers")
    # Returns None (no raise) — the fetcher proceeds to unpack/execute only past this guard.
    assert versioned_fetch.require_verified_before_execute(
        archive, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE) is None


# --- CLI exit taxonomy ---------------------------------------------------------------------------


def test_cli_returns_zero_on_valid(capsys):
    code = versioned_fetch.main([
        "--archive", str(SAMPLE_ARTIFACT), "--signature", str(SAMPLE_SIG),
        "--trust-root", str(COMMITTED_TRUST_ROOT), "--identity", TEST_IDENTITY,
        "--namespace", NAMESPACE])
    assert code == verify_artifact.OK
    assert "OK:" in capsys.readouterr().out


def test_cli_refused_returns_one_and_writes_stderr(tmp_path, capsys):
    tampered = tmp_path / "sample-artifact.bin"
    tampered.write_bytes(SAMPLE_ARTIFACT.read_bytes() + b"x")
    code = versioned_fetch.main([
        "--archive", str(tampered), "--signature", str(SAMPLE_SIG),
        "--trust-root", str(COMMITTED_TRUST_ROOT), "--identity", TEST_IDENTITY,
        "--namespace", NAMESPACE])
    assert code == verify_artifact.REFUSED
    assert "REFUSED" in capsys.readouterr().err

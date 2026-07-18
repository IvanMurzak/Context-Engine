"""a17 — TRUST-TIER RED-TEAM: the fail-closed TAMPER e2e (M8.5 exit clause; issue #283).

This is the M8.5 "a TAMPERED signed artifact is refused through the a08-wired fetch path" clause as a
permanent regression test — NOT a unit test of a pre-made fixture. It drives the REAL versioned-fetch
verify-before-EXECUTE seam (tools/versioned_fetch.py -> tools/verify_artifact.py, the R-VER-004 rule-5
day-one contract) end-to-end:

  1. mint an ephemeral Ed25519 signer,
  2. sign a PLAUSIBLY-NAMED version archive (`context-1.4.2.tar.zst`) as a legitimate release would,
  3. produce the ATTACK: a tampered copy with the SAME valid-looking name AND the SAME byte length
     (one interior byte flipped — not appended, so a naive name/length check is fooled),
  4. assert the seam REFUSES the tampered artifact machine-readably on every surface: the
     verify_fetched_version verdict code, the require_verified_before_execute execute-boundary guard
     (which RAISES), and the CLI exit taxonomy + stderr — while the untampered original verifies.
  5. and assert the tampered artifact is likewise refused by the DEFAULT PINNED PRODUCTION trust root
     (the real day-one posture: only the production release key is trusted).

The attacker's key material is never trusted; the point is that a modified artifact carrying a
valid-looking name can NEVER be used, because authenticity is cryptographic, not nominal.

Reuses ephemeral ssh-keygen keys (no private key is ever committed). Skipped where ssh-keygen is absent.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import load_tool

versioned_fetch = load_tool("versioned_fetch")
verify_artifact = load_tool("verify_artifact")

TEST_IDENTITY = "context-engine-test"
NAMESPACE = "context-engine-artifact"

# A plausibly-named, plausibly-shaped release archive name — the kind of valid-looking name an attacker
# would keep while swapping the bytes underneath.
ARCHIVE_NAME = "context-1.4.2.tar.zst"

SSH_KEYGEN = shutil.which("ssh-keygen")
pytestmark = pytest.mark.skipif(SSH_KEYGEN is None, reason="ssh-keygen (OpenSSH) not on PATH")


def mint_key(dirpath: Path, name: str = "release-signer") -> Path:
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


def _flip_interior_byte(data: bytes) -> bytes:
    """Return `data` with ONE interior byte flipped — same length, same head/tail, so the name AND the
    size are unchanged. The most adversarial tamper shape: only the cryptographic digest changes."""
    assert len(data) >= 3
    mid = len(data) // 2
    return data[:mid] + bytes([data[mid] ^ 0xFF]) + data[mid + 1:]


@pytest.fixture()
def signed_release(tmp_path):
    """A freshly-minted, correctly-signed, plausibly-named release archive + its test trust root."""
    key = mint_key(tmp_path)
    archive = tmp_path / ARCHIVE_NAME
    # Realistic-ish payload bytes (a zstd magic prefix + body) — content is irrelevant to the crypto.
    archive.write_bytes(b"\x28\xb5\x2f\xfd" + b"pretend engine version payload " * 64)
    sig = sign(key, archive)
    trust = allowed_signers_for(key.with_suffix(".pub"), tmp_path / "allowed_signers")
    return archive, sig, trust


# --- positive control: the untampered signed release verifies + is safe to execute ------------------


def test_untampered_release_verifies_and_executes(signed_release):
    archive, sig, trust = signed_release
    verdict = versioned_fetch.verify_fetched_version(
        archive, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert verdict.ok
    assert verdict.code == verify_artifact.OK
    # The execute-boundary guard passes (returns None ⇒ the fetcher may unpack/execute).
    assert versioned_fetch.require_verified_before_execute(
        archive, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE) is None


# --- THE ATTACK: a tampered artifact with a valid-looking name + unchanged length is refused --------


def test_tampered_release_same_name_same_length_is_refused(signed_release, tmp_path):
    archive, sig, trust = signed_release
    original = archive.read_bytes()

    tampered = tmp_path / ARCHIVE_NAME  # the SAME valid-looking name
    tampered.write_bytes(_flip_interior_byte(original))

    # The tamper is invisible to a nominal check: identical name, identical byte length.
    assert tampered.name == archive.name
    assert tampered.stat().st_size == archive.stat().st_size
    assert tampered.read_bytes() != original  # but the bytes differ

    # 1) The verify-before-execute verdict REFUSES, machine-readably (a stable non-zero code).
    verdict = versioned_fetch.verify_fetched_version(
        tampered, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert not verdict.ok
    assert verdict.code == verify_artifact.REFUSED

    # 2) The execute-boundary guard RAISES — an unverified version can never reach an unpack/exec call.
    with pytest.raises(PermissionError) as excinfo:
        versioned_fetch.require_verified_before_execute(
            tampered, sig, trust_root=trust, identity=TEST_IDENTITY, namespace=NAMESPACE)
    assert "verify-before-execute" in str(excinfo.value)


def test_tampered_release_cli_exit_taxonomy_is_machine_readable(signed_release, tmp_path, capsys):
    archive, sig, trust = signed_release
    tampered = tmp_path / ARCHIVE_NAME
    tampered.write_bytes(_flip_interior_byte(archive.read_bytes()))

    # The CLI surface (what a fetcher/release step shells out to) returns the REFUSED exit code and
    # writes a machine-greppable fail-closed banner to stderr — never a silent zero.
    code = versioned_fetch.main([
        "--archive", str(tampered), "--signature", str(sig),
        "--trust-root", str(trust), "--identity", TEST_IDENTITY, "--namespace", NAMESPACE])
    assert code == verify_artifact.REFUSED
    err = capsys.readouterr().err
    assert "REFUSED (fail closed)" in err


def test_tampered_release_refused_by_default_production_root(signed_release, tmp_path):
    """With NO explicit trust root the seam defaults to the PINNED PRODUCTION allowed_signers, which
    pins only the production release key — so a tampered (or even an untampered test-key) artifact is
    refused. The real day-one fail-closed posture, exercised through the default fetch path."""
    archive, _sig, _trust = signed_release
    tampered = tmp_path / ARCHIVE_NAME
    tampered.write_bytes(_flip_interior_byte(archive.read_bytes()))
    # Re-sign the tampered bytes with the attacker key so the signature MATCHES the tampered artifact —
    # the strongest attack: internally-consistent, but by an UNTRUSTED key under the production root.
    attacker = mint_key(tmp_path, "attacker")
    forged = sign(attacker, tampered)

    verdict = versioned_fetch.verify_fetched_version(
        tampered, forged, identity=TEST_IDENTITY, namespace=NAMESPACE)  # default = production root
    assert not verdict.ok

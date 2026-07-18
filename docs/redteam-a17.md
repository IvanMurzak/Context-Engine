# Trust-tier red-team (a17) — probe map, findings, and honest v1 boundaries

M8.5 wedge-hardening task **a17** (issue #283). This records the ADVERSARIAL validation of the v1
trust posture — the red-team suite *probes* the already-landed enforcement primitives as an attacker
would; it rebuilds nothing. Design refs: R-SEC-001/002/006/007/008/009/010/011(a), L-49, ROADMAP
§1-M8.5. It depends on a08 (trust-root pin + fetch-verify), landed in #266.

The v1 trust posture held under this probing — **no bypass was found**. What follows is the map of
what is now permanently regression-tested and the residual risks that are honest v1 boundaries rather
than defects.

## Probe map (each probe is a permanent regression test)

| Probe | Where | Asserts |
|---|---|---|
| Dispatcher scope enforcement — every door | `src/tests/integration/test_a17_trust_redteam.cpp` (`redteam-trust-tier`) | A read/query token is refused every out-of-scope verb (file-write / build-install / session families) over BOTH the in-process `dispatch()` and the JSON-RPC `handle()` wire (the door the CLI daemon client + the MCP adapter both funnel through), with the permission-class `scope.denied` code (exit-class 6). The scope check PRECEDES verb resolution (no surface enumeration for the under-scoped), so adapter-level tool filtering cannot be bypassed via direct RPC (R-SEC-007). |
| Launch-ceiling clamp (no escalation over the wire) | same file | A client that requests every scope in the attach handshake against a read/query operator ceiling is clamped to read/query — it cannot escalate past `--launch-scopes` (R-SEC-007). |
| Path-jail traversal + narrowed-importer-jail | same file | Deep/mixed `..` traversal, prefix-adjacency, and absolute/drive escapes are refused by `is_inside_jail`; a `declared_read_paths` entry that net-escapes the jail cannot widen the read set, and a write target using `..` (or a sibling prefix) to climb out of the output key is refused (R-SEC-008, the issue-#72 narrowed importer jail). |
| Scrubbed child env is an ALLOWLIST | same file | `scrubbed_environment()` returns ONLY a fixed locale allowlist — every returned key is asserted in-allowlist, so a future ambient-inheritance regression (a secret/token/PATH passthrough) fails structurally, not just on a substring denylist; `allow_network` defaults false (R-SEC-010). |
| Verify-before-use fails closed | same file | The C++ `verify_signature` (the mirror of `tools/verify_artifact.py`) never verifies a non-production artifact against the pinned PRODUCTION trust root, and refuses on every abnormal path (unsigned ⇒ Refused; missing/unreadable root or artifact ⇒ ConfigError). Nothing is ever "verified with a warning" (R-SEC-009 / a08). |
| JS host exposes only injected bindings | `src/runtime/js/tests/test_ambient_redteam.cpp` (`redteam-js-ambient`) | From inside a guest, `require`/`module`/`process`/`fetch`/`XMLHttpRequest`/`WebSocket`/`Deno`/`Bun`/`globalThis.fs` are all `undefined` — no ambient fs/net/process — while an explicitly bound host function IS reachable (injection is the sole door in). V8-gated (R-SEC-001/002 / L-49). |
| Tamper e2e (the M8.5 exit clause) | `tools/tests/test_a17_tamper_e2e.py` | A freshly-signed, plausibly-named release archive (`context-1.4.2.tar.zst`) is tampered with a valid-looking name AND unchanged byte length (one interior byte flipped) and is REFUSED machine-readably through the real `versioned_fetch` verify-before-execute fetch path — the verdict code, the raising execute-boundary guard, and the CLI exit taxonomy + stderr — and also by the default pinned production root (R-VER-004 rule 5 / R-SEC-009). |

### Existing coverage this rides (not duplicated)

The a17 suite deliberately does not re-implement probes the landed primitives already ship as
permanent regression tests; those remain the authoritative gates for their seam:

- **WASM cannot reach an ungranted import** — `src/runtime/wasm/tests/test_wasm_runner.cpp`
  (`test_import_declaring_module_fails_to_instantiate`): the sandbox is STRUCTURALLY zero-import
  (instantiation passes an empty import list), so a module declaring ANY import never runs. CI-gated
  by the `wasm-runner` job (the real wasmtime backend; the local GCC gate builds the honest stub).
- **Physical path-jail TOCTOU (symlink/junction escape)** — `src/editor/filesync/tests/test_native_file_store.cpp`:
  root-escaping links are refused on every write-side op (write/rename/remove) via resolve-then-verify-by-fd
  (`openat`+`O_NOFOLLOW` on POSIX / `FILE_FLAG_OPEN_REPARSE_POINT`+re-realpath on Windows). The a17 suite
  covers the LOGICAL jail (lexical `is_inside_jail`) adversarially; the physical TOCTOU layer stays owned there.
- **Importer per-OS syscall lockdown** — `src/editor/import/tests/test_sandbox.cpp`: the real seccomp-bpf
  (Linux) / Seatbelt (macOS) permit-vs-deny gate (denied `openat`/`socket`/exec-map; permitted rw-map/pipe-write).

## Honest v1 boundaries (residual risks — by design, not defects)

1. **One TS trust domain (R-SEC-001).** All first-party TypeScript gameplay runs in ONE V8 isolate;
   v1 has NO per-package sub-sandbox WITHIN the TS tier. The trust boundary that matters is
   sandboxed-tier (WASM/TS) vs the explicitly-reviewed native (C++) tier with a loud consent gate at
   install — AI-installed packages default to the sandbox tier, so an agent can never silently
   introduce native code (L-49). Intra-TS isolation between first-party packages is not a v1 claim.
   The `redteam-js-ambient` probe validates the sandbox↔host boundary (no ambient fs/net/process),
   NOT an intra-TS package boundary.

2. **Windows importer syscall lockdown is STAGED.** `os_sandbox_support()` reports `enforced=false` on
   Windows — the AppContainer / restricted Job Object primitive is a tracked follow-up (R-SEC-006).
   On Windows the POLICY jail (the input-bytes-only read set, the output-key write set, the scrubbed
   env, no ambient network) still holds at the reference-runner layer and is asserted here; the
   OS-syscall STRUCTURAL enforcement is live on Linux + macOS only. This is honestly surfaced, never
   silently assumed.

3. **The `redteam-js-ambient` probe is V8-gated.** Its ambient-absence assertions run wherever the
   rusty_v8 prebuilt links (the V8 CI legs / a Clang/MSVC host with the prebuilt); on a toolchain that
   only builds the stub (the local Strawberry-GCC dev gate) the host cannot instantiate, so no guest
   JS runs at all — the probe asserts that honest stub state and stays green. The real ambient
   validation is therefore a CI-leg guarantee, matching the whole `js-test_*` family's carve-out.

4. **The logical path jail is lexical.** `is_inside_jail` / `normalize_path` resolve `..` and separators
   purely lexically over the injectable FileStore seam; they do NOT resolve symlinks. The fully
   TOCTOU-safe physical jail (resolve-then-verify-by-fd, refuse root-escaping reparse points) is the
   native FileStore's responsibility and is tested there (see above). Callers apply the logical jail
   BEFORE the physical layer adds fd/handle verification beneath it — defense in depth, not a single check.

5. **Trust-root day-one posture.** The pinned production `tools/trust-root/allowed_signers` pins exactly
   ONE Ed25519 publisher key (custody model B — the env-protected `RELEASE_SIGNING_KEY` GitHub secret;
   only public key material is committed). Any artifact not signed by that key under principal
   `context-engine-release` and namespace `context-engine-artifact` is REFUSED fail-closed. The
   resolver/fetcher/launcher machinery of R-VER-004 is a second-release deliverable; the verify-before-
   execute SEAM (`versioned_fetch.py`) is the day-one contract and is what the tamper e2e drives.

## Findings

No trust bypass was discovered. Every probe confirmed the enforcement fails closed. Should a future
change regress any of the above, the corresponding probe turns its gate red (all probes are blocking
per-PR ctests / pytest — `redteam-trust-tier` + `redteam-js-ambient` on all three build legs, the
tamper e2e in the `python-tests` job).

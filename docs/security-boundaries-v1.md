# v1 trust boundaries — the red-team map (M8.5 Wave-1 a17)

This engineering doc records how Context Engine's **v1 trust posture** was ADVERSARIALLY validated
(red-teamed) at M8.5, what held, what was hardened, and which boundaries remain honestly incomplete in
v1. It is a companion to the design authority (`R-SEC-*` in the owner's `REQUIREMENTS.md`, locks
`L-49`/`L-58`/`L-62`) — this file only records the repository's validation of them. The permanent
regression tests that back every claim below live in `src/tests/integration/test_redteam_boundaries.cpp`
(`security-redteam-boundaries`) and `src/tests/integration/test_redteam_tamper_e2e.cpp`
(`security-redteam-tamper-e2e`), plus the per-seam unit suites they cross-check.

The threat model is the design's: **untrusted same-user code** — AI agents, npm/vcpkg packages, imported
assets, editor extensions — all run **as the OS user**, so the defence is least-privilege +
no-ambient-secrets + no-default-egress + verify-before-use, not an OS user boundary.

## The doors, and what the red-team found

| Door | Control | Enforcement point | Red-team verdict |
|---|---|---|---|
| Dispatcher scope gate | R-SEC-007 | `required_scope_for` / `authorize` in the RPC **dispatcher** (`src/editor/bridge/`) | **Held** for every wired verb; **hardened** — the method→scope table was completed (see below). |
| Path jail | R-SEC-008 | `is_inside_jail` / `normalize_path` (`src/editor/filesync/`) | **Held** — traversal / absolute-reroot / drive-letter / backslash / prefix-not-parent all refused structurally. |
| Importer read/write jail | R-SEC-006 / R-SEC-008 | `read_permitted` / `write_permitted` (`src/editor/import/`) | **Held** — input-bytes ∪ declared-paths for reads, own output key for writes; the declared-read hatch can never widen past the jail. |
| Scrubbed child env | R-SEC-010 | `scrubbed_environment` (`src/editor/import/`) | **Held** — an exact locale allowlist; no ambient secret/token crosses to a child. |
| Sandbox-primitive staging | R-SEC-006 | `os_sandbox_support` / `apply_importer_sandbox` | **Honest** — never claims a lockdown that is not there (see residual boundaries). |
| Verify-before-use | R-SEC-009 / L-58 (a08) | `verify_signature` (`src/common/`) + the `context build` fetch path | **Held & hardened** — a same-length, valid-named tamper is refused machine-readably; the envelope was polluted and is now clean (see below). |
| JS host capability surface | R-SEC-001 / R-SEC-002 | the V8 host (`src/runtime/js/`) | **Held** on the build legs (only injected bindings reachable); **fails closed** on the stub. |
| WASM sandbox | R-SEC-002 | the deterministic WasmRunner (`src/runtime/wasm/`) | **Residual — verified by a dedicated CI job**, not the default build (see below). |

## What was HARDENED in this lane (the discovered gaps)

1. **R-SEC-007 method→scope table completeness (defense-in-depth).** `required_scope_for` gated only the
   currently-WIRED mutating verbs. Reserved-but-mutating verbs — `install`, `migrate`, `merge-file`,
   `resolve-conflict`, `re-key`, `asset.move`, `asset.rename`, `session.new`/`seed`/`inject`/`record`,
   `replay`, `ui.send` — fell through to the read/query baseline. No LIVE bypass existed (their bridge
   backings return `contract.unimplemented` today, and the daemon backend serves only
   `edit`/`edit-batch`/`snapshot`/`reconcile`/`query`/`resource.read`/`shutdown`, every mutating one of
   which was already gated). But wiring any of their backings later would have silently exposed them to a
   read-only token. **Fixed:** each is now gated by its semantic class, so a read/query token is denied
   fail-closed the day a backing lands. `test_redteam_boundaries.cpp`'s scope-table sweep is the
   regression test — a NEW registry verb with no classification there fails the build.

2. **Verify-before-use stdout pollution (machine-readability defect).** `ssh-keygen -Y verify` prints its
   `Good "<ns>" signature ...` confirmation to **stdout**; because the verifier runs through the inherited
   stdout of the caller, a `context build --toolchain-sig` run leaked that line onto its own
   **machine-readable R-CLI-008 envelope** on stdout — so a consumer parsing the JSON broke on the SUCCESS
   path. The verdict always travelled via the exit code, so this was never a security bypass, but it
   defeated "refused **machine-readably**". **Fixed:** `verify_signature` routes ssh-keygen's chatter to
   stderr (`1>&2`), keeping the caller's stdout a clean envelope. `security-redteam-tamper-e2e` (which
   parses the CLI's stdout JSON) is the regression test.

## The tamper exit clause (M8.5)

`security-redteam-tamper-e2e` mints an ephemeral Ed25519 key (ssh-keygen — on every CI runner), signs a
valid-looking fetched "version archive", and drives the **real `context` binary**'s a08
verify-before-execute fetch path end-to-end:

- an **authentic** archive verifies through the CLI and the build is green (non-vacuous control);
- the same archive with **one byte flipped** — **identical length, identical valid name** — is refused
  with a machine-readable `build.toolchain_fetch_failed`, before it is ever used;
- the sibling export-template surface (`--runtime`) is refused with `build.template_unverified`;
- a namespace replay is refused; the pinned PRODUCTION root refuses the ephemeral key (no downgrade
  surface). If ssh-keygen is ever absent, the unconditional fail-closed refusal still runs.

## Residual v1 boundaries (honest, by design)

These are NOT gaps to fix in v1 — they are the design's staged posture. They are called out so nobody
assumes a stronger boundary than exists.

- **One TS trust domain (R-SEC-001).** All TypeScript runs in ONE shared V8 domain in v1 — there is **no
  package-vs-package isolation** between TS packages. v1 TS isolation is a *constrained host ABI* (no
  ambient `fs`/`net`/`process`; only engine-injected, capability-gated bindings), validated by Door F.
  Per-package isolates are post-v1.
- **Windows importer syscall sandbox is staged (R-SEC-006).** The per-OS lockdown is enforced on **Linux
  (seccomp-bpf)** and **macOS (Seatbelt)**; **Windows AppContainer / restricted Job Object is a tracked
  de-risk item** — `os_sandbox_support()` reports `enforced=false` + a note there, and the runner falls
  back to the portable in-process slice (path jail + scrubbed env + no-ambient-network + input-bytes-only).
  Door E asserts this honesty (never a false "locked down").
- **WASM zero-import sandbox is a CI-only dependency path.** The deterministic WasmRunner links the
  MSVC/Clang-ABI wasmtime prebuilt, so the default 3-OS `build` job and the local dev gate build no WASM
  target at all (the subdir early-returns). Its zero-import structural guarantee (a module declaring ANY
  import fails to instantiate — no WASI, no clock, no IO) and its fail-closed migration are gated by the
  dedicated **`wasm-runner`** CI job (`src/runtime/wasm/tests/`, incl. `test_wasm_fail_closed.cpp`), not
  by the red-team suite. Documented here rather than duplicated.
- **`reconcile` is read/query.** It force-re-ingests external file changes and re-derives — the manual
  form of what the watcher does automatically. It writes no authored file, so it stays on the read
  baseline. `profile.gc` (a profiling probe) and `debug.attach` (R-OBS-005 CDP inspector) are likewise
  left on the read baseline in v1; whether an interactive debugger attach should require `session-control`
  is a design question deferred to the owner (it is inert on the bridge today).
- **Native tier is consent-gated, not sandboxed (R-SEC-001/L-49).** Native (C++) packages are honestly
  not sandboxed — reviewed + consent-gated. v1 ships **no third-party native packages**, so the polished
  consent UX and the from-source build-env jail are `SHOULD` in v1 (R-SEC-005). Not in this suite's scope.
- **Async consent park-and-resume is `SHOULD` in v1 (R-SEC-011(b)).** An out-of-scope call already fails
  machine-readably with the reserved `scope.denied`/`consent_required`-class code; the full out-of-band
  approve-and-resume machinery is post-v1.

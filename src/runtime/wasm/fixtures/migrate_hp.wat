;; migrate_hp — the committed sandboxed-WASM migration fixture (issue #71 PR4).
;;
;; Built to the frozen zero-import guest ABI (src/editor/migrate/migration_runner.h, protocolMajor 1):
;; exports "memory" + ctx_alloc (bump allocator) + ctx_migrate. Zero imports (no WASI/clock/IO/
;; randomness) — the sandbox is structural. ctx_migrate performs the phys:body v1 -> v2 migration by
;; emitting the fixed canonical payload {"hp":2}; the host re-parses + re-canonicalises + re-checks
;; every structural invariant around it (budget, id immutability). Regenerate the committed .wasm with
;;   python3 tools/gen_wasm_fixtures.py
;; and pin its reproducibility with tools/tests/test_gen_wasm_fixtures.py.
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "{\22hp\22:2}")
  (global $next (mut i32) (i32.const 8192))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 8))
    (i32.const 0)))

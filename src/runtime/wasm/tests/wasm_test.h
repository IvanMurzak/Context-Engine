// Tiny zero-dependency test harness for the wasm-runner ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework), plus the WAT guest corpus the
// REAL-backend tests assemble at runtime via wasmtime_wat2wasm (the C-API ships the .wat
// frontend). Guests here are tiny, purpose-built probes of the frozen PR2 guest ABI
// (ctx_alloc / ctx_migrate / optional ctx_map_path over exported linear memory, ZERO imports) —
// the committed .wasm fixture corpus is PR 4, so nothing binary is checked in here.

#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

namespace wasmtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace wasmtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            wasmtest::fail(__FILE__, __LINE__, #cond);                                             \
    } while (false)

#define WASM_TEST_MAIN_END() return wasmtest::g_failures == 0 ? 0 : 1

#if defined(CONTEXT_WASM_HAS_RUNTIME)

#include "context/runtime/wasm/wasm_runner.h"

#include <wasmtime.h>

namespace wasmtest
{

// Assemble a WAT source into wasm bytes via the C-API frontend. CHECK-fails (and returns empty)
// on a syntax error, so a broken guest fails loudly at the first use.
inline std::string compile_wat(std::string_view wat)
{
    wasm_byte_vec_t out{};
    wasmtime_error_t* error = wasmtime_wat2wasm(wat.data(), wat.size(), &out);
    if (error != nullptr)
    {
        wasm_byte_vec_t msg{};
        wasmtime_error_message(error, &msg);
        std::fprintf(stderr, "wat2wasm failed: %.*s\n", static_cast<int>(msg.size), msg.data);
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        ++g_failures;
        return {};
    }
    std::string bytes(out.data, out.size);
    wasm_byte_vec_delete(&out);
    return bytes;
}

// A ModuleResolver over an in-memory ref -> wasm-bytes table (tests own module distribution — the
// runner performs no IO).
class ModuleTable
{
public:
    void add(std::string ref, std::string_view wat) { modules_[std::move(ref)] = compile_wat(wat); }

    [[nodiscard]] context::runtime::wasm::ModuleResolver resolver() const
    {
        return [this](std::string_view ref, std::string& bytes, std::string& problem) {
            const auto it = modules_.find(std::string(ref));
            if (it == modules_.end())
            {
                problem = "unknown test module";
                return false;
            }
            bytes = it->second;
            return true;
        };
    }

private:
    std::unordered_map<std::string, std::string> modules_;
};

// ---- the WAT guest corpus ------------------------------------------------------------------
// Shared shape: exported "memory" + a bump-allocator ctx_alloc starting at a nonzero offset
// (0 is the ABI's allocation-failure sentinel). Payload staging areas sit above the data
// segments; tests keep inputs far below the 4-page (256 KiB) memories.

// Identity migrate: output = the input region, byte for byte.
inline constexpr const char* kWatIdentity = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.store (local.get $outpp) (local.get $in))
    (i32.store (local.get $outlp) (local.get $inlen))
    (i32.const 0)))
)WAT";

// Fixed-output migrate: ignores the input, returns the canonical JSON {"hp":2} — the "real
// transform" stand-in for end-to-end document migration (v1 {"hp":1} -> v2 {"hp":2}).
inline constexpr const char* kWatFixedOutput = R"WAT(
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
)WAT";

// Fresh-instance probe: a mutable global counts ctx_migrate calls WITHIN one instance and the
// output {"n":D} encodes the count. A fresh Store+instance per step (the frozen lifecycle
// contract) makes EVERY run_step return {"n":1}; instance reuse would leak {"n":2}.
inline constexpr const char* kWatCounter = R"WAT(
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "{\22n\22:0}")
  (global $next (mut i32) (i32.const 8192))
  (global $n (mut i32) (i32.const 0))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (global.set $n (i32.add (global.get $n) (i32.const 1)))
    (i32.store8 (i32.const 4101) (i32.add (i32.const 48) (global.get $n)))
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 7))
    (i32.const 0)))
)WAT";

// Bounded spin then identity: ~1e6 loop iterations (a few million fuel units). Far above the
// K x 1 fuel of a max_nodes=1 budget (traps out-of-fuel), far below the default 65536-node
// budget's grant (completes) — the budget->fuel scaling probe.
inline constexpr const char* kWatSpin = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (local $i i32)
    (local.set $i (i32.const 1000000))
    (block $done
      (loop $l
        (local.set $i (i32.sub (local.get $i) (i32.const 1)))
        (br_if $done (i32.eqz (local.get $i)))
        (br $l)))
    (i32.store (local.get $outpp) (local.get $in))
    (i32.store (local.get $outlp) (local.get $inlen))
    (i32.const 0)))
)WAT";

// Endless loop: without a deterministic fuel limit this would hang derivation forever — the L-37
// hazard the budget exists for.
inline constexpr const char* kWatInfinite = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (loop $l (br $l))
    (i32.const 0)))
)WAT";

// Non-fuel wasm trap (unreachable) — must map to a plain step failure, NOT budget_exceeded.
inline constexpr const char* kWatTrap = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    unreachable))
)WAT";

// Guest-reported failure: ANY non-zero ctx_migrate return (frozen ABI).
inline constexpr const char* kWatGuestError = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.const 7)))
)WAT";

// Missing ctx_migrate (memory + ctx_alloc only).
inline constexpr const char* kWatNoMigrate = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr)))
)WAT";

// Declares an import — must FAIL TO INSTANTIATE (the zero-import sandbox is structural).
inline constexpr const char* kWatImports = R"WAT(
(module
  (import "env" "f" (func $f))
  (memory (export "memory") 1)
  (func (export "ctx_alloc") (param $size i32) (result i32) (i32.const 8))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (call $f)
    (i32.const 0)))
)WAT";

// ctx_alloc reports allocation failure (returns 0).
inline constexpr const char* kWatAllocFails = R"WAT(
(module
  (memory (export "memory") 1)
  (func (export "ctx_alloc") (param $size i32) (result i32) (i32.const 0))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.const 0)))
)WAT";

// Stores an out-of-bounds output region — the host's bounds check must refuse it.
inline constexpr const char* kWatOobOutput = R"WAT(
(module
  (memory (export "memory") 1)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.store (local.get $outpp) (i32.const 60000))
    (i32.store (local.get $outlp) (i32.const 100000))
    (i32.const 0)))
)WAT";

// Arithmetic-NaN probe: computes 0.0/0.0 from RUNTIME values and returns the raw f64 bit pattern
// as its 8 output bytes. With Cranelift NaN canonicalization ON the pattern is the canonical
// positive quiet NaN (0x7ff8000000000000) on EVERY platform; x86 hardware would otherwise
// produce the sign-negative 0xfff8000000000000 — the cross-platform divergence the deterministic
// config exists to kill.
inline constexpr const char* kWatNan = R"WAT(
(module
  (memory (export "memory") 4)
  (global $next (mut i32) (i32.const 8))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (local $z f64)
    (local.set $z (f64.convert_i32_s (i32.sub (local.get $inlen) (local.get $inlen))))
    (i64.store (i32.const 4096) (i64.reinterpret_f64 (f64.div (local.get $z) (local.get $z))))
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 8))
    (i32.const 0)))
)WAT";

// memory.grow probe: tries to grow by the WAT-baked page count and outputs "1" when the grow was
// DENIED (returned -1), "0" when it succeeded. The fixed store limit must deny a beyond-limit
// grow (2048 pages = 128 MiB > kWasmMemoryLimitBytes) and allow a small one.
inline constexpr const char* kWatGrowBeyondLimit = R"WAT(
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "0")
  (global $next (mut i32) (i32.const 8192))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (if (i32.eq (memory.grow (i32.const 2048)) (i32.const -1))
      (then (i32.store8 (i32.const 4096) (i32.const 49))))
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 1))
    (i32.const 0)))
)WAT";

inline constexpr const char* kWatGrowWithinLimit = R"WAT(
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "0")
  (global $next (mut i32) (i32.const 8192))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (if (i32.eq (memory.grow (i32.const 1)) (i32.const -1))
      (then (i32.store8 (i32.const 4096) (i32.const 49))))
    (i32.store (local.get $outpp) (i32.const 4096))
    (i32.store (local.get $outlp) (i32.const 1))
    (i32.const 0)))
)WAT";

// Optional ctx_map_path: "/old" -> "/new" (mapped), "/gone" -> return 1 (unmapped/orphan),
// anything else echoes (identity). ctx_migrate is the identity transform.
inline constexpr const char* kWatMapPath = R"WAT(
(module
  (memory (export "memory") 4)
  (data (i32.const 4096) "/new")
  (global $next (mut i32) (i32.const 8192))
  (func (export "ctx_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    (local.set $ptr (global.get $next))
    (global.set $next (i32.add (global.get $next) (local.get $size)))
    (local.get $ptr))
  (func (export "ctx_migrate") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (i32.store (local.get $outpp) (local.get $in))
    (i32.store (local.get $outlp) (local.get $inlen))
    (i32.const 0))
  (func (export "ctx_map_path") (param $in i32) (param $inlen i32) (param $outpp i32) (param $outlp i32) (result i32)
    (if (i32.and (i32.eq (local.get $inlen) (i32.const 4))
                 (i32.eq (i32.load (local.get $in)) (i32.const 0x646c6f2f)))
      (then
        (i32.store (local.get $outpp) (i32.const 4096))
        (i32.store (local.get $outlp) (i32.const 4))
        (return (i32.const 0))))
    (if (i32.and (i32.eq (local.get $inlen) (i32.const 5))
                 (i32.and (i32.eq (i32.load (local.get $in)) (i32.const 0x6e6f672f))
                          (i32.eq (i32.load8_u (i32.add (local.get $in) (i32.const 4)))
                                  (i32.const 101))))
      (then (return (i32.const 1))))
    (i32.store (local.get $outpp) (local.get $in))
    (i32.store (local.get $outlp) (local.get $inlen))
    (i32.const 0)))
)WAT";

} // namespace wasmtest

#endif // CONTEXT_WASM_HAS_RUNTIME

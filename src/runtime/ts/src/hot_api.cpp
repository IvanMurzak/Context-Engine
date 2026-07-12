// The engine-provided pooled / no-allocation hot API JS source (see hot_api.h). The module is a
// single idempotent IIFE installing `globalThis.ctx`; every function is monomorphic, closure-free
// per call, and allocation-free at steady state (pools fall back to allocating on exhaustion,
// counting the miss, so correctness never depends on pool sizing).

#include "context/runtime/ts/hot_api.h"

namespace context::runtime::ts
{

std::string_view hot_api_js()
{
    // Fixed-point exactness notes (mirroring packages/simmath fixed.h, Q16.16, |raw| <= 2^31):
    //  * mul: C++ is (a*b) >> 16 — an arithmetic shift, i.e. floor(a*b / 2^16). The int64 product
    //    can reach 2^62 (NOT double-exact), so JS splits a = aHi*2^16 + aLo (aHi = floor(a/2^16),
    //    0 <= aLo < 2^16): floor(a*b/2^16) == aHi*b + floor(aLo*b/2^16), where |aHi*b| <= 2^46 and
    //    aLo*b <= 2^47 are exact doubles and /2^16 is an exact binary scale — bit-exact vs C++.
    //  * div / from_ratio: C++ is (a << 16) / b — C++ integer division TRUNCATES TOWARD ZERO. The
    //    scaled numerator |a*2^16| <= 2^47 is exact, but the double quotient can misround at a
    //    floor boundary, so tdiv computes trunc(n/b) then corrects with the EXACT remainder
    //    (n - q*b, always double-exact here) — at most two correction steps. b === 0 (C++ UB)
    //    deterministically returns 0.
    static constexpr std::string_view kSource = R"JS(
(function () {
    'use strict';
    if (globalThis.ctx && globalThis.ctx.__hotApi === 1) { return; }
    var ctx = { __hotApi: 1 };

    // --- ctx.pool: preallocated vector registers (steady-state zero-alloc) ---------------------
    function makePool(width, n) {
        var regs = new Array(n);
        for (var i = 0; i < n; i++) { regs[i] = new Float64Array(width); }
        return { regs: regs, next: 0, width: width };
    }
    var P2 = makePool(2, 32);
    var P3 = makePool(3, 32);
    var P4 = makePool(4, 32);
    function takeReg(p, pool) {
        if (p.next < p.regs.length) { return p.regs[p.next++]; }
        pool.misses++;
        return new Float64Array(p.width);
    }
    var pool = {
        misses: 0,
        v2: function () { return takeReg(P2, pool); },
        v3: function () { return takeReg(P3, pool); },
        v4: function () { return takeReg(P4, pool); },
        reset: function () { P2.next = 0; P3.next = 0; P4.next = 0; }
    };
    ctx.pool = pool;

    // --- ctx.math: in-place vector math (write `out`, return it; no temporaries) ---------------
    var math = {
        v2set: function (out, x, y) { out[0] = x; out[1] = y; return out; },
        v2add: function (out, a, b) { out[0] = a[0] + b[0]; out[1] = a[1] + b[1]; return out; },
        v2sub: function (out, a, b) { out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; return out; },
        v2scale: function (out, a, s) { out[0] = a[0] * s; out[1] = a[1] * s; return out; },
        v2dot: function (a, b) { return a[0] * b[0] + a[1] * b[1]; },
        v2len2: function (a) { return a[0] * a[0] + a[1] * a[1]; },
        v3set: function (out, x, y, z) { out[0] = x; out[1] = y; out[2] = z; return out; },
        v3add: function (out, a, b) {
            out[0] = a[0] + b[0]; out[1] = a[1] + b[1]; out[2] = a[2] + b[2]; return out;
        },
        v3sub: function (out, a, b) {
            out[0] = a[0] - b[0]; out[1] = a[1] - b[1]; out[2] = a[2] - b[2]; return out;
        },
        v3scale: function (out, a, s) {
            out[0] = a[0] * s; out[1] = a[1] * s; out[2] = a[2] * s; return out;
        },
        v3dot: function (a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; },
        v3len2: function (a) { return a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; },
        v3len: function (a) { return Math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); },
        // Aliasing-safe cross (out may be a or b): compute into locals (numbers), then store.
        v3cross: function (out, a, b) {
            var x = a[1] * b[2] - a[2] * b[1];
            var y = a[2] * b[0] - a[0] * b[2];
            var z = a[0] * b[1] - a[1] * b[0];
            out[0] = x; out[1] = y; out[2] = z;
            return out;
        },
        lerp: function (a, b, t) { return a + (b - a) * t; },
        clamp: function (v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); }
    };
    ctx.math = math;

    // --- ctx.fixed: Q16.16 mirror of packages/simmath fixed.h (raw ints in Numbers) ------------
    var FX_ONE = 65536;
    // Exact truncate-toward-zero division of two safe integers (C++ `/` semantics; b===0 -> 0).
    function tdiv(n, b) {
        if (b === 0) { return 0; }
        var q = Math.trunc(n / b);
        var r = n - q * b;
        while (r !== 0 && ((r > 0) !== (n > 0) || Math.abs(r) >= Math.abs(b))) {
            if ((r > 0) === (b > 0)) { q += 1; r -= b; } else { q -= 1; r += b; }
        }
        return q;
    }
    var fixed = {
        ONE: FX_ONE,
        fromInt: function (v) { return v * FX_ONE; },
        fromRatio: function (num, den) { return tdiv(num * FX_ONE, den); },
        // floor(a*b / 2^16) via the exact hi/lo split (see hot_api.cpp's exactness notes).
        mul: function (a, b) {
            var aHi = Math.floor(a / FX_ONE);
            var aLo = a - aHi * FX_ONE;
            return aHi * b + Math.floor((aLo * b) / FX_ONE);
        },
        div: function (a, b) { return tdiv(a * FX_ONE, b); },
        mulInt: function (a, n) { return a * n; },
        divInt: function (a, n) { return tdiv(a, n); },
        floorInt: function (a) { return Math.floor(a / FX_ONE); },
        truncInt: function (a) { return Math.trunc(a / FX_ONE); },
        abs: function (a) { return a < 0 ? -a : a; },
        min: function (a, b) { return a < b ? a : b; },
        max: function (a, b) { return a > b ? a : b; },
        clamp: function (v, lo, hi) { return v < lo ? lo : (v > hi ? hi : v); },
        sign: function (a) { return a > 0 ? 1 : (a < 0 ? -1 : 0); }
    };
    ctx.fixed = fixed;

    // --- ctx.query: pooled strided row-cursors over R-LANG-009 zero-copy views -----------------
    function Cursor() { this.a = null; this.stride = 1; this.count = 0; }
    Cursor.prototype.get = function (row, lane) { return this.a[row * this.stride + lane]; };
    Cursor.prototype.set = function (row, lane, v) { this.a[row * this.stride + lane] = v; };
    var CURSOR_CAP = 16; // mirrors the host's kMaxSystemViews view cap
    var cursors = new Array(CURSOR_CAP);
    for (var ci = 0; ci < CURSOR_CAP; ci++) { cursors[ci] = new Cursor(); }
    var query = {
        misses: 0,
        next: 0,
        open: function (view, stride) {
            var c;
            if (query.next < CURSOR_CAP) { c = cursors[query.next++]; }
            else { query.misses++; c = new Cursor(); }
            c.a = view;
            c.stride = stride > 0 ? stride : 1;
            c.count = Math.floor(view.length / c.stride);
            return c;
        },
        reset: function () {
            for (var i = 0; i < CURSOR_CAP; i++) { cursors[i].a = null; }
            query.next = 0;
        }
    };
    ctx.query = query;

    globalThis.ctx = ctx;
})();
)JS";
    return kSource;
}

} // namespace context::runtime::ts

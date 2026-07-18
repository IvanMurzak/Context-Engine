// Scope enforcement (R-SEC-007): attach-token scopes checked in the RPC DISPATCHER, not the adapter.
//
// R-SEC-007 (MUST): attach tokens carry scopes — read/query, file-write, session-control,
// build+install — and the default scope for an unrecognized client is read/query. Enforcement lives
// in the RPC dispatcher (adapter-level tool filtering is bypassable via direct RPC), so the
// dispatcher checks the token's scope on EVERY method regardless of the client door (CLI/RPC/MCP).
// "File-write is effectively code execution": a derivation-triggered dependency install requires the
// build+install scope. This header models the scope vocabulary + the method→required-scope table the
// dispatcher consults; read/query is the always-granted baseline.

#pragma once

#include <string>
#include <vector>

namespace context::editor::bridge
{

// The R-SEC-007 scope vocabulary. read_query is the baseline every token implicitly holds; the
// other three are least-privilege grants a launch-time operator scope (or a minted token) adds.
enum class Scope
{
    read_query,      // queries + derived reads; the default for an unrecognized client
    file_write,      // authored-file mutations (R-ARCH-002 file writes)
    session_control, // play/pause/step + session lifecycle
    build_install,   // build + package/dependency install ("file-write is effectively code execution")
};

// A granted scope set. read/query is ALWAYS satisfiable (baseline); the three non-baseline scopes
// are tracked as flags. Constructed empty (= read/query only) so an unrecognized client defaults to
// least privilege by construction.
class ScopeSet
{
public:
    ScopeSet() = default;

    // The baseline: read/query only (the unrecognized-client default).
    [[nodiscard]] static ScopeSet read_query();
    // All scopes — a fully-privileged launch-time operator token.
    [[nodiscard]] static ScopeSet all();
    // Parse a comma/space-separated scope spec, e.g. "read", "write,build", "session build-install".
    // Unknown tokens are ignored (least privilege); an empty/absent spec yields the read/query
    // baseline.
    [[nodiscard]] static ScopeSet parse(const std::string& spec);

    ScopeSet& grant(Scope s);
    // has() reports read/query as always held; the other scopes only when explicitly granted.
    [[nodiscard]] bool has(Scope s) const;

    // The scopes held by BOTH sets — the least-privilege result of clamping a requested scope set to
    // a launch-time operator ceiling (R-SEC-007). read/query survives (it is the baseline in both).
    [[nodiscard]] ScopeSet intersect(const ScopeSet& other) const;

    // The granted scope names for the attach/handshake payload (stable, grep-able strings). Always
    // includes "read-query"; then any explicitly-granted higher scopes in enum order.
    [[nodiscard]] std::vector<std::string> names() const;

private:
    unsigned mask_ = 0; // one bit per non-baseline scope
};

// The scope a given RPC method-id (the registry's rpc_method) requires. Unknown / read-only methods
// map to read_query. The install/build family (package.add, build, install) requires build_install;
// the file-writer family (set, new, edit, edit-batch, migrate, merge-file, resolve-conflict, re-key,
// asset.move, asset.rename) requires file_write; the session family (session.new/seed/step/inject/
// record, replay, ui.send, shutdown) requires session_control. Verbs still RESERVED on the bridge are
// gated by SEMANTIC CLASS regardless (defense-in-depth): a mutating verb is denied fail-closed under
// a read/query token the day its backing is wired, never silently exposed by the read baseline
// (R-SEC-007). Read-only siblings (session.hash, determinism.diff, ui.dump/query/assert, reconcile,
// validate, doctor, resource.read, snapshot, describe, query, subscribe/unsubscribe/ack) stay baseline.
[[nodiscard]] Scope required_scope_for(const std::string& rpc_method);

// Does `granted` satisfy the requirement for `rpc_method`? read/query-required methods are always
// authorized; otherwise the required scope must be explicitly present. This is the single predicate
// the dispatcher calls on every method (R-SEC-007).
[[nodiscard]] bool authorize(const std::string& rpc_method, const ScopeSet& granted);

// The diagnostic code the dispatcher emits when a token's scope does not permit a method. This is
// PROMOTED into the versioned error-code catalog (src/editor/contract/error_catalog.{h,cpp}): the
// contract cluster carries `scope.denied` (and its sibling `scope.insufficient`) on the frozen v0
// baseline with permission-class exit code 6 (R-SEC-007), so a scope-denied envelope's exit_code()
// classes as permission (6) rather than the generic error (1). The string is kept here — grep-stable
// and identical to the catalog code — so the bridge references the one catalog entry by value.
inline constexpr const char* kScopeDeniedCode = "scope.denied";

} // namespace context::editor::bridge

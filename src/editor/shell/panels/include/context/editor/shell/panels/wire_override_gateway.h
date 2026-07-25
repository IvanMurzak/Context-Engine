// The WIRE override-write gateway (M9 e09b-2, design 05 §7-§8): the editor's gesture commits go out
// over the daemon's `edit` RPC with raw-byte CAS, exactly like any other client's write.
//
// WHAT THIS RETIRES. Until e09b-2 the ONLY implementation of `inspector::OverrideWriteGateway` that
// could actually write was `gui/viewport`'s disk-backed one — it opens the project directory itself
// (compose::plan_write + filesync atomic IO), which the Shell structurally cannot do: `context_compose`
// and `context_filesync` are both on the D10 shell-boundary FORBIDDEN list (`src/CMakeLists.txt`,
// pinned by the `editor-shell-boundary` ctest). So the live Shell bound NO gateway and a staged edit
// committed nowhere. This gateway closes that: it holds NO kernel type, opens NO file, and reaches the
// SAME `compose::plan_write` path — on the DAEMON, over `edit`'s pointer/value mode (e09b-1). The
// FORBIDDEN list is untouched; the capability moved, the gate did not move.
//
// THE L-30 ENGINE IS NOT REIMPLEMENTED HERE. `commit_override_write` (inspector_panel.h) already owns
// rebase-or-drop at field-path granularity, over exactly the two operations this class supplies:
// `attempt` (CAS-guarded write) and `read` (re-read the field + the current CAS token). A `cas.mismatch`
// therefore reruns the EXISTING engine — this file adds a transport, not a policy.
//
// LIFETIME — THE CLIENT POINTER IS A NON-OWNING VIEW WITH A DEFINED CLEAR POINT, exactly as
// `session_feed.h` § LIFETIME states for the session feed, and for the same reason: the daemon
// lifecycle owns the `client::Client` and DESTROYS it on a lost daemon and at exit, while a panel
// write is renderer-driven and can land in that window. The owner therefore re-derives this binding
// from the lifecycle every frame and clears it with `nullptr` before tearing the lifecycle down — one
// seam, `panels::bind_write_client` (builtin_panels.h). An UNBOUND gateway is not a silent no-op: every
// `attempt` refuses with `kNoDaemonCode` and every `read` answers `present:false` with a 0 token, which
// the L-30 engine reads as "this field cannot be verified" and therefore DROPS rather than overwrites.
// Fail-closed is the only acceptable posture on a user-data write path.
//
// READ-YOUR-WRITES (05 §7) — WHERE THE BARRIER ACTUALLY IS. The design names an `--after-generation`
// barrier; in the shipped contract that core flag is RESERVED-but-accepted and inert
// (`registry.cpp` make_core_flags: "reserved and accepted, replay store lands later"), so SENDING it
// would be a barrier in name only. The LIVE R-CLI-006 own-write barrier is `--after-hash`
// (`EditorKernel::query_after_hash`), and the daemon ALREADY applies it inside `edit` itself
// (`kernel_server.cpp` finish_edit) before replying — reporting the verdict as `reflected`. This
// gateway therefore does not re-request a barrier the reply already passed; it RECORDS the verdict
// (`barrier_misses()`), so a barrier that timed out is visible instead of silent. The Inspector's own
// re-read after a commit is `editor.inspect`, which composes from REAL DISK through a fresh
// `ProjectSceneResolver` — strictly stronger than a derivation barrier for this field.

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::client
{
// Forward-declared, NOT included: only wire_override_gateway.cpp needs the complete type (the same
// discipline session_feed.h applies to its client).
class Client;
} // namespace context::editor::client

namespace context::editor::shell::panels
{

namespace inspector = gui::panels::inspector;

class WireOverrideWriteGateway final : public inspector::OverrideWriteGateway
{
public:
    WireOverrideWriteGateway() = default;

    // Non-copyable and non-movable: `InspectorPanel::set_gateway` stores a raw pointer to it, so an
    // object that could be relocated out from under that pointer is a use-after-free waiting to
    // happen (the PanelHost/BridgeRouter rule).
    WireOverrideWriteGateway(const WireOverrideWriteGateway&) = delete;
    WireOverrideWriteGateway& operator=(const WireOverrideWriteGateway&) = delete;
    WireOverrideWriteGateway(WireOverrideWriteGateway&&) = delete;
    WireOverrideWriteGateway& operator=(WireOverrideWriteGateway&&) = delete;

    // The LOCAL refusal code an unbound gateway answers with. Deliberately NOT a catalog code and
    // deliberately NOT `internal.error`: nothing failed internally — there was simply no daemon to
    // ask, which is an ordinary editor state (booting, reconnecting, exiting). `panel_host.h` states
    // the same rule for its `panel.*` codes: a host-side condition does not get to pollute the
    // published R-CLI-008 catalog.
    static constexpr const char* kNoDaemonCode = "shell.no_daemon";

    // Bind the live connection; `nullptr` detaches (see § LIFETIME above). The Shell always goes
    // through `panels::bind_write_client`, next to the session feed's own re-derive.
    void bind_client(client::Client* client) noexcept;
    [[nodiscard]] bool has_client() const noexcept { return client_ != nullptr; }

    // --- the OverrideWriteGateway seam ------------------------------------------------------------

    [[nodiscard]] inspector::WriteAttempt attempt(const inspector::OverrideWriteRequest& request,
                                                  std::uint64_t expected_raw_hash) const override;

    [[nodiscard]] inspector::FieldState read(const std::string& root_scene,
                                             const std::vector<std::string>& id_path,
                                             const std::string& pointer) const override;

    // --- the wire vocabulary (pure, and asserted against the daemon's own spellings in T1) --------

    // The `target` wire token for an override-write target. MIRRORS `compose::write_target_token`,
    // which the Shell cannot link (D10). An unknown token is refused by the daemon rather than
    // defaulting to `outermost` (e09b-1), so a drift here reddens the T1 round-trip, never writes the
    // wrong file.
    [[nodiscard]] static const char* write_target_token(
        inspector::OverrideWriteTarget target) noexcept;

    // Join an L-35 id-path into the wire identity key. MIRRORS `builders::join_identity` — likewise
    // unlinkable here — and the T1 suite, which DOES link the builders, asserts the round trip
    // through the exported `builders::split_identity` inverse the daemon itself parses with.
    [[nodiscard]] static std::string join_id_path(const std::vector<std::string>& id_path);

    // --- observability (what the T1/T2 suites assert on) ------------------------------------------

    [[nodiscard]] std::size_t writes_issued() const noexcept { return writes_issued_; }
    [[nodiscard]] std::size_t writes_applied() const noexcept { return writes_applied_; }
    // How many `attempt`s the daemon refused with `cas.mismatch` — the L-30 trigger count.
    [[nodiscard]] std::size_t cas_refusals() const noexcept { return cas_refusals_; }
    [[nodiscard]] std::size_t reads_issued() const noexcept { return reads_issued_; }
    // How many APPLIED writes came back with `reflected:false` — the daemon's own read-your-writes
    // barrier bound expired before the derived world caught up. Not a failure (the bytes are on
    // disk), but a barrier miss must be countable rather than invisible (see § READ-YOUR-WRITES).
    [[nodiscard]] std::size_t barrier_misses() const noexcept { return barrier_misses_; }

private:
    client::Client* client_ = nullptr;

    // `mutable` because the seam's two methods are `const` (a gateway is a READER of the write path
    // from the panel's point of view) while these count what actually crossed the wire.
    mutable std::size_t writes_issued_ = 0;
    mutable std::size_t writes_applied_ = 0;
    mutable std::size_t cas_refusals_ = 0;
    mutable std::size_t reads_issued_ = 0;
    mutable std::size_t barrier_misses_ = 0;
};

} // namespace context::editor::shell::panels

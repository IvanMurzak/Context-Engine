// The WIRE file-write gateway (M9 e2, design 05 §8 / D10 write half): the Files panel's rename,
// move and delete go out over the daemon's `editor file-move` / `editor file-delete` /
// `editor file-restore` verbs, exactly like any other client's write.
//
// WHY THE SHELL CANNOT DO THIS ITSELF, which is the whole reason this class exists. The engine
// operation lives in `context_assetdb` (the GUID index decides identity; the R-FILE-004 write order
// decides safety), and both `context_assetdb` and `context_filesync` are on the D10 shell-boundary
// FORBIDDEN list (`src/CMakeLists.txt`, pinned by the `editor-shell-boundary` ctest). So the Shell
// structurally cannot open the project directory, cannot read a sidecar, and cannot move a byte.
// This gateway holds NO kernel type and opens NO file: it turns a panel request into an RPC and the
// reply into a `files::FileWriteResult`. The capability moved to the daemon; the gate did not move.
//
// LIFETIME — the non-owning client pointer with a defined clear point, verbatim the rule
// `wire_override_gateway.h` § LIFETIME states and for the identical reason: the daemon lifecycle
// owns the `client::Client` and DESTROYS it on a lost daemon and at exit, while a panel write is
// renderer-driven and can land in that window. `panels::bind_write_client` (builtin_panels.h) is the
// ONE seam that re-derives this binding every frame and clears it with `nullptr` before the
// lifecycle is torn down. An UNBOUND gateway is NOT a silent no-op: every operation refuses with
// `kNoDaemonCode`, which the panel renders and the Shell narrates.
//
// FAIL-CLOSED IS THE ONLY ACCEPTABLE POSTURE ON A DESTRUCTIVE PATH, and here it is structural
// rather than argued: there is no path through this class that reports `applied` without a daemon
// having said so. The one asymmetry worth naming is the DELETE reply's `restoreToken` — an applied
// delete whose reply carried no token is still applied (the bytes are gone), and this gateway
// reports exactly that, with an empty token. The caller (the undo journal) then records no
// reversible step rather than recording one that cannot be replayed — a missing undo entry is
// recoverable by the human, a phantom one is not.
//
// CEF-FREE and D10 boundary-clean like its sibling, so `tests/test_wire_file_gateway.cpp` drives the
// SAME class the real Shell runs, on all three default `build` legs.

#pragma once

#include "context/editor/gui/panels/files/files_panel.h"
#include "context/editor/shell/panels/wire_override_gateway.h" // kNoDaemonCode (shared, see below)

#include <cstddef>
#include <string>

namespace context::editor::client
{
// Forward-declared, NOT included: only wire_file_gateway.cpp needs the complete type (the same
// discipline session_feed.h / wire_override_gateway.h apply to their clients).
class Client;
} // namespace context::editor::client

namespace context::editor::shell::panels
{

namespace files = gui::panels::files;

class WireFileWriteGateway final : public files::FileWriteGateway
{
public:
    WireFileWriteGateway() = default;

    // Non-copyable and non-movable: `FilesPanel::set_write_gateway` and `UndoJournal` both store a
    // raw pointer to it, so an object that could be relocated out from under those pointers is a
    // use-after-free waiting to happen (the PanelHost/BridgeRouter rule).
    WireFileWriteGateway(const WireFileWriteGateway&) = delete;
    WireFileWriteGateway& operator=(const WireFileWriteGateway&) = delete;
    WireFileWriteGateway(WireFileWriteGateway&&) = delete;
    WireFileWriteGateway& operator=(WireFileWriteGateway&&) = delete;

    // The LOCAL refusal code an unbound gateway answers with — the same host-minted, uncatalogued
    // code `WireOverrideWriteGateway` uses, and for the same reason: nothing failed internally,
    // there was simply no daemon to ask, which is an ordinary editor state (booting, reconnecting,
    // exiting). It must be spelled identically so a renderer that groups notices by code sees ONE
    // "no daemon" class rather than two — so it is ALIASED to that one rather than re-spelled here,
    // because a second literal is an invariant maintained by eye whose failure is a silently split
    // notice class.
    static constexpr const char* kNoDaemonCode = WireOverrideWriteGateway::kNoDaemonCode;

    // The wire method ids. Named constants rather than string literals at the call sites because
    // the T2 drill asserts them against a LIVE daemon: a drift on either side reddens a ctest rather
    // than silently refusing every file operation with `usage.unknown_verb`.
    static constexpr const char* kMoveMethod = "editor.file-move";
    static constexpr const char* kDeleteMethod = "editor.file-delete";
    static constexpr const char* kRestoreMethod = "editor.file-restore";

    // Bind the live connection; `nullptr` detaches (see § LIFETIME above).
    void bind_client(client::Client* client) noexcept;
    [[nodiscard]] bool has_client() const noexcept { return client_ != nullptr; }

    // --- the FileWriteGateway seam ----------------------------------------------------------------

    [[nodiscard]] files::FileWriteResult move_file(const std::string& from,
                                                   const std::string& to) override;
    [[nodiscard]] files::FileWriteResult delete_file(const std::string& path) override;
    [[nodiscard]] files::FileWriteResult restore_file(const std::string& restore_token) override;

    // --- observability (what the T1/T2 suites assert on) ------------------------------------------

    [[nodiscard]] std::size_t writes_issued() const noexcept { return writes_issued_; }
    [[nodiscard]] std::size_t writes_applied() const noexcept { return writes_applied_; }
    // How many operations the write path REFUSED (including the unbound-gateway refusals) — the
    // counter that makes "the human was told" checkable rather than assumed.
    [[nodiscard]] std::size_t refusals() const noexcept { return refusals_; }
    // How many APPLIED deletes came back with no `restoreToken`, i.e. landed IRREVERSIBLY. Zero on
    // every path this build can reach; countable rather than invisible, because the day it is not
    // zero is the day a human loses an undo they were promised.
    [[nodiscard]] std::size_t unrecoverable_deletes() const noexcept
    {
        return unrecoverable_deletes_;
    }

private:
    client::Client* client_ = nullptr;
    std::size_t writes_issued_ = 0;
    std::size_t writes_applied_ = 0;
    std::size_t refusals_ = 0;
    std::size_t unrecoverable_deletes_ = 0;
};

} // namespace context::editor::shell::panels

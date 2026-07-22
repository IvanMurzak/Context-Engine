// `context edit .` — the project-OPEN entry path (M9 e14b, design 07 §4 / 05, D15/C-F23).
//
// This is the file-association / "Open with" handler's logic, reachable from a terminal as
// `context edit <project-dir>` (`.` = the current directory). It resolves the project, then arbitrates
// per-project single-instance via the editor presence marker (client::arbitrate_open): a live editor
// on that project is FOCUSED (the handshake writes a focus request its owner loop consumes) instead of
// duplicated; otherwise a new editor is launched, feeding the e14a spawn-or-attach lifecycle.
//
// It is a CLI-LOCAL OPERATIONAL command (it launches a GUI process), NOT a contract registry verb —
// so it is intercepted in main_cli, exactly like `daemon` / `bench`, before registry resolution. It is
// disambiguated from the reserved operational `edit` file-writer purely by SHAPE: `context edit <dir>`
// (one positional naming an existing directory, or `.`) opens; `context edit <file> <content>` (the
// two-positional write) is untouched and still returns `contract.operational_only`.

#pragma once

#include "context/editor/contract/envelope.h"

#include <filesystem>
#include <string>
#include <vector>

namespace context::cli
{

// True when the args AFTER the leading `edit` selector are the project-OPEN shape: exactly one non-flag
// positional that is `.`, `..`, or an existing directory. False for the two-positional file-write shape
// (which stays the reserved operational verb) and for a lone file positional. Pure over the filesystem
// state at call time.
[[nodiscard]] bool is_edit_open_shape(const std::vector<std::string>& args_after_edit);

// Run `context edit <dir>`: resolve the project, arbitrate, and either report a focus of the running
// editor or launch a new one. `self_exe` is argv[0] of the running `context` binary, used to locate the
// sibling `context_editor` to spawn. Flags: `--no-launch` (arbitrate + report the decision, and STILL
// focus a live editor, but do not spawn a new process — the CI/handler-probe path), `--focus-timeout-ms
// <n>` (how long to wait for a live editor to acknowledge the focus request; default below). Never
// throws for user-input errors — they become failure envelopes with catalog codes.
[[nodiscard]] editor::contract::Envelope run_edit_open(const std::vector<std::string>& args_after_edit,
                                                       const std::filesystem::path& self_exe);

// Locate the `context_editor` shell binary to spawn, relative to the running `context` executable —
// the inverse of the shell's locate_context_binary. Checks the install-layout sibling and the dev
// build-tree layout (`<build>/cli` -> `<build>/editor/shell`); returns the first that exists, else a
// sibling best-guess so the spawn reports a clear error. Exposed for unit testing.
[[nodiscard]] std::filesystem::path locate_editor_binary(const std::filesystem::path& self_exe);

} // namespace context::cli

// The keybindings read/watch bridge surface (M9 e07c, design 05 §6 / 03 §6 / 04 §1).
//
// WHAT THIS IS. editor-core owns the keymap (default map + resolver, keymap.ts), but it is a PURE
// WIRE-CLIENT (04 §1 / 08 §1) — it has no filesystem and CANNOT read the per-user override
// `~/.context/keybindings.json` itself. Reading, WATCHING, and hot-reloading that file is a SHELL
// responsibility; this bridge does it and PUBLISHES the raw bytes to editor-core over the SAME
// privileged e05c bridge (`keybindings.get`), which schema-validates + merges them. The Shell serves
// BYTES ONLY — it does not know or enforce the keybindings schema; that discipline lives in
// editor-core (keymap.ts `parseUserKeybindings`), so a malformed file is rejected THERE with a
// diagnostic and the defaults stand.
//
// D10 BOUNDARY-CLEAN, deliberately. This reads a per-user CONFIG file with plain std::filesystem +
// stream IO — it does NOT touch the authored-project file machinery (context_filesync / the derived
// index), which is exactly what keeps `keybindings.cpp` out of the shell-boundary gate's FORBIDDEN
// closure. There is no reason for this to reach a kernel-internal target, and it must not: the gate's
// FORBIDDEN list stays byte-identical (design: "add the bridge in a boundary-clean module").
//
// CEF-FREE, like ipc_bridge.h / editor_state_bridge.h and for the same reason: the router handler runs
// nowhere the local dev gate can reach, so the read/watch logic lives here where the T1 suite
// (tests/test_keybindings_bridge.cpp) drives the SAME code the renderer reaches, on all three default
// `build` legs.
//
// THE WATCH. `poll()` is driven from the shell's single-threaded owner loop. It re-reads the file only
// when its mtime/size moved (a cheap stat gates the read), and bumps a GENERATION counter when the
// observed content actually changes. editor-core reads that generation over `keybindings.get` and
// re-applies only when it moves — so hot reload costs one counter compare on the renderer side and one
// stat per loop on the Shell side, with the expensive read happening once per real change.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace context::editor::shell
{

// The bridge method editor-core calls to read the override snapshot. Grep-stable and MIRRORED by the
// TS side (src/editor/webui/core/src/keymap.ts `KEYBINDINGS_GET_METHOD`). The `webui-panel-contract`
// gate re-reads this value out of the BUILT bundle and compares it to this constant, the same
// cross-language discipline editor_state_bridge.h's `kEditor*Method` uses: a rename on either side
// would leave editor-core calling a method the Shell no longer routes, so the user override would
// silently never load and NOTHING would report it.
inline constexpr const char* kKeybindingsGetMethod = "keybindings.get";

// The largest override file the Shell will read and ship. A hand-authored keybindings file is tiny; a
// file past this cap is treated as unreadable (served as absent) rather than pushing a multi-megabyte
// blob through the response path. Comfortably above any real keymap, well under the bridge's 1 MiB.
inline constexpr std::size_t kMaxKeybindingsBytes = 256u * 1024u;

// The user's home directory (`$HOME` on POSIX, `%USERPROFILE%` on Windows), or nullopt when neither is
// set. Reads the VALUE, so unlike doctor_command.cpp's presence-only probe the MSVC branch uses the
// two-call getenv_s form; the local GCC gate + the Clang legs compile the std::getenv branch.
[[nodiscard]] std::optional<std::filesystem::path> home_directory();

// `<home>/.context/keybindings.json`, or an EMPTY path when the home directory cannot be resolved (in
// which case the bridge serves a permanently-absent snapshot — the editor runs on the default keymap).
[[nodiscard]] std::filesystem::path default_keybindings_path();

// The snapshot the bridge serves and the watch maintains. `text` is meaningful only when `present`.
struct KeybindingsSnapshot
{
    bool present = false;
    std::uint64_t generation = 0; // bumped on every OBSERVED content change (the hot-reload trigger)
    std::string text;             // the raw file bytes, UNVALIDATED (editor-core owns the schema)
};

class KeybindingsBridge
{
public:
    KeybindingsBridge() = default;

    // Non-copyable and non-movable, like the sibling bridges: `install` binds a handler capturing
    // `this`, and a router outlives nothing that could be relocated out from under it.
    KeybindingsBridge(const KeybindingsBridge&) = delete;
    KeybindingsBridge& operator=(const KeybindingsBridge&) = delete;
    KeybindingsBridge(KeybindingsBridge&&) = delete;
    KeybindingsBridge& operator=(KeybindingsBridge&&) = delete;

    // Point the bridge at the override file. The composition root binds `default_keybindings_path()`;
    // the T1 suite binds a temp file so the read/watch/generation behaviour is deterministic. An EMPTY
    // path means "no home directory" — the bridge then serves a permanently-absent snapshot. Binding
    // performs the FIRST read so `snapshot()` is meaningful before the first `poll()`.
    void bind_path(std::filesystem::path path);

    // The watch tick, driven from the owner loop. A cheap stat gates the read: the file is re-read only
    // when its mtime or size moved, and `generation` is bumped only when the observed CONTENT actually
    // changes (a rewrite with identical bytes bumps nothing). Returns true when the snapshot changed
    // this call — the signal a caller (or a test) uses to know a hot reload is pending on the renderer.
    [[nodiscard]] bool poll();

    // The current `{present, generation, text}` snapshot as a JSON object (what `keybindings.get`
    // returns). An oversized/unreadable file reads as absent with empty text.
    [[nodiscard]] contract::Json snapshot_json() const;

    // Bind `keybindings.get` on `router`. False when the binding was refused (a name collision), which
    // the caller must treat as a wiring bug rather than ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (state + the live-smoke assertion surface) ----------------------------------
    [[nodiscard]] bool present() const { return snapshot_.present; }
    [[nodiscard]] std::uint64_t generation() const { return snapshot_.generation; }
    [[nodiscard]] const std::string& text() const { return snapshot_.text; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    // How many times `keybindings.get` was served over the router — the CEF smoke asserts this is
    // non-zero after the live renderer boots (the end-to-end proof the override channel is wired).
    [[nodiscard]] std::size_t reads() const { return reads_; }

private:
    // Read the file at `path_` (if any) into a fresh snapshot state, applying the size cap. Returns
    // true when the observed (present, text) state differs from the current snapshot, bumping the
    // generation in that case.
    [[nodiscard]] bool refresh_from_disk();
    // Adopt a new (present, text) observation, bumping the generation iff it differs from the current.
    [[nodiscard]] bool adopt(bool present, std::string text);

    std::filesystem::path path_;
    KeybindingsSnapshot snapshot_;
    // Cheap-path stat cache — the last mtime/size we read, so an unchanged file skips the re-read.
    bool have_stat_ = false;
    std::filesystem::file_time_type last_write_{};
    std::uintmax_t last_size_ = 0;
    std::size_t reads_ = 0;
};

} // namespace context::editor::shell

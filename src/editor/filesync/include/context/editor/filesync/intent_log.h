// Crash-recovery intent log + serialized multi-file write queue (R-FILE-004).

#pragma once

#include "context/editor/filesync/diagnostic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::kernel
{
class Clock;
} // namespace context::kernel

namespace context::editor::filesync
{

class FileStore;

// What one planned step does. `remove` exists for the multi-file MOVE verbs (L-33 owned satellites:
// a move is dest writes + src removals inside ONE intent-logged op, so a crash mid-move resumes to
// the moved state instead of silently forking the file). Recovery semantics per kind are documented
// on WriteQueue::recover.
enum class WriteKind : std::uint8_t
{
    write = 0,
    remove = 1,
};

// One planned file write inside a multi-file operation.
//   expected_prev_hash — content-hash of the file read at PLANNING time (the CAS guard: on resume, if
//                         the file no longer holds this, it moved on and must NOT be clobbered).
//   target_hash        — content-hash of the bytes we intend to write (used to detect "already
//                         applied" for idempotent resume). Unused (0) for kind == remove, where
//                         "already applied" means the file is ABSENT.
//   data               — the payload. Kept in the log so a resume can always complete (M1
//                         simplification; a production impl would stage payloads in the temp files to
//                         avoid doubling write volume). See the component README. Empty for removes.
//   kind               — write (atomic temp+rename) or remove (delete; CAS-guarded like a write).
struct PlannedWrite
{
    std::string path;
    std::uint64_t expected_prev_hash = 0;
    std::uint64_t target_hash = 0;
    std::string data;
    WriteKind kind = WriteKind::write;
};

// A bounded crash-recovery intent-log entry: (opId, incarnationId, planned writes + target hashes).
struct IntentEntry
{
    std::string op_id;
    std::string incarnation_id;
    std::vector<PlannedWrite> writes;
};

// Load-or-create the per-project HMAC secret under `<editor_dir>/hmac.key` (created with 32 random
// bytes if absent). NOTE: the native impl stores it 0600 (attach-token trust class, R-BRIDGE-007);
// the in-memory seam has no permission model. The key protects against corruption + foreign-log
// replay, NOT same-user tampering (R-FILE-004 / R-SEC-010).
[[nodiscard]] std::string ensure_hmac_key(FileStore& fs, std::string_view editor_dir);

// The intent log itself — one HMAC-stamped file per in-flight op under `<editor_dir>/intent/`.
class IntentLog
{
public:
    IntentLog(FileStore& fs, std::string editor_dir, std::string hmac_key);

    // fsync-before: durably write the intent entry BEFORE the caller performs any of its writes.
    bool begin(const IntentEntry& entry);
    // clear-after: remove the entry AFTER the last durable rename of the op.
    void clear(std::string_view op_id);

    // Op ids with a pending entry on disk.
    [[nodiscard]] std::vector<std::string> pending() const;

    // Load + integrity-verify one entry. Returns nullopt (and sets *error) when the file is missing or
    // the HMAC fails (corruption or a foreign / cross-project log). A caller turns that into a
    // Diagnostic rather than trusting the bytes.
    [[nodiscard]] std::optional<IntentEntry> load(std::string_view op_id, std::string& error) const;

    [[nodiscard]] std::string op_path(std::string_view op_id) const;

private:
    FileStore& fs_;
    std::string dir_; // <editor_dir>/intent
    std::string hmac_key_;
};

// Serializes multi-file verbs through the intent log so a crash never loses a half-applied op.
class WriteQueue
{
public:
    WriteQueue(FileStore& fs, std::string root, IntentLog& log, context::kernel::Clock& clock);

    // Execute a multi-file verb: fsync the intent entry, apply each write per-file-atomically in the
    // given (dependency-safe) order, then clear the entry. Returns true when every write durably
    // landed and the entry was cleared. May propagate SimulatedCrash from the injected FileStore
    // (that is how a test models a crash mid-op) — the entry then stays on disk for recover().
    bool execute(std::string_view op_id, const std::vector<PlannedWrite>& writes);

    // Recovery pass on restart: for each pending op, verify integrity, then for each planned step
    // re-JAIL (R-SEC-008) + re-CAS against the planning-time hash. For kind == write:
    //   current == target_hash        -> already applied, skip (idempotent replay)
    //   current == expected_prev_hash -> safe to (re)apply the write
    //   otherwise                     -> the file moved on: do NOT clobber; emit a CAS diagnostic
    // For kind == remove:
    //   file absent                   -> already applied, skip (idempotent replay)
    //   current == expected_prev_hash -> safe to (re)apply the removal
    //   otherwise                     -> the file moved on: do NOT delete; emit a CAS diagnostic
    // A fully-resumed op is cleared; an op that could not be fully + safely resumed is left on disk
    // and reported via a Diagnostic naming it. Returns the diagnostics (empty == clean recovery).
    std::vector<Diagnostic> recover();

private:
    FileStore& fs_;
    std::string root_;
    IntentLog& log_;
    context::kernel::Clock& clock_;
};

} // namespace context::editor::filesync

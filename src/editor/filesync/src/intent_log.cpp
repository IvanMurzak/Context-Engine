// Crash-recovery intent log + serialized write queue implementation.

#include "context/editor/filesync/intent_log.h"

#include "context/editor/filesync/atomic_io.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/hmac.h"
#include "context/editor/filesync/path_jail.h"
#include "context/kernel/platform.h"

#include <random>
#include <sstream>
#include <utility>

namespace context::editor::filesync
{
namespace
{

// --- length-prefixed body (de)serialization -----------------------------------------------------
//
// The entry body is a sequence of length-prefixed fields so a payload may contain ANY bytes
// (newlines included). The on-disk file is: "<hmac-hex>\n<body>", so integrity is one recompute over
// everything after the first newline.

void put_field(std::ostringstream& out, std::string_view value)
{
    out << value.size() << '\n';
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    out << '\n';
}

void put_u64(std::ostringstream& out, std::uint64_t value)
{
    out << value << '\n';
}

std::string serialize_body(const IntentEntry& entry)
{
    std::ostringstream out;
    put_field(out, entry.op_id);
    put_field(out, entry.incarnation_id);
    put_u64(out, entry.writes.size());
    for (const PlannedWrite& write : entry.writes)
    {
        put_field(out, write.path);
        put_u64(out, write.expected_prev_hash);
        put_u64(out, write.target_hash);
        put_field(out, write.data);
        put_u64(out, write.kind == WriteKind::remove ? 1 : 0);
    }
    return out.str();
}

// A cursor over the serialized body.
class Cursor
{
public:
    explicit Cursor(const std::string& text) : text_(text) {}

    [[nodiscard]] bool read_u64(std::uint64_t& out)
    {
        std::string line;
        if (!read_line(line))
            return false;
        try
        {
            out = std::stoull(line);
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool read_field(std::string& out)
    {
        std::uint64_t len = 0;
        if (!read_u64(len))
            return false;
        if (pos_ + len > text_.size())
            return false;
        out = text_.substr(pos_, static_cast<std::size_t>(len));
        pos_ += static_cast<std::size_t>(len);
        // Skip the trailing separator newline.
        if (pos_ >= text_.size() || text_[pos_] != '\n')
            return false;
        ++pos_;
        return true;
    }

private:
    [[nodiscard]] bool read_line(std::string& out)
    {
        const std::size_t nl = text_.find('\n', pos_);
        if (nl == std::string::npos)
            return false;
        out = text_.substr(pos_, nl - pos_);
        pos_ = nl + 1;
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

std::optional<IntentEntry> parse_body(const std::string& body)
{
    Cursor cursor{body};
    IntentEntry entry;
    if (!cursor.read_field(entry.op_id) || !cursor.read_field(entry.incarnation_id))
        return std::nullopt;
    std::uint64_t count = 0;
    if (!cursor.read_u64(count))
        return std::nullopt;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        PlannedWrite write;
        std::uint64_t kind = 0;
        if (!cursor.read_field(write.path) || !cursor.read_u64(write.expected_prev_hash) ||
            !cursor.read_u64(write.target_hash) || !cursor.read_field(write.data) ||
            !cursor.read_u64(kind) || kind > 1)
            return std::nullopt;
        write.kind = kind == 1 ? WriteKind::remove : WriteKind::write;
        entry.writes.push_back(std::move(write));
    }
    return entry;
}

std::string op_basename(const std::string& op_id)
{
    // Keep op-id filenames flat + safe: intent files live directly under <editor>/intent/.
    std::string safe;
    safe.reserve(op_id.size());
    for (char ch : op_id)
        safe.push_back((ch == '/' || ch == '\\') ? '_' : ch);
    // A basename that lexically resolves to "." or ".." would escape the intent/ subtree
    // (op_path() would yield "<editor>/intent/.." == "<editor>"); neutralize the traversal segment.
    if (safe == "." || safe == "..")
        safe.insert(safe.begin(), '_');
    return safe;
}

} // namespace

std::string ensure_hmac_key(FileStore& fs, std::string_view editor_dir)
{
    const std::string key_path = std::string{editor_dir} + "/hmac.key";
    if (const std::optional<std::string> existing = fs.read(key_path))
        return *existing;

    // std::random_device is the entropy source. On most platforms it is a real OS CSPRNG; on a few
    // (older MinGW libstdc++) it is deterministic, making the key predictable and weakening the
    // integrity check. Acceptable for M1: the HMAC's job is corruption / foreign-log detection, NOT
    // defending against a same-user attacker who can already read `.editor/hmac.key`. A hardened build
    // should mix in a high-resolution clock / OS CSPRNG on platforms with a weak random_device.
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, 255);
    std::string key;
    key.reserve(32);
    for (int i = 0; i < 32; ++i)
        key.push_back(static_cast<char>(dist(rd)));

    atomic_write(fs, key_path, key, "keygen");
    return key;
}

IntentLog::IntentLog(FileStore& fs, std::string editor_dir, std::string hmac_key)
    : fs_(fs), dir_(std::move(editor_dir) + "/intent"), hmac_key_(std::move(hmac_key))
{
}

std::string IntentLog::op_path(std::string_view op_id) const
{
    return dir_ + "/" + op_basename(std::string{op_id});
}

bool IntentLog::begin(const IntentEntry& entry)
{
    const std::string body = serialize_body(entry);
    const std::string file = hmac_sha256_hex(hmac_key_, body) + "\n" + body;
    // Atomic + fsync: the entry is durable before the caller performs its first write.
    return atomic_write(fs_, op_path(entry.op_id), file, "intent");
}

void IntentLog::clear(std::string_view op_id)
{
    fs_.remove(op_path(op_id));
}

std::vector<std::string> IntentLog::pending() const
{
    std::vector<std::string> ops;
    const std::string prefix = dir_ + "/";
    for (const std::string& path : fs_.list(dir_))
    {
        if (is_atomic_temp_name(path))
            continue; // ignore staging residue
        if (path.rfind(prefix, 0) == 0)
            ops.push_back(path.substr(prefix.size()));
    }
    return ops;
}

std::optional<IntentEntry> IntentLog::load(std::string_view op_id, std::string& error) const
{
    const std::optional<std::string> content = fs_.read(op_path(op_id));
    if (!content)
    {
        error = "intent entry missing";
        return std::nullopt;
    }
    const std::size_t nl = content->find('\n');
    if (nl == std::string::npos)
    {
        error = "intent entry malformed (no HMAC line)";
        return std::nullopt;
    }
    const std::string mac = content->substr(0, nl);
    const std::string body = content->substr(nl + 1);
    if (hmac_sha256_hex(hmac_key_, body) != mac)
    {
        error = "intent entry HMAC mismatch (corruption or foreign-log replay)";
        return std::nullopt;
    }
    std::optional<IntentEntry> entry = parse_body(body);
    if (!entry)
    {
        error = "intent entry body unparseable";
        return std::nullopt;
    }
    return entry;
}

WriteQueue::WriteQueue(FileStore& fs, std::string root, IntentLog& log,
                       context::kernel::Clock& clock)
    : fs_(fs), root_(std::move(root)), log_(log), clock_(clock)
{
}

bool WriteQueue::execute(std::string_view op_id, const std::vector<PlannedWrite>& writes)
{
    IntentEntry entry;
    entry.op_id = std::string{op_id};
    entry.incarnation_id = std::to_string(clock_.now_nanos());
    entry.writes = writes;

    if (!log_.begin(entry))
        return false;

    for (std::size_t i = 0; i < writes.size(); ++i)
    {
        const PlannedWrite& write = writes[i];
        if (!is_inside_jail(root_, write.path))
            return false; // front-door writes are jailed; leave the entry for recover() to report.
        if (write.kind == WriteKind::remove)
        {
            // Idempotent: an already-absent file IS the target state. remove() routes through the
            // FileStore seam, so the native store's TOCTOU-safe jail (R-SEC-008) covers it too.
            if (fs_.exists(write.path) && !fs_.remove(write.path))
                return false;
            continue;
        }
        const std::string unique = std::string{op_id} + "." + std::to_string(i);
        if (!atomic_write(fs_, write.path, write.data, unique))
            return false;
    }

    log_.clear(op_id);
    return true;
}

std::vector<Diagnostic> WriteQueue::recover()
{
    std::vector<Diagnostic> diagnostics;

    for (const std::string& op_name : log_.pending())
    {
        std::string error;
        const std::optional<IntentEntry> entry = log_.load(op_name, error);
        if (!entry)
        {
            // Integrity / foreign-log / parse failure: no verified entry, so the on-disk basename is
            // the only id we can name. (For a loaded entry below, we name entry->op_id — the caller's
            // ORIGINAL id, which may differ from the sanitized on-disk basename.)
            diagnostics.push_back(Diagnostic{"filesync.intent.integrity", op_name, error, {}});
            continue;
        }

        bool fully_resumed = true;

        for (std::size_t i = 0; i < entry->writes.size(); ++i)
        {
            const PlannedWrite& write = entry->writes[i];

            if (!is_inside_jail(root_, write.path))
            {
                fully_resumed = false;
                diagnostics.push_back(Diagnostic{"filesync.intent.jail", entry->op_id,
                                                 "planned write escapes the project root: " +
                                                     write.path,
                                                 {write.path}});
                continue;
            }

            const std::optional<std::string> current = fs_.read(write.path);

            if (write.kind == WriteKind::remove)
            {
                if (!current)
                    continue; // already applied — the file is absent (idempotent replay).
                if (content_hash(*current) == write.expected_prev_hash)
                {
                    // Safe to (re)apply the removal — same jail + seam as a fresh remove.
                    if (!fs_.remove(write.path))
                    {
                        fully_resumed = false;
                        diagnostics.push_back(Diagnostic{"filesync.intent.resume", entry->op_id,
                                                         "resume removal did not complete "
                                                         "(jail-refused or I/O failure): " +
                                                             write.path,
                                                         {write.path}});
                    }
                    continue;
                }
                // The file moved on since planning: do NOT delete it (L-25 — no rollback either).
                fully_resumed = false;
                diagnostics.push_back(Diagnostic{"filesync.intent.cas", entry->op_id,
                                                 "content changed since crash; not removing " +
                                                     write.path,
                                                 {write.path}});
                continue;
            }

            const std::uint64_t current_hash = content_hash(current ? *current : std::string{});

            if (current_hash == write.target_hash)
                continue; // already applied — idempotent skip.

            if (current_hash == write.expected_prev_hash)
            {
                // The unique tag becomes part of a temp filename, so use the slash-free on-disk
                // basename (not entry->op_id, which may contain a path separator).
                const std::string unique = op_name + ".r" + std::to_string(i);
                if (!atomic_write(fs_, write.path, write.data, unique))
                {
                    // The store refused or failed the re-apply — e.g. the native store's TOCTOU-safe
                    // jail (R-SEC-008) rejected a resume whose target now resolves outside the
                    // project root (a symlink swapped in since the crash), or a real I/O error.
                    // R-FILE-004: an op that cannot be fully + safely resumed is REPORTED, never
                    // silently dropped; the entry stays pending.
                    fully_resumed = false;
                    diagnostics.push_back(Diagnostic{"filesync.intent.resume", entry->op_id,
                                                     "resume write did not complete durably "
                                                     "(jail-refused or I/O failure): " +
                                                         write.path,
                                                     {write.path}});
                }
                continue;
            }

            // The file moved on since planning: refuse to clobber it (L-25 — no rollback either).
            fully_resumed = false;
            diagnostics.push_back(Diagnostic{"filesync.intent.cas", entry->op_id,
                                             "content changed since crash; not clobbering " +
                                                 write.path,
                                             {write.path}});
        }

        if (fully_resumed)
            log_.clear(op_name);
    }

    return diagnostics;
}

} // namespace context::editor::filesync

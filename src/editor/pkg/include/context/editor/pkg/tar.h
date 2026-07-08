// A minimal, dependency-free POSIX ustar reader/writer for the R-SEC-005 install path. Package
// artifacts the verified installer consumes are UNCOMPRESSED ustar archives whose SRI is computed
// over the archive bytes; the installer extracts entries WITHOUT ever executing anything (structural
// --ignore-scripts). The writer produces deterministic archives (fixed mode/mtime/owner) for test
// fixtures and a future `context pack` path.
//
// v1 scope (documented seam): npm's live registry serves gzip'd `.tgz` (gzip(ustar)) over HTTPS.
// The gzip-decompress + TLS/cert-pinned live-registry fetcher is a tracked follow-up; the SECURITY
// ENVELOPE (pin + integrity + scripts classification + consent gate) this module underpins is
// algorithm-agnostic and lands + is tested now over plain ustar artifacts.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace context::editor::pkg
{

struct TarEntry
{
    std::string path;  // the archived path, verbatim (e.g. "package/index.js")
    std::string data;  // file contents (empty for a directory)
    bool is_dir = false;
};

// Parse an uncompressed ustar archive. Returns std::nullopt on a malformed archive: a truncated
// header/body, a bad ustar magic, a header-checksum mismatch, or a non-octal size (fail-closed — a
// tampered archive must not partially extract).
[[nodiscard]] std::optional<std::vector<TarEntry>> tar_read(std::string_view archive);

// Serialize entries into a deterministic ustar archive (mode 0644 files / 0755 dirs, uid/gid/mtime
// 0, empty owner names), terminated by the two zero blocks. A path longer than 100 bytes that does
// not split cleanly into the ustar prefix/name fields is rejected (std::nullopt).
[[nodiscard]] std::optional<std::string> tar_write(const std::vector<TarEntry>& entries);

} // namespace context::editor::pkg

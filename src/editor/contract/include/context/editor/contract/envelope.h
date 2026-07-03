// Result envelope: the ONE machine-readable shape every CLI/RPC result takes (R-CLI-008).
//
// Every result is `{ ok, data | error, generationAfter, warnings[] }` where
// `error = { code, message, retriable, pointer?, data? }` and `code` is drawn from the versioned
// error-code catalog (error_catalog.h). Over JSON-RPC the transport-level error `data` carries the
// same `code`, so CLI and RPC diagnostics are one schema. `generationAfter` is the derived-world
// generation the write will be incorporated into (R-CLI-006); 0 for read-only / non-mutating verbs.
// exit_code() maps the envelope outcome to a process exit code via the fixed exit-code table.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::contract
{

// The `error` object inside a failed envelope. `code` MUST be a catalog code (error_catalog.h).
struct Error
{
    std::string code;
    std::string message;
    bool retriable = false;
    std::optional<std::string> pointer; // JSON-pointer into the offending file/payload, if any
    std::optional<Json> data;           // structured detail (e.g. conflict entries), if any
};

class Envelope
{
public:
    // Build a success envelope carrying `data` (default null).
    [[nodiscard]] static Envelope success(Json data = Json(),
                                          std::uint64_t generation_after = 0);

    // Build a failure envelope. message / retriable / exit-code default from the catalog entry for
    // `code`; pass a non-empty `message` to override the catalog's human template.
    [[nodiscard]] static Envelope failure(const std::string& code, std::string message = {},
                                          std::optional<std::string> pointer = std::nullopt);

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] const Json& data() const noexcept { return data_; }
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t generation_after() const noexcept { return generation_after_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    void add_warning(std::string warning) { warnings_.push_back(std::move(warning)); }
    void set_generation_after(std::uint64_t g) noexcept { generation_after_ = g; }

    // The process exit code: 0 when ok, else the catalog exit code for the error's `code`.
    [[nodiscard]] int exit_code() const;

    // Serialize to the canonical R-CLI-008 envelope JSON.
    [[nodiscard]] Json to_json() const;

    // Convenience: serialize to a string (compact when indent == 0).
    [[nodiscard]] std::string dump(int indent = 0) const { return to_json().dump(indent); }

private:
    bool ok_ = true;
    Json data_;
    std::optional<Error> error_;
    std::uint64_t generation_after_ = 0;
    std::vector<std::string> warnings_;
};

} // namespace context::editor::contract

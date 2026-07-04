// R-CLI-017 resource-handle implementation (see resource_handle.h).

#include "context/editor/contract/resource_handle.h"

#include <string>

namespace context::editor::contract
{

namespace
{
// The instance-id charset the URI format admits. Strict: anything else fails parse() — the id is
// embedded between '/' separators, so the charset excludes every URI-structural character.
bool valid_instance_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '-';
}

bool valid_instance_id(std::string_view id)
{
    if (id.empty())
        return false;
    for (const char ch : id)
    {
        if (!valid_instance_char(ch))
            return false;
    }
    return true;
}

// Parse a non-empty all-digits decimal. nullopt on empty / non-digit / overflow-ish length.
std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    if (text.empty() || text.size() > 20)
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        value = value * 10u + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}
} // namespace

std::string ResourceHandle::to_uri() const
{
    return std::string(kResourceUriScheme) + "://v0/" + instance_id + "/" +
           std::to_string(payload_id) + "?bytes=" + std::to_string(size_bytes);
}

std::optional<ResourceHandle> ResourceHandle::parse(std::string_view uri)
{
    // Scheme + version prefix, verbatim: "context-res://v0/".
    const std::string prefix = std::string(kResourceUriScheme) + "://v0/";
    if (uri.rfind(prefix, 0) != 0)
        return std::nullopt;
    std::string_view rest = uri.substr(prefix.size());

    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const std::string_view instance = rest.substr(0, slash);
    if (!valid_instance_id(instance))
        return std::nullopt;
    rest = rest.substr(slash + 1);

    const std::size_t query = rest.find("?bytes=");
    if (query == std::string_view::npos)
        return std::nullopt;
    const std::optional<std::uint64_t> payload = parse_u64(rest.substr(0, query));
    const std::optional<std::uint64_t> bytes = parse_u64(rest.substr(query + 7));
    if (!payload.has_value() || !bytes.has_value())
        return std::nullopt;

    ResourceHandle handle;
    handle.instance_id = std::string(instance);
    handle.payload_id = *payload;
    handle.size_bytes = *bytes;
    return handle;
}

Json ResourceHandle::to_json(const std::string& local_path_hint) const
{
    Json out = Json::object();
    out.set("handle", Json(to_uri()));
    out.set("sizeBytes", Json(size_bytes));
    if (!local_path_hint.empty())
        out.set("localPath", Json(local_path_hint));
    return out;
}

Json large_result_descriptor()
{
    // The R-CLI-013 self-description of the R-CLI-017 mechanism. Field SHAPES are contract; the
    // spool threshold is daemon policy (operational, not part of the wire shape).
    Json handle_shape = Json::object();
    handle_shape.set("handle", Json(std::string("string — the opaque resource URI; treat as opaque "
                                                "and feed back to resource.read verbatim")));
    handle_shape.set("sizeBytes", Json(std::string("number — total payload size in bytes")));
    handle_shape.set("localPath",
                     Json(std::string("string? — same-filesystem fast-path hint (optional; an "
                                      "optimization, never the sole mechanism)")));

    Json read_result_shape = Json::object();
    read_result_shape.set("handle", Json(std::string("string — the URI that was read")));
    read_result_shape.set("offsetBytes", Json(std::string("number — where this chunk starts")));
    read_result_shape.set("lengthBytes", Json(std::string("number — decoded bytes in this chunk")));
    read_result_shape.set("totalBytes", Json(std::string("number — total payload size")));
    read_result_shape.set("eof", Json(std::string("bool — true when offset+length == total")));
    read_result_shape.set("chunkHex",
                          Json(std::string("string — the chunk bytes, lowercase hex-encoded "
                                           "(binary-clean inside the JSON envelope)")));

    Json out = Json::object();
    out.set("uriScheme", Json(std::string(kResourceUriScheme)));
    out.set("rpcMethod", Json(std::string("resource.read")));
    out.set("cliCommand", Json(std::string("context fetch")));
    out.set("chunkEncoding", Json(std::string("hex")));
    out.set("envelopeField",
            Json(std::string("an oversized success result is replaced by data.largeResult "
                             "carrying the handle shape")));
    out.set("handleShape", std::move(handle_shape));
    out.set("readParams", Json(std::string("{ handle: string, range?: { offsetBytes: number, "
                                           "lengthBytes: number } }")));
    out.set("readResultShape", std::move(read_result_shape));
    return out;
}

} // namespace context::editor::contract

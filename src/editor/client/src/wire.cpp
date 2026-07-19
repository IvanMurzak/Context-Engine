// Wire framing + inbound-frame classification (see wire.h).

#include "context/editor/client/wire.h"

namespace context::editor::client
{

using contract::Json;

std::string build_request(std::int64_t id, const std::string& method, Json params)
{
    Json req = Json::object();
    req.set("jsonrpc", Json(std::string("2.0")));
    req.set("id", Json(id));
    req.set("method", Json(method));
    req.set("params", std::move(params));
    return req.dump(0);
}

std::optional<InboundFrame> parse_frame(const std::string& text)
{
    Json doc;
    try
    {
        doc = Json::parse(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (!doc.is_object())
        return std::nullopt;

    InboundFrame frame;

    // A notification carries `method` and no id; a response carries id + result|error.
    if (doc.contains("method") && doc.at("method").is_string())
    {
        const std::string& method = doc.at("method").as_string();
        if (method == "event")
        {
            frame.kind = FrameKind::event;
            if (doc.contains("params") && doc.at("params").is_object())
            {
                const Json& params = doc.at("params");
                if (params.contains("subId") && params.at("subId").is_string())
                    frame.sub_id = params.at("subId").as_string();
                if (params.contains("event"))
                    frame.event = params.at("event");
            }
            return frame;
        }
        if (method == "event.gap")
        {
            frame.kind = FrameKind::gap;
            return frame;
        }
        frame.kind = FrameKind::unknown; // an additive notification a newer daemon pushes
        return frame;
    }

    if (doc.contains("id"))
    {
        frame.kind = FrameKind::response;
        frame.id = doc.at("id").as_int();
        if (doc.contains("error"))
        {
            frame.has_error = true;
            const Json& err = doc.at("error");
            frame.error_message = (err.is_object() && err.contains("message") &&
                                   err.at("message").is_string())
                                      ? err.at("message").as_string()
                                      : std::string("(no message)");
            return frame;
        }
        if (doc.contains("result"))
        {
            frame.result = doc.at("result");
            return frame;
        }
        frame.has_error = true;
        frame.error_message = "response carried neither result nor error";
        return frame;
    }

    frame.kind = FrameKind::unknown;
    return frame;
}

} // namespace context::editor::client

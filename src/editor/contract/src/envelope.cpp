// Result-envelope implementation (see envelope.h).

#include "context/editor/contract/envelope.h"

#include "context/editor/contract/error_catalog.h"

#include <utility>

namespace context::editor::contract
{

Envelope Envelope::success(Json data, std::uint64_t generation_after)
{
    Envelope env;
    env.ok_ = true;
    env.data_ = std::move(data);
    env.generation_after_ = generation_after;
    return env;
}

Envelope Envelope::failure(const std::string& code, std::string message,
                           std::optional<std::string> pointer)
{
    Envelope env;
    env.ok_ = false;
    Error err;
    err.code = code;
    const ErrorCode* entry = find_code(code);
    err.message = !message.empty()  ? std::move(message)
                  : entry != nullptr ? entry->message
                                     : std::string("Unclassified error.");
    err.retriable = entry != nullptr && entry->retriable;
    err.pointer = std::move(pointer);
    env.error_ = std::move(err);
    return env;
}

int Envelope::exit_code() const
{
    if (ok_)
        return 0;
    return error_ ? exit_code_for(error_->code) : 1;
}

Json Envelope::to_json() const
{
    Json out = Json::object();
    out.set("ok", Json(ok_));
    if (ok_)
    {
        out.set("data", data_);
    }
    else if (error_)
    {
        Json err = Json::object();
        err.set("code", Json(error_->code));
        err.set("message", Json(error_->message));
        err.set("retriable", Json(error_->retriable));
        if (error_->pointer)
            err.set("pointer", Json(*error_->pointer));
        if (error_->data)
            err.set("data", *error_->data);
        out.set("error", std::move(err));
    }
    out.set("generationAfter", Json(generation_after_));
    Json warnings = Json::array();
    for (const std::string& w : warnings_)
        warnings.push_back(Json(w));
    out.set("warnings", std::move(warnings));
    return out;
}

} // namespace context::editor::contract

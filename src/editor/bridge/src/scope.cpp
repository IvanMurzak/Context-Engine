// Scope enforcement implementation (see scope.h).

#include "context/editor/bridge/scope.h"

#include <cctype>

namespace context::editor::bridge
{

namespace
{
// Bit for a non-baseline scope. read_query is the baseline (no bit) so an empty mask == read/query.
unsigned bit_for(Scope s)
{
    switch (s)
    {
    case Scope::file_write:
        return 1u << 0;
    case Scope::session_control:
        return 1u << 1;
    case Scope::build_install:
        return 1u << 2;
    case Scope::read_query:
        return 0u;
    }
    return 0u;
}

// Split a spec on commas/whitespace into lowercased tokens.
std::vector<std::string> tokenize(const std::string& spec)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : spec)
    {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}
} // namespace

ScopeSet ScopeSet::read_query()
{
    return ScopeSet{};
}

ScopeSet ScopeSet::all()
{
    ScopeSet s;
    s.grant(Scope::file_write).grant(Scope::session_control).grant(Scope::build_install);
    return s;
}

ScopeSet ScopeSet::parse(const std::string& spec)
{
    ScopeSet s;
    for (const std::string& t : tokenize(spec))
    {
        if (t == "write" || t == "file-write" || t == "file_write")
            s.grant(Scope::file_write);
        else if (t == "session" || t == "session-control" || t == "session_control")
            s.grant(Scope::session_control);
        else if (t == "build" || t == "install" || t == "build-install" || t == "build_install")
            s.grant(Scope::build_install);
        // "read"/"query"/"read-query" and any unknown token leave the read/query baseline (least
        // privilege).
    }
    return s;
}

ScopeSet& ScopeSet::grant(Scope s)
{
    mask_ |= bit_for(s);
    return *this;
}

bool ScopeSet::has(Scope s) const
{
    if (s == Scope::read_query)
        return true; // baseline, always held
    return (mask_ & bit_for(s)) != 0u;
}

ScopeSet ScopeSet::intersect(const ScopeSet& other) const
{
    ScopeSet out;
    out.mask_ = mask_ & other.mask_;
    return out;
}

std::vector<std::string> ScopeSet::names() const
{
    std::vector<std::string> out;
    out.emplace_back("read-query");
    if (has(Scope::file_write))
        out.emplace_back("file-write");
    if (has(Scope::session_control))
        out.emplace_back("session-control");
    if (has(Scope::build_install))
        out.emplace_back("build-install");
    return out;
}

Scope required_scope_for(const std::string& rpc_method)
{
    // Install/build family — "file-write is effectively code execution" (R-SEC-007).
    if (rpc_method == "package.add" || rpc_method == "build")
        return Scope::build_install;
    // File-rewriter family (R-ARCH-002 authored-file writes). `edit` is the daemon's operational
    // cross-process file-write method (the composed-loop backing behind the bridge), so it is gated
    // as a write exactly like the reserved `set` verb.
    if (rpc_method == "set" || rpc_method == "new" || rpc_method == "edit")
        return Scope::file_write;
    // Session lifecycle family (reserved verb-ids; gated now so activation is non-breaking). The
    // daemon's operational `shutdown` control method is a session-lifecycle action.
    if (rpc_method == "session.play" || rpc_method == "session.pause" ||
        rpc_method == "session.step" || rpc_method == "shutdown")
        return Scope::session_control;
    // describe, the operational `query` read, and everything else is a read/query read.
    return Scope::read_query;
}

bool authorize(const std::string& rpc_method, const ScopeSet& granted)
{
    const Scope required = required_scope_for(rpc_method);
    if (required == Scope::read_query)
        return true; // baseline is always authorized
    return granted.has(required);
}

} // namespace context::editor::bridge

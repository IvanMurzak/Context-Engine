// Material/shader authoring IR: canonical (de)serialize, variant enumeration, content hashing.
// Backend-free — no native shader toolchain is involved (R-REND-005, first slice; issue #121).

#include "context/render/material/material_ir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace context::render::material
{
namespace
{

// Split into lines, stripping a trailing '\r' from each (so a CRLF checkout hashes identically to an
// LF one — the content hash MUST be stable across OSes). A trailing empty final line (text ending in
// '\n') is dropped.
std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text)
    {
        if (c == '\n')
        {
            if (!cur.empty() && cur.back() == '\r')
            {
                cur.pop_back();
            }
            lines.push_back(cur);
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
    {
        if (cur.back() == '\r')
        {
            cur.pop_back();
        }
        lines.push_back(cur);
    }
    return lines;
}

std::string trim(std::string_view s)
{
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t'))
    {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
    {
        --end;
    }
    return std::string(s.substr(begin, end - begin));
}

std::vector<std::string> tokenize(std::string_view s)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s)
    {
        if (c == ' ' || c == '\t')
        {
            if (!cur.empty())
            {
                tokens.push_back(cur);
                cur.clear();
            }
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty())
    {
        tokens.push_back(cur);
    }
    return tokens;
}

std::string join_lines(const std::vector<std::string>& lines)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i != 0)
        {
            out += '\n';
        }
        out += lines[i];
    }
    return out;
}

} // namespace

std::string_view to_string(ShaderStageKind kind) noexcept
{
    switch (kind)
    {
    case ShaderStageKind::Vertex:
        return "vertex";
    case ShaderStageKind::Fragment:
        return "fragment";
    case ShaderStageKind::Compute:
        return "compute";
    }
    return "vertex";
}

std::optional<ShaderStageKind> stage_from_string(std::string_view name) noexcept
{
    if (name == "vertex")
    {
        return ShaderStageKind::Vertex;
    }
    if (name == "fragment")
    {
        return ShaderStageKind::Fragment;
    }
    if (name == "compute")
    {
        return ShaderStageKind::Compute;
    }
    return std::nullopt;
}

std::string VariantKey::canonical() const
{
    std::string out;
    for (std::size_t i = 0; i < defines.size(); ++i)
    {
        if (i != 0)
        {
            out += ';';
        }
        out += defines[i].first;
        out += '=';
        out += defines[i].second;
    }
    return out;
}

std::optional<ShaderIr> parse_shader(std::string_view text)
{
    ShaderIr ir;
    bool have_name = false;
    const std::vector<std::string> lines = split_lines(text);

    std::size_t i = 0;
    while (i < lines.size())
    {
        const std::string t = trim(lines[i]);
        // Blank + '#' comment lines are ignored OUTSIDE a stage body (inside a body they are authored
        // source and are preserved verbatim — the "#version 450" first line depends on this).
        if (t.empty() || t[0] == '#')
        {
            ++i;
            continue;
        }

        const std::vector<std::string> tok = tokenize(t);
        const std::string& directive = tok[0];

        if (directive == "shader")
        {
            if (tok.size() != 2)
            {
                return std::nullopt;
            }
            ir.name = tok[1];
            have_name = true;
            ++i;
        }
        else if (directive == "keyword")
        {
            if (tok.size() < 3) // name + at least one value
            {
                return std::nullopt;
            }
            ShaderKeyword k;
            k.name = tok[1];
            for (std::size_t v = 2; v < tok.size(); ++v)
            {
                k.values.push_back(tok[v]);
            }
            ir.keywords.push_back(std::move(k));
            ++i;
        }
        else if (directive == "stage")
        {
            if (tok.size() != 3)
            {
                return std::nullopt;
            }
            const std::optional<ShaderStageKind> kind = stage_from_string(tok[1]);
            if (!kind.has_value())
            {
                return std::nullopt;
            }
            ShaderStage s;
            s.kind = *kind;
            s.entry_point = tok[2];

            ++i;
            std::vector<std::string> body;
            bool closed = false;
            while (i < lines.size())
            {
                if (trim(lines[i]) == "endstage")
                {
                    closed = true;
                    ++i;
                    break;
                }
                body.push_back(lines[i]);
                ++i;
            }
            if (!closed)
            {
                return std::nullopt;
            }
            s.source = join_lines(body);
            ir.stages.push_back(std::move(s));
        }
        else
        {
            return std::nullopt; // unknown directive
        }
    }

    if (!have_name || ir.name.empty())
    {
        return std::nullopt;
    }
    return ir;
}

std::string serialize_shader(const ShaderIr& ir)
{
    std::vector<ShaderKeyword> kws = ir.keywords;
    std::sort(kws.begin(), kws.end(),
              [](const ShaderKeyword& a, const ShaderKeyword& b) { return a.name < b.name; });

    std::string out;
    out += "shader ";
    out += ir.name;
    out += '\n';

    for (const ShaderKeyword& k : kws)
    {
        out += "keyword ";
        out += k.name;
        for (const std::string& v : k.values)
        {
            out += ' ';
            out += v;
        }
        out += '\n';
    }

    for (const ShaderStage& s : ir.stages)
    {
        out += "stage ";
        out += to_string(s.kind);
        out += ' ';
        out += s.entry_point;
        out += '\n';
        if (!s.source.empty())
        {
            out += s.source;
            out += '\n';
        }
        out += "endstage\n";
    }
    return out;
}

std::vector<VariantKey> enumerate_variants(const ShaderIr& ir)
{
    std::vector<ShaderKeyword> kws;
    for (const ShaderKeyword& k : ir.keywords)
    {
        if (!k.values.empty())
        {
            kws.push_back(k);
        }
    }
    std::sort(kws.begin(), kws.end(),
              [](const ShaderKeyword& a, const ShaderKeyword& b) { return a.name < b.name; });

    if (kws.empty())
    {
        return {VariantKey{}}; // exactly one empty variant
    }

    std::vector<VariantKey> out;
    std::vector<std::size_t> idx(kws.size(), 0);
    for (;;)
    {
        VariantKey vk;
        vk.defines.reserve(kws.size());
        for (std::size_t i = 0; i < kws.size(); ++i)
        {
            vk.defines.emplace_back(kws[i].name, kws[i].values[idx[i]]);
        }
        out.push_back(std::move(vk));

        // Odometer increment: the last keyword varies fastest.
        std::size_t pos = kws.size();
        for (;;)
        {
            if (pos == 0)
            {
                return out;
            }
            --pos;
            if (++idx[pos] < kws[pos].values.size())
            {
                break;
            }
            idx[pos] = 0;
        }
    }
}

std::string content_hash_hex(std::string_view bytes)
{
    std::uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    for (char c : bytes)
    {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL; // FNV prime
    }

    constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i)
    {
        out[15 - i] = kHex[h & 0xFU];
        h >>= 4;
    }
    return out;
}

std::string ir_content_hash(const ShaderIr& ir)
{
    return content_hash_hex(serialize_shader(ir));
}

} // namespace context::render::material

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

// A keyword name/value MUST be free of every byte used as a structural delimiter while deriving the
// R-FILE-010 content-addressed cache key: ';' and '=' (VariantKey::canonical()'s delimiters) and the
// 0x1f unit separator ShaderCompileNode::cache_key() splices its components with (src/editor/derivation/
// shader_compile_node.cpp). Otherwise two distinct inputs could encode to the same key string and
// collide in the cache, silently returning the wrong artifact.
bool has_key_delimiter(std::string_view s)
{
    return s.find(';') != std::string_view::npos || s.find('=') != std::string_view::npos ||
           s.find('\x1f') != std::string_view::npos;
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

std::size_t component_count(MaterialParamType type) noexcept
{
    switch (type)
    {
    case MaterialParamType::Float:
        return 1;
    case MaterialParamType::Vec2:
        return 2;
    case MaterialParamType::Vec3:
        return 3;
    case MaterialParamType::Vec4:
        return 4;
    }
    return 1;
}

std::string_view to_string(MaterialParamType type) noexcept
{
    switch (type)
    {
    case MaterialParamType::Float:
        return "float";
    case MaterialParamType::Vec2:
        return "vec2";
    case MaterialParamType::Vec3:
        return "vec3";
    case MaterialParamType::Vec4:
        return "vec4";
    }
    return "float";
}

std::optional<MaterialParamType> param_type_from_string(std::string_view name) noexcept
{
    if (name == "float")
    {
        return MaterialParamType::Float;
    }
    if (name == "vec2")
    {
        return MaterialParamType::Vec2;
    }
    if (name == "vec3")
    {
        return MaterialParamType::Vec3;
    }
    if (name == "vec4")
    {
        return MaterialParamType::Vec4;
    }
    return std::nullopt;
}

std::string_view to_string(TextureSemantic semantic) noexcept
{
    switch (semantic)
    {
    case TextureSemantic::BaseColor:
        return "base_color";
    case TextureSemantic::MetallicRoughness:
        return "metallic_roughness";
    case TextureSemantic::Normal:
        return "normal";
    case TextureSemantic::Emissive:
        return "emissive";
    case TextureSemantic::Occlusion:
        return "occlusion";
    case TextureSemantic::Lightmap:
        return "lightmap";
    }
    return "base_color";
}

std::optional<TextureSemantic> semantic_from_string(std::string_view name) noexcept
{
    if (name == "base_color")
    {
        return TextureSemantic::BaseColor;
    }
    if (name == "metallic_roughness")
    {
        return TextureSemantic::MetallicRoughness;
    }
    if (name == "normal")
    {
        return TextureSemantic::Normal;
    }
    if (name == "emissive")
    {
        return TextureSemantic::Emissive;
    }
    if (name == "occlusion")
    {
        return TextureSemantic::Occlusion;
    }
    if (name == "lightmap")
    {
        return TextureSemantic::Lightmap;
    }
    return std::nullopt;
}

bool is_float_literal(std::string_view token) noexcept
{
    std::size_t i = 0;
    if (i < token.size() && (token[i] == '+' || token[i] == '-'))
    {
        ++i;
    }
    const auto digits = [&token, &i]
    {
        std::size_t n = 0;
        while (i < token.size() && token[i] >= '0' && token[i] <= '9')
        {
            ++i;
            ++n;
        }
        return n;
    };
    if (digits() == 0)
    {
        return false; // at least one integer digit required ("0.5", never ".5")
    }
    if (i < token.size() && token[i] == '.')
    {
        ++i;
        if (digits() == 0)
        {
            return false; // "1." is not canonical
        }
    }
    if (i < token.size() && (token[i] == 'e' || token[i] == 'E'))
    {
        ++i;
        if (i < token.size() && (token[i] == '+' || token[i] == '-'))
        {
            ++i;
        }
        if (digits() == 0)
        {
            return false;
        }
    }
    return i == token.size();
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
            if (tok.size() != 2 || has_key_delimiter(tok[1]))
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
            // Reject a name carrying a canonical-key delimiter, or a duplicate keyword name — either
            // would let two enumerated variants share one canonical (cache-key) string.
            if (has_key_delimiter(k.name))
            {
                return std::nullopt;
            }
            for (const ShaderKeyword& existing : ir.keywords)
            {
                if (existing.name == k.name)
                {
                    return std::nullopt; // duplicate keyword name
                }
            }
            for (std::size_t v = 2; v < tok.size(); ++v)
            {
                const std::string& value = tok[v];
                // Same delimiter guard for values, plus reject a duplicate value within one keyword
                // (which would enumerate the same variant twice).
                if (has_key_delimiter(value))
                {
                    return std::nullopt;
                }
                for (const std::string& existing_value : k.values)
                {
                    if (existing_value == value)
                    {
                        return std::nullopt; // duplicate value within this keyword
                    }
                }
                k.values.push_back(value);
            }
            ir.keywords.push_back(std::move(k));
            ++i;
        }
        else if (directive == "param")
        {
            // param <name> <float|vec2|vec3|vec4> <default per component> — the material contract's
            // typed parameters (defaults kept as validated float-literal TOKENS; see material_ir.h).
            if (tok.size() < 3)
            {
                return std::nullopt;
            }
            MaterialParam p;
            p.name = tok[1];
            if (has_key_delimiter(p.name))
            {
                return std::nullopt;
            }
            for (const MaterialParam& existing : ir.params)
            {
                if (existing.name == p.name)
                {
                    return std::nullopt; // duplicate parameter name
                }
            }
            const std::optional<MaterialParamType> type = param_type_from_string(tok[2]);
            if (!type.has_value())
            {
                return std::nullopt;
            }
            p.type = *type;
            if (tok.size() != 3 + component_count(p.type)) // arity must match the type
            {
                return std::nullopt;
            }
            for (std::size_t v = 3; v < tok.size(); ++v)
            {
                if (!is_float_literal(tok[v]))
                {
                    return std::nullopt;
                }
                p.defaults.push_back(tok[v]);
            }
            ir.params.push_back(std::move(p));
            ++i;
        }
        else if (directive == "texture")
        {
            // texture <name> <semantic> [uv<0..3>] — the material contract's texture slots. The
            // `lightmap` semantic + its uv channel are the R-REND-006 baked-lighting INPUT hooks.
            if (tok.size() != 3 && tok.size() != 4)
            {
                return std::nullopt;
            }
            TextureSlot slot;
            slot.name = tok[1];
            if (has_key_delimiter(slot.name))
            {
                return std::nullopt;
            }
            for (const TextureSlot& existing : ir.textures)
            {
                if (existing.name == slot.name)
                {
                    return std::nullopt; // duplicate texture-slot name
                }
            }
            const std::optional<TextureSemantic> semantic = semantic_from_string(tok[2]);
            if (!semantic.has_value())
            {
                return std::nullopt;
            }
            slot.semantic = *semantic;
            if (tok.size() == 4)
            {
                const std::string& uv = tok[3];
                // Exactly "uv<d>" with one digit 0..3 (four UV sets is the mesh-vertex bound).
                if (uv.size() != 3 || uv[0] != 'u' || uv[1] != 'v' || uv[2] < '0' || uv[2] > '3')
                {
                    return std::nullopt;
                }
                slot.uv_channel = static_cast<std::uint32_t>(uv[2] - '0');
            }
            ir.textures.push_back(std::move(slot));
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

    // The material contract, canonicalized like keywords: params then textures, each sorted by name.
    // The uv channel is always emitted explicitly ("uv0" even when defaulted) so the canonical form
    // is a parse/serialize fixed point. An empty contract emits nothing — a contract-free document
    // serializes byte-identically to the pre-contract format (hash/cache-key compatible).
    std::vector<MaterialParam> params = ir.params;
    std::sort(params.begin(), params.end(),
              [](const MaterialParam& a, const MaterialParam& b) { return a.name < b.name; });
    for (const MaterialParam& p : params)
    {
        out += "param ";
        out += p.name;
        out += ' ';
        out += to_string(p.type);
        for (const std::string& v : p.defaults)
        {
            out += ' ';
            out += v;
        }
        out += '\n';
    }

    std::vector<TextureSlot> textures = ir.textures;
    std::sort(textures.begin(), textures.end(),
              [](const TextureSlot& a, const TextureSlot& b) { return a.name < b.name; });
    for (const TextureSlot& t : textures)
    {
        out += "texture ";
        out += t.name;
        out += ' ';
        out += to_string(t.semantic);
        out += " uv";
        out += static_cast<char>('0' + (t.uv_channel & 3u));
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
    std::uint64_t h = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
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

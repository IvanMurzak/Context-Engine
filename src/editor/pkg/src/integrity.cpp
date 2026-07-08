// SRI verification (see integrity.h).

#include "context/editor/pkg/integrity.h"

#include "context/editor/filesync/sha256.h"
#include "context/editor/pkg/base64.h"
#include "context/editor/pkg/sha512.h"

#include <string>
#include <vector>

namespace context::editor::pkg
{
namespace
{

struct SriToken
{
    std::string algorithm;
    std::string base64digest;
};

// Split the SRI string on ASCII whitespace into `<alg>-<base64>` tokens. A token with no '-' (or an
// empty algorithm / empty digest) is dropped as unparseable; the caller decides Malformed vs.
// UnsupportedAlgorithm from whether ANY token survived.
std::vector<SriToken> tokenize(std::string_view sri)
{
    std::vector<SriToken> tokens;
    std::size_t i = 0;
    while (i < sri.size())
    {
        while (i < sri.size() && (sri[i] == ' ' || sri[i] == '\t' || sri[i] == '\n' ||
                                  sri[i] == '\r' || sri[i] == '\f' || sri[i] == '\v'))
            ++i;
        const std::size_t start = i;
        while (i < sri.size() && !(sri[i] == ' ' || sri[i] == '\t' || sri[i] == '\n' ||
                                   sri[i] == '\r' || sri[i] == '\f' || sri[i] == '\v'))
            ++i;
        if (i == start)
            continue;
        const std::string_view raw = sri.substr(start, i - start);
        const std::size_t dash = raw.find('-');
        if (dash == std::string_view::npos || dash == 0 || dash + 1 >= raw.size())
            continue;
        tokens.push_back({std::string(raw.substr(0, dash)), std::string(raw.substr(dash + 1))});
    }
    return tokens;
}

// Compare the byte digest of `bytes` under `algorithm` to any listed token of that algorithm.
bool matches_any(const std::vector<SriToken>& tokens, std::string_view algorithm,
                 const std::string& expected_digest)
{
    for (const SriToken& token : tokens)
    {
        if (token.algorithm != algorithm)
            continue;
        const std::optional<std::string> decoded = base64_decode(token.base64digest);
        if (!decoded.has_value())
            continue; // a malformed digest never satisfies the check (fail-closed)
        if (*decoded == expected_digest)
            return true;
    }
    return false;
}

} // namespace

SriVerification verify_integrity(std::string_view sri, std::string_view bytes)
{
    const std::vector<SriToken> tokens = tokenize(sri);
    if (tokens.empty())
        return {SriResult::Malformed, {}};

    bool has_sha512 = false;
    bool has_sha256 = false;
    for (const SriToken& token : tokens)
    {
        if (token.algorithm == "sha512")
            has_sha512 = true;
        else if (token.algorithm == "sha256")
            has_sha256 = true;
    }

    // Strongest supported algorithm present is authoritative; a weaker supported hash never
    // satisfies a stronger declared one.
    if (has_sha512)
    {
        const Sha512Digest digest = sha512(bytes);
        const std::string expected(reinterpret_cast<const char*>(digest.data()), digest.size());
        return {matches_any(tokens, "sha512", expected) ? SriResult::Ok : SriResult::Mismatch,
                "sha512"};
    }
    if (has_sha256)
    {
        const filesync::Sha256Digest digest = filesync::sha256(bytes);
        const std::string expected(reinterpret_cast<const char*>(digest.data()), digest.size());
        return {matches_any(tokens, "sha256", expected) ? SriResult::Ok : SriResult::Mismatch,
                "sha256"};
    }
    return {SriResult::UnsupportedAlgorithm, {}};
}

} // namespace context::editor::pkg

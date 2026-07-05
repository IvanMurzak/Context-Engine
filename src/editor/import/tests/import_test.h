// Tiny zero-dependency test harness for the import ctest executables (mirrors the sibling modules'
// tests/*_test.h) PLUS in-code builders that synthesize valid minimal PNG / WAV / glTF / GLB assets.
// The builders give the determinism + unit tests precise, byte-exact fixtures without committing
// binaries for every case (the committed corpus under tests/corpora/ is the fuzz-replay seed set).

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace importtest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// --- little byte-writers ------------------------------------------------------------------------

inline void put_u32be(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFFU));
    out.push_back(static_cast<char>((v >> 16) & 0xFFU));
    out.push_back(static_cast<char>((v >> 8) & 0xFFU));
    out.push_back(static_cast<char>(v & 0xFFU));
}

inline void put_u32le(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFFU));
    out.push_back(static_cast<char>((v >> 8) & 0xFFU));
    out.push_back(static_cast<char>((v >> 16) & 0xFFU));
    out.push_back(static_cast<char>((v >> 24) & 0xFFU));
}

inline void put_u16le(std::string& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFFU));
    out.push_back(static_cast<char>((v >> 8) & 0xFFU));
}

// CRC-32 (PNG chunk CRC) — a local copy for building fixtures (the importer's is internal).
inline std::uint32_t crc32(const std::string& data)
{
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready)
    {
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char ch : data)
        crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

// --- asset builders -----------------------------------------------------------------------------

inline std::string png_chunk(const char* type, const std::string& data)
{
    std::string out;
    put_u32be(out, static_cast<std::uint32_t>(data.size()));
    const std::string typed = std::string(type, 4) + data;
    out += typed;
    put_u32be(out, crc32(typed));
    return out;
}

// A valid minimal PNG (signature + IHDR + a tiny IDAT + IEND). The IDAT bytes are arbitrary — the
// importer verifies chunk CRCs + structure, not the DEFLATE stream (texel decode is the follow-up).
inline std::string make_png(std::uint32_t w, std::uint32_t h, std::uint8_t color_type = 6,
                            std::uint8_t bit_depth = 8, std::uint8_t interlace = 0,
                            bool with_srgb = false)
{
    std::string out;
    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    out.append(reinterpret_cast<const char*>(sig), 8);
    std::string ihdr;
    put_u32be(ihdr, w);
    put_u32be(ihdr, h);
    ihdr.push_back(static_cast<char>(bit_depth));
    ihdr.push_back(static_cast<char>(color_type));
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter
    ihdr.push_back(static_cast<char>(interlace));
    out += png_chunk("IHDR", ihdr);
    if (with_srgb)
        out += png_chunk("sRGB", std::string(1, '\0')); // rendering-intent byte
    out += png_chunk("IDAT", std::string("\x08\x1d\x63\x00", 4));
    out += png_chunk("IEND", std::string());
    return out;
}

// A valid minimal PCM WAV (RIFF/WAVE + fmt + data). `frames` frames of silence (deterministic).
inline std::string make_wav(std::uint16_t channels, std::uint32_t sample_rate, std::uint16_t bits,
                            std::uint32_t frames, std::uint16_t format = 1)
{
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits / 8U));
    const std::uint32_t data_size = frames * block_align;
    const std::uint32_t byte_rate = sample_rate * block_align;
    std::string fmt;
    put_u16le(fmt, format);
    put_u16le(fmt, channels);
    put_u32le(fmt, sample_rate);
    put_u32le(fmt, byte_rate);
    put_u16le(fmt, block_align);
    put_u16le(fmt, bits);
    std::string body;
    body += "WAVE";
    body += "fmt ";
    put_u32le(body, static_cast<std::uint32_t>(fmt.size()));
    body += fmt;
    body += "data";
    put_u32le(body, data_size);
    body += std::string(data_size, '\0');
    std::string out;
    out += "RIFF";
    put_u32le(out, static_cast<std::uint32_t>(body.size()));
    out += body;
    return out;
}

// A valid minimal glTF-2.0 JSON document: 1 material, 5 accessors, 1 mesh / 1 primitive with
// POSITION + NORMAL + TEXCOORD_0 (and TEXCOORD_1 when `with_uv2`), indexed (accessor 4, count 3).
inline std::string make_gltf_json(bool with_uv2)
{
    std::string attributes = "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2";
    if (with_uv2)
        attributes += ",\"TEXCOORD_1\":3";
    std::string json = "{";
    json += "\"asset\":{\"version\":\"2.0\"},";
    json += "\"materials\":[{\"name\":\"m0\"}],";
    json += "\"accessors\":[{\"count\":3},{\"count\":3},{\"count\":3},{\"count\":3},{\"count\":3}],";
    json += "\"meshes\":[{\"name\":\"Tri\",\"primitives\":[{\"attributes\":{" + attributes +
            "},\"indices\":4,\"material\":0}]}]";
    json += "}";
    return json;
}

// Wrap a glTF JSON string in a GLB binary container (12-byte header + a 4-byte-aligned JSON chunk).
inline std::string make_glb(const std::string& json)
{
    std::string padded = json;
    while (padded.size() % 4 != 0)
        padded.push_back(' '); // GLB chunks are 4-byte aligned; JSON pads with spaces
    std::string out;
    out += "glTF";
    put_u32le(out, 2); // container version
    put_u32le(out, static_cast<std::uint32_t>(12 + 8 + padded.size()));
    put_u32le(out, static_cast<std::uint32_t>(padded.size())); // chunk length
    out += "JSON";
    out += padded;
    return out;
}
} // namespace importtest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            importtest::fail(__FILE__, __LINE__, #cond);                                           \
    } while (false)

#define IMPORT_TEST_MAIN_END() return importtest::g_failures == 0 ? 0 : 1

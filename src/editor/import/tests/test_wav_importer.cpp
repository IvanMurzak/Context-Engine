// WAV importer: RIFF/WAVE parse + the descriptor + the raw-PCM payload artifact + failure paths.

#include "context/editor/import/import_settings.h"
#include "context/editor/import/importers/wav_importer.h"
#include "context/editor/import/platform_profile.h"

#include "descriptor_read.h"
#include "import_test.h"

#include <string>
#include <vector>

using namespace context::editor::import;
using context::editor::serializer::JsonValue;

namespace
{
bool has_code(const std::vector<ImportDiagnostic>& diags, const char* code)
{
    for (const ImportDiagnostic& d : diags)
        if (d.code == code)
            return true;
    return false;
}
} // namespace

int main()
{
    const WavImporter importer;
    const PlatformProfile win = *find_platform_profile("windows");
    const ImportSettings settings = resolve_import_settings("", "windows");

    // Happy path: mono 16-bit PCM, 8 frames.
    {
        const std::string wav = importtest::make_wav(1, 44100, 16, 8);
        WavInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_wav(wav, info, diags));
        CHECK(diags.empty());
        CHECK(info.format == 1);
        CHECK(info.channels == 1);
        CHECK(info.sample_rate == 44100);
        CHECK(info.bits_per_sample == 16);
        CHECK(info.frame_count == 8);
        CHECK(info.data_size == 8 * 2); // 8 frames * (1 ch * 2 bytes)

        ImportInput in;
        in.source_path = "jump.wav";
        in.source_bytes = wav;
        in.settings = settings;
        in.platform = win;
        const ImportResult r = importer.import(in);
        CHECK(r.ok);
        // Two artifacts: the descriptor + the PCM payload (WAV data IS raw PCM -> complete import).
        CHECK(r.artifacts.size() == 2);
        CHECK(r.artifacts[0].kind == ArtifactKind::audio);
        CHECK(r.artifacts[0].name == "audio");
        CHECK(r.artifacts[1].name == "audio.pcm");
        CHECK(r.artifacts[1].bytes.size() == info.data_size); // the raw PCM bytes, verbatim

        const auto desc = importtest::parse_descriptor(r.artifacts[0].bytes);
        CHECK(importtest::sfield(desc, "kind") == "audio");
        CHECK(importtest::sfield(desc, "format") == "pcm");
        CHECK(importtest::ifield(desc, "channels") == 1);
        CHECK(importtest::ifield(desc, "sampleRate") == 44100);
        CHECK(importtest::ifield(desc, "bitsPerSample") == 16);
        CHECK(importtest::ifield(desc, "frameCount") == 8);
        const JsonValue* transcode = importtest::ofield(desc, "transcode");
        CHECK(transcode != nullptr);
        CHECK(importtest::sfield(*transcode, "format") == "pcm16"); // desktop keeps uncompressed PCM
    }

    // Edge: stereo, and the Web memory-constrained platform selects a compressed transcode target.
    {
        const std::string wav = importtest::make_wav(2, 48000, 16, 4);
        WavInfo info;
        std::vector<ImportDiagnostic> diags;
        CHECK(parse_wav(wav, info, diags));
        CHECK(info.channels == 2);
        CHECK(info.frame_count == 4);

        ImportInput in;
        in.source_path = "music.wav";
        in.source_bytes = wav;
        in.settings = resolve_import_settings("", "web");
        in.platform = *find_platform_profile("web");
        const ImportResult r = importer.import(in);
        CHECK(r.ok);
        const auto desc = importtest::parse_descriptor(r.artifacts[0].bytes);
        const JsonValue* transcode = importtest::ofield(desc, "transcode");
        CHECK(importtest::sfield(*transcode, "format") == "vorbis"); // Web ceiling -> compressed
    }

    // Failure paths.
    {
        WavInfo info;
        std::vector<ImportDiagnostic> diags;

        CHECK(!parse_wav("RIFF????NOTWAVE", info, diags)); // bad WAVE tag
        CHECK(has_code(diags, "import.source_malformed"));

        // Non-PCM (IEEE float, format 3) is refused, not silently mis-imported.
        diags.clear();
        CHECK(!parse_wav(importtest::make_wav(1, 44100, 32, 4, /*format=*/3), info, diags));
        CHECK(has_code(diags, "import.unsupported_format"));

        // Truncated header.
        diags.clear();
        CHECK(!parse_wav("RIFF", info, diags));
        CHECK(has_code(diags, "import.source_malformed"));
    }

    IMPORT_TEST_MAIN_END();
}

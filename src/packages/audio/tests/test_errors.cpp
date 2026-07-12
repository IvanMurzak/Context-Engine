// The audio.* fail-closed error-code strings (M6 P6, R-SYS-006 / L-46) are the SOURCE OF TRUTH the
// contract catalog registers. This test pins the package's k*Code constants to their exact dotted
// strings (so a rename here that drifts from the catalog's audio.* block is caught), matching the
// sibling packages' test_errors. The catalog's own class/exit-code assertions live in
// src/editor/contract/tests/test_error_catalog.cpp (this package never links the contract layer).

#include "context/packages/audio/errors.h"

#include "audio_test.h"

#include <string_view>

namespace audio = context::packages::audio;

int main()
{
    CHECK(std::string_view(audio::kInvalidEntityCode) == "audio.invalid_entity");
    CHECK(std::string_view(audio::kInvalidBusCode) == "audio.invalid_bus");
    CHECK(std::string_view(audio::kInvalidEventCode) == "audio.invalid_event");
    CHECK(std::string_view(audio::kDeviceUnavailableCode) == "audio.device_unavailable");

    // All four are distinct.
    CHECK(std::string_view(audio::kInvalidEntityCode) != audio::kInvalidBusCode);
    CHECK(std::string_view(audio::kInvalidBusCode) != audio::kInvalidEventCode);
    CHECK(std::string_view(audio::kInvalidEventCode) != audio::kDeviceUnavailableCode);

    AUDIO_TEST_MAIN_END();
}

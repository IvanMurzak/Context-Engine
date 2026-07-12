// The vendored miniaudio single-header IMPLEMENTATION translation unit (third_party/miniaudio/).
//
// Compiled as its OWN static lib (context_audio_miniaudio) that does NOT link the context_warnings
// baseline and disables all warnings (this is third-party C code — see CMakeLists.txt), so miniaudio's
// ~90k-line single header compiles clean under our -Werror / /WX gate without editing upstream.
//
// Only the NULL device backend is enabled (MA_ENABLE_ONLY_SPECIFIC_BACKENDS + MA_ENABLE_NULL, set via
// target_compile_definitions): CI runners have no audio hardware and the tests must never open a real
// device (R-SYS-006 CI = null backend). The high-level engine / resource-manager / decoders / encoders
// / generators are disabled too — this package does its own float mixing on the LOW-LEVEL device API
// (audio_engine.cpp), so only the core + null device I/O compiles (smaller, faster, and no platform
// audio libraries to link). MA_IMPLEMENTATION lives ONLY here so the header is a declarations-only
// include everywhere else.

#define MA_IMPLEMENTATION
#include <miniaudio.h>

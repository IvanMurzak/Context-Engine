// The shipped RuntimeKernel host (see host.h).

#include "context/runtime/host/host.h"

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/state_hash.h"

#ifdef CONTEXT_HOST_RENDER
#include "context/render/extract.h"
#include "context/render/render_world.h"
#endif

#include <string>

namespace context::runtime::host
{

namespace content = context::runtime::content;
namespace session = context::runtime::session;

namespace
{

// DCE footprint markers (L-5 minimal-build proof). Two stable sentinel strings that the toolchain-
// agnostic render-footprint audit (render_footprint_check.cmake, file(STRINGS)) greps out of the two
// shipped binaries: the CONTROL marker is linked into EVERY flavor; the RENDER marker is linked ONLY
// into the desktop flavor (its declaration is compiled out of the server build), so its presence-in-
// desktop / absence-in-server is a deterministic tripwire on the size delta the render subsystem adds.
// They are referenced by host_signal_json below so DCE never strips them.
constexpr const char* kHostControlMarker = "CTXHOST-FOOTPRINT:runtime-host";
#ifdef CONTEXT_HOST_RENDER
constexpr const char* kHostRenderMarker = "CTXHOST-FOOTPRINT:render-present";
#endif

// The flavor this binary was linked as (a compile-time fact, not a runtime flag — R-HEAD-002: headless
// is the ABSENCE of the render subsystem, not a toggle on a present one).
#ifdef CONTEXT_HOST_RENDER
constexpr bool kRenderPresent = true;
constexpr const char* kFlavor = "desktop";
#else
constexpr bool kRenderPresent = false;
constexpr const char* kFlavor = "server";
#endif

// Minimal JSON string escaping for the scene path / diagnostic text the signal carries (the only
// string fields; every other field is a number). Escapes the two structurally-significant bytes plus
// control chars, so a scene path with a quote or backslash cannot break the one-line JSON.
[[nodiscard]] std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                out += hex[static_cast<unsigned char>(c) & 0xF];
            }
            else
            {
                out += c;
            }
            break;
        }
    }
    return out;
}

} // namespace

HostResult run_host(const HostConfig& config)
{
    HostResult result;
    result.flavor = kFlavor;
    result.render_present = kRenderPresent;

    // --- Phase 1: load the packed artifact through the RuntimeKernel content seam (a02) --------------
    // PackContentSource is total — a malformed pack leaves ok()==false with a message, never throws.
    content::PackContentSource source(config.pack_bytes);
    if (!source.ok())
    {
        result.ok = false;
        result.error_code = "host.pack_invalid";
        result.error_message = source.error();
        return result;
    }
    result.root_scene = source.root_scene();
    result.engine_version = source.engine_version();

    // Materialize every content unit into residency (the shipped-binary proof that the packed bytes
    // are runtime-loadable), then fold the feed-independent world hash over the resident world.
    content::RuntimeContentLoader loader(source);
    for (const content::UnitDescriptor& unit : source.directory())
        (void)loader.load_now(unit.unit_id);
    result.unit_count = loader.resident_unit_count();
    result.entity_count = loader.resident_entity_count();
    result.world_hash = loader.world_hash();

    // --- Phase 2: boot the shipped RuntimeKernel session and step N fixed ticks (R-BUILD-009) --------
    session::SessionConfig scfg;
    scfg.seed = config.seed;
    scfg.scenario = config.scenario;
    session::Session sim(scfg);
    const session::StepResult stepped = sim.step(config.ticks);
    result.sim_tick = stepped.sim_tick;
    result.sim_state_root = stepped.state_hash.root;

    // --- Phase 3: render subsystem (DESKTOP flavor ONLY) --------------------------------------------
    // The render module is present but headless-safe: the GPU-free sim→render extract runs (proving the
    // subsystem is linked + functional), and NO GPU device is ever created (R-HEAD-002). The server
    // flavor compiles this out entirely and never links context_render.
#ifdef CONTEXT_HOST_RENDER
    render::RenderDoubleBuffer render_buffer;
    render::extract_render_world(sim.world(), result.sim_tick, render_buffer.back());
    render_buffer.swap();
    result.render_extract_items = render_buffer.front().items.size();
#endif

    result.ok = true;
    return result;
}

std::string host_signal_json(const HostResult& result)
{
    std::string out = "{";
    out += "\"ok\":";
    out += result.ok ? "true" : "false";
    out += ",\"flavor\":\"" + json_escape(result.flavor) + "\"";
    out += ",\"renderPresent\":";
    out += result.render_present ? "true" : "false";
    if (!result.ok)
    {
        out += ",\"errorCode\":\"" + json_escape(result.error_code) + "\"";
        out += ",\"errorMessage\":\"" + json_escape(result.error_message) + "\"";
    }
    // 64-bit hashes are reported as DECIMAL STRINGS (the envelope/JSON-number precision rule the CLI
    // build path already follows for generation/packHash — a double loses precision above 2^53).
    out += ",\"rootScene\":\"" + json_escape(result.root_scene) + "\"";
    out += ",\"engineVersion\":" + std::to_string(result.engine_version);
    out += ",\"unitCount\":" + std::to_string(result.unit_count);
    out += ",\"entityCount\":" + std::to_string(result.entity_count);
    out += ",\"worldHash\":\"" + std::to_string(result.world_hash) + "\"";
    out += ",\"simTick\":" + std::to_string(result.sim_tick);
    out += ",\"simStateHash\":\"" + std::to_string(result.sim_state_root) + "\"";
    out += ",\"renderExtractItems\":" + std::to_string(result.render_extract_items);
    // The DCE footprint markers must survive into the binary's string table for render_footprint_check.
    // cmake to grep them. Each is read through a VOLATILE pointer: without it the optimizer constant-
    // folds the whole emit over the compile-time-known literal (computing the escaped result at compile
    // time and dropping the source bytes entirely — verified: both markers vanished from .rodata). A
    // volatile load makes the pointer opaque, so the bytes must live in memory. The control marker is
    // emitted for EVERY flavor; the render marker only when context_render is linked (desktop).
    const char* volatile control_marker = kHostControlMarker;
    out += ",\"footprintControl\":\"";
    out += control_marker;
    out += "\"";
#ifdef CONTEXT_HOST_RENDER
    const char* volatile render_marker = kHostRenderMarker;
    out += ",\"footprintRender\":\"";
    out += render_marker;
    out += "\"";
#endif
    out += "}";
    return out;
}

} // namespace context::runtime::host

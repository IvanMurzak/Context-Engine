// The shipped RuntimeKernel host (R-BUILD-009 / R-HEAD-001/002, M8 task a06): the runnable entrypoint
// baked into a packed Linux build. It is the FIRST standalone runtime `main()` in the engine — every
// prior executable was the `context` CLI (an editor host) or a test. It closes the a05→a06 seam: a05
// produces the v1 pack + the honest adapter stub; a06 ships the binary that BOOTS that pack.
//
// run_host is the headless smoke a R-BUILD-009 build launches against the artifact it just produced:
//   1. load the v1 pack through the RuntimeKernel content seam (PackContentSource — the a02 loader),
//      materializing every content unit and folding a feed-independent world hash over the residency;
//   2. boot a deterministic Session and step N fixed ticks against the SHIPPED RuntimeKernel (NOT the
//      editor-embedded one) — R-SIM-002 / R-QA-005;
//   3. report a machine-readable boot/state signal: the named scene reached, the resident entity/unit
//      counts + content world hash, and the post-step simTick + sim state hash.
//
// Two FLAVORS differ ONLY by whether the render subsystem is linked (R-HEAD-002 detach seam, L-5
// DCE'd minimal builds): the DESKTOP flavor links context_render and reports render present (the
// GPU-free sim→render extract runs headlessly — no GPU device is ever created); the SERVER/headless
// flavor omits context_render ENTIRELY, so the shipped binary carries zero render/GUI payload (the
// blocking size + marker audit proves it). Rendering is strictly optional and detachable — the same
// simulation loop runs identically in both.
//
// SCOPE HONESTY (R-BUILD-007): a06 proves the shipped binary (a) LOADS the packed artifact through the
// real runtime content seam and (b) RUNS the deterministic sim loop N ticks — exactly R-BUILD-009's
// "boot + step N ticks + state signal". Instantiating each packed entity INTO the live sim World (the
// content→ECS bridge) is a later task; the host reports both the content world hash (over the loaded
// pack) and the sim state hash (over the stepped session), so neither half is faked.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace context::runtime::host
{

// Everything the pure host smoke needs, supplied by the caller (main() reads the pack off disk; a test
// constructs it in memory). Total + deterministic: the same config always yields the same result.
struct HostConfig
{
    std::string pack_bytes;       // the whole v1 pack stream to boot (owned)
    std::uint64_t ticks = 0;      // fixed ticks to step the shipped RuntimeKernel (R-SIM-002)
    std::uint64_t seed = 0;       // the session seed (determinism law: reproducible from seed + input)
    std::string scenario = "demo"; // the session scenario tenant (v1: the built-in "demo")
};

// The boot/state signal a successful host run reports (R-BUILD-009). ok ⇒ the artifact booted and
// stepped; !ok ⇒ (error_code, error_message) name the fail-closed refusal (a malformed pack).
struct HostResult
{
    bool ok = false;
    std::string error_code;    // empty on ok; "host.pack_invalid" on a malformed/undecodable pack
    std::string error_message;

    std::string flavor;                // "desktop" | "server" (which subsystem set this binary carries)
    bool render_present = false;       // desktop ⇒ true (context_render linked); server ⇒ false
    std::size_t render_extract_items = 0; // desktop: drawables the GPU-free extract observed (headless)

    std::string root_scene;            // the pack's root scene — the "named scene reached" signal
    std::uint64_t engine_version = 0;  // the pack's engine version (R-FILE-010 cache-key input)
    std::size_t unit_count = 0;        // content units materialized from the pack
    std::size_t entity_count = 0;      // entities across the resident units
    std::uint64_t world_hash = 0;      // feed-independent content hash over the resident world

    std::uint64_t sim_tick = 0;        // the monotonic simTick AFTER stepping (== ticks on success)
    std::uint64_t sim_state_root = 0;  // the hierarchical sim state-hash root after stepping (R-QA-005)
};

// Boot the packed artifact and step it. Pure + deterministic + total (never throws). The render half
// is compiled in only under CONTEXT_HOST_RENDER (the desktop flavor) — the server flavor never links
// or references context_render, so its binary contains no render code (the L-5 DCE proof).
[[nodiscard]] HostResult run_host(const HostConfig& config);

// Render the boot/state signal as a compact one-line JSON object (the machine-readable R-BUILD-009
// result main() prints to stdout and the smoke gate parses). Stable key set + ordering (deterministic).
[[nodiscard]] std::string host_signal_json(const HostResult& result);

} // namespace context::runtime::host

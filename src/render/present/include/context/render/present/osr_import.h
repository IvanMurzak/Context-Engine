// OSR (off-screen rendering) texture import — the policy half of the CEF-frame -> GPU-texture seam
// (design 03 §3; L-41). The RHI's IDevice::import_external_texture is the MECHANISM and fails closed;
// this is where the per-platform DECISION lives, and it is deliberately a pure, GPU-free function so
// every platform's branch is compiled and executed by the ctest on EVERY OS (the platform-`#if` blind
// spot the local Windows dev gate otherwise has).
//
// The ratified per-platform tree, as re-scoped by the owner ruling of 2026-07-19:
//
//   * Windows — CPU UPLOAD. Accelerated DXGI shared-handle import is DEFERRED, not merely unbuilt:
//     stock wgpu-native's C API exposes no external-texture import, and carrying a patched fork was
//     rejected as an unbounded long-term maintenance cost. The upstream ask is filed as
//     gfx-rs/wgpu-native#621. The measured cost of the software path is ~114 us/frame vs ~27 us
//     zero-copy (spikes/cef-compositing FINDINGS) — accepted for the Editor on Windows only. The
//     accelerated BRANCH is kept live in this policy (and in the RHI seam) so it returns by flipping
//     one predicate once upstream lands the API, with no re-architecting.
//   * macOS — ACCELERATED via raw IOSurface, using wgpu-native's STOCK native Metal accessors
//     (wgpuDeviceGetNativeMetalDevice / wgpuQueueGetNativeMetalCommandQueue /
//     wgpuTextureGetNativeMetalTexture, shipped upstream in wgpu-native PR #557). No fork needed.
//   * Linux — CPU upload by default; dmabuf import only behind the accel gate, which SHIPS OFF.
//
// One flag (OsrImportOptions::force_software) switches accelerated <-> software everywhere, which is
// the L-41 discipline: the degrade is a decision someone made, never an accident.

#pragma once

#include "context/render/rhi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::render::present
{

// The host platform of the present path. A LOCAL enum on purpose: src/render/ must not depend on the
// editor tree, so this deliberately does not reuse editor::gui::compositor::HostPlatform. The two are
// kept in correspondence by the Shell, which owns both. `linux_` carries a trailing underscore
// because a GNU-dialect compiler predefines `linux` to 1 and would mangle the enumerator.
enum class PresentPlatform
{
    windows,
    macos,
    linux_,
};

// The compile-time host — the ONLY `#if` in this seam, and trivial. Every policy function takes the
// platform EXPLICITLY so all three branches run in the tests on every OS.
[[nodiscard]] PresentPlatform current_present_platform();

[[nodiscard]] const char* to_string(PresentPlatform platform);
[[nodiscard]] const char* to_string(ExternalTextureSource source);

struct OsrImportOptions
{
    // The L-41 surface handoff reported a shared GPU texture (accelerated_osr / iosurface). False
    // means the producer is in software OSR and there is nothing to import zero-copy in the first
    // place.
    bool shared_texture_offered = false;
    // Linux only: the Mesa/X11-ozone dmabuf accel gate. Research-grade — SHIPS OFF (03 §3).
    bool linux_dmabuf_gate = false;
    // The ONE switch that forces the software path regardless of everything above: a device loss, a
    // per-CEF-bump regression tripwire, or an operator override. L-41 discipline.
    bool force_software = false;
    // Windows only: set true once gfx-rs/wgpu-native#621 lands external-texture import in the stock
    // C API. Until then the accelerated Windows branch stays unreachable BY POLICY rather than by
    // deletion, so restoring it is a one-line change (plus the backend implementation).
    bool windows_shared_handle_import_available = false;
};

// What the policy chose, and WHY. The rationale is carried, not logged-and-lost: it is what a
// support report needs in order to explain a slow editor on a particular box.
struct OsrImportDecision
{
    ExternalTextureSource source = ExternalTextureSource::CpuBgra;
    bool accelerated = false;
    std::string rationale;
};

// Choose the import path for `platform`. Deterministic and total — never asserts, always yields a
// usable path (CpuBgra is always reachable).
[[nodiscard]] OsrImportDecision select_osr_import(PresentPlatform platform,
                                                  const OsrImportOptions& options);

// One frame as the producer hands it over. On the CPU path `pixels` are BGRA8 with PREMULTIPLIED
// alpha (what CEF's OnPaint delivers) and `dirty` names the damaged sub-rects; on an accelerated path
// only `shared_handle` + `coded_size` are meaningful.
struct OsrFrame
{
    const void* pixels = nullptr;
    std::size_t byte_size = 0;
    std::uint32_t bytes_per_row = 0;
    // The producer's FULL allocation. CEF's coded_size is >= visible_rect and is what the composite
    // pass divides by to build its UV sub-rect.
    Extent2D coded_size;
    Rect2D visible_rect;
    // Damaged sub-rects. EMPTY means "the whole visible rect" — the first paint after a resize.
    std::vector<Rect2D> dirty;
    void* shared_handle = nullptr;
};

// Clip `rect` to a `bounds`-sized image, returning an empty extent when it falls fully outside. A
// producer may report a dirty rect that a concurrent resize has already invalidated, so clipping is
// a correctness requirement, not defensive noise.
[[nodiscard]] Rect2D clip_rect(const Rect2D& rect, Extent2D bounds);

// Owns the imported texture for one OSR surface and keeps it in step with the producer's frames.
// Reimports on a coded_size change; uploads only the damaged rects on the CPU path.
class OsrTextureImporter
{
public:
    OsrTextureImporter(PresentPlatform platform, const OsrImportOptions& options);

    // Adopt `frame`. Returns false and sets diagnostic() when the frame is malformed or the import
    // failed outright. An ACCELERATED import that the backend cannot honour degrades to the CPU
    // upload path and is recorded in diagnostic() + degraded() — the degrade is always visible.
    bool update(IDevice& device, const OsrFrame& frame);

    [[nodiscard]] ITexture* texture() const { return texture_.get(); }
    [[nodiscard]] const OsrImportDecision& decision() const { return decision_; }
    // The inputs the decision was made from — what a support report needs in order to explain why a
    // particular box took a particular path.
    [[nodiscard]] PresentPlatform platform() const { return platform_; }
    [[nodiscard]] const OsrImportOptions& options() const { return options_; }
    [[nodiscard]] bool accelerated() const { return accelerated_; }
    [[nodiscard]] bool degraded() const { return degraded_; }
    [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }
    [[nodiscard]] Extent2D coded_size() const { return coded_size_; }
    // How many frames were adopted and how many sub-rect uploads they cost — the numbers that make
    // "dirty rects are cheaper than a full re-upload" an assertion instead of a claim.
    [[nodiscard]] int frame_count() const { return frame_count_; }
    [[nodiscard]] int upload_count() const { return upload_count_; }
    [[nodiscard]] int import_count() const { return import_count_; }
    // Accelerated-path per-frame refreshes (the macOS IOSurface blit) — zero on the CPU path.
    [[nodiscard]] int refresh_count() const { return refresh_count_; }

private:
    bool ensure_texture(IDevice& device, Extent2D coded_size);

    PresentPlatform platform_;
    OsrImportOptions options_;
    OsrImportDecision decision_;
    std::unique_ptr<ITexture> texture_;
    Extent2D coded_size_;
    std::string diagnostic_;
    bool accelerated_ = false;
    bool degraded_ = false;
    int frame_count_ = 0;
    int upload_count_ = 0;
    int import_count_ = 0;
    int refresh_count_ = 0;
};

} // namespace context::render::present

// OSR import policy + the dirty-rect upload driver — see osr_import.h.

#include "context/render/present/osr_import.h"

#include <algorithm>

namespace context::render::present
{
namespace
{

// The upstream issue every Windows-accelerated refusal points at, so the reason a box runs the
// software path is one search away instead of tribal knowledge.
constexpr const char* kWindowsImportIssue = "gfx-rs/wgpu-native#621";

} // namespace

PresentPlatform current_present_platform()
{
#if defined(_WIN32)
    return PresentPlatform::windows;
#elif defined(__APPLE__)
    return PresentPlatform::macos;
#else
    return PresentPlatform::linux_;
#endif
}

const char* to_string(PresentPlatform platform)
{
    switch (platform)
    {
    case PresentPlatform::windows:
        return "windows";
    case PresentPlatform::macos:
        return "macos";
    case PresentPlatform::linux_:
        break;
    }
    return "linux";
}

const char* to_string(ExternalTextureSource source)
{
    switch (source)
    {
    case ExternalTextureSource::D3D12SharedHandle:
        return "d3d12-shared-handle";
    case ExternalTextureSource::IOSurface:
        return "iosurface";
    case ExternalTextureSource::DmaBuf:
        return "dmabuf";
    case ExternalTextureSource::CpuBgra:
        break;
    }
    return "cpu-bgra";
}

OsrImportDecision select_osr_import(PresentPlatform platform, const OsrImportOptions& options)
{
    OsrImportDecision decision;
    decision.source = ExternalTextureSource::CpuBgra;

    // The single L-41 override, checked FIRST so it is unconditional: whatever the platform could
    // do, an explicit force_software wins.
    if (options.force_software)
        decision.rationale = "forced software: the L-41 accelerated/software switch is set to software";
    else if (!options.shared_texture_offered)
        decision.rationale =
            "cpu upload: the producer is in software OSR, so there is no shared texture to import";
    else
    {
        switch (platform)
        {
        case PresentPlatform::windows:
            if (options.windows_shared_handle_import_available)
            {
                decision.source = ExternalTextureSource::D3D12SharedHandle;
                decision.rationale = "accelerated: DXGI shared-handle import is available";
            }
            else
            {
                decision.rationale =
                    std::string("cpu upload: stock wgpu-native exposes no external-texture import "
                                "and the fork was rejected (owner ruling 2026-07-19); upstream ask ") +
                    kWindowsImportIssue;
            }
            break;
        case PresentPlatform::macos:
            decision.source = ExternalTextureSource::IOSurface;
            decision.rationale =
                "accelerated: raw IOSurface via wgpu-native's stock native Metal accessors";
            break;
        case PresentPlatform::linux_:
            if (options.linux_dmabuf_gate)
            {
                decision.source = ExternalTextureSource::DmaBuf;
                decision.rationale = "accelerated: the dmabuf capability gate is open";
            }
            else
            {
                decision.rationale = "cpu upload: the dmabuf accel gate is closed (ships OFF)";
            }
            break;
        }
    }
    return decision;
}

Rect2D clip_rect(const Rect2D& rect, Extent2D bounds)
{
    Rect2D out;
    if (is_empty(bounds) || rect.origin.x >= bounds.width || rect.origin.y >= bounds.height)
    {
        return out; // fully outside — a zero extent, which every caller skips
    }
    out.origin = rect.origin;
    out.size.width = std::min(rect.size.width, bounds.width - rect.origin.x);
    out.size.height = std::min(rect.size.height, bounds.height - rect.origin.y);
    return out;
}

OsrTextureImporter::OsrTextureImporter(PresentPlatform platform, const OsrImportOptions& options)
    : platform_(platform), options_(options), decision_(select_osr_import(platform, options))
{
}

void OsrTextureImporter::degrade_to_cpu_upload(std::string reason)
{
    // One degrade path for BOTH failure moments (a refused import and a failed refresh), because
    // the recovery is identical: forget the accelerated choice for good, drop the texture so the
    // next ensure_texture re-imports as CpuBgra, and say so. Rewriting decision_ is what stops a
    // degraded importer from re-attempting — and re-failing — the accelerated import on every
    // subsequent resize.
    decision_.source = ExternalTextureSource::CpuBgra;
    accelerated_ = false;
    texture_.reset();
    degrade_reason_ = std::move(reason) + " — degraded to the CPU upload path";
    diagnostic_ = degrade_reason_;
}

bool OsrTextureImporter::ensure_texture(IDevice& device, Extent2D coded_size)
{
    if (texture_ != nullptr && coded_size_.width == coded_size.width &&
        coded_size_.height == coded_size.height)
    {
        return true; // still valid — the common per-paint case allocates nothing
    }

    ExternalTextureDesc desc;
    desc.source = decision_.source;
    desc.coded_size = coded_size;

    ++import_count_;
    ExternalTexture imported = device.import_external_texture(desc);
    if (imported.texture == nullptr && decision_.accelerated())
    {
        // The accelerated path was chosen but the backend refused it. Degrade to the always-works
        // upload path and SAY SO — a silent degrade here is exactly the ~4x per-paint cost
        // regression nobody would notice until a user reports a sluggish editor.
        degrade_to_cpu_upload(std::string("accelerated import (") + to_string(decision_.source) +
                              " on " + to_string(platform_) +
                              ") refused by the backend: " + imported.diagnostic);
        ExternalTextureDesc fallback;
        fallback.source = ExternalTextureSource::CpuBgra;
        fallback.coded_size = coded_size;
        ++import_count_;
        imported = device.import_external_texture(fallback);
    }

    if (imported.texture == nullptr)
    {
        if (diagnostic_.empty())
        {
            diagnostic_ = "external-texture import failed: " + imported.diagnostic;
        }
        return false;
    }

    texture_ = std::move(imported.texture);
    coded_size_ = coded_size;
    accelerated_ = imported.accelerated;
    return true;
}

bool OsrTextureImporter::update(IDevice& device, const OsrFrame& frame)
{
    // Reset to the sticky part. A per-frame diagnostic must not outlive the frame that produced it
    // (one malformed paint would otherwise make every later support report read as if the importer
    // were still broken), but the DEGRADE reason genuinely is a standing condition — and rebuilding
    // from it each call is also what stops a repeated failure from appending without bound.
    diagnostic_ = degrade_reason_;

    if (is_empty(frame.coded_size))
    {
        diagnostic_ = "malformed OSR frame: coded_size is empty";
        return false;
    }

    // Import FIRST: only the import result says whether this is the zero-copy or the upload path,
    // so validating the CPU inputs before it would reject a perfectly good accelerated frame (which
    // legitimately carries no pixels at all).
    if (!ensure_texture(device, frame.coded_size))
    {
        return false;
    }

    ++frame_count_;

    if (accelerated_)
    {
        if (refresh_accelerated(device, frame))
        {
            return true;
        }
        // The refresh failed and degraded, which DROPPED the accelerated texture. Re-import as
        // CpuBgra before this frame's pixels can go anywhere.
        if (!ensure_texture(device, frame.coded_size))
        {
            return false;
        }
    }
    return upload_frame(device, frame);
}

// The accelerated path moves no texels over the bus — but it is not necessarily a one-shot adoption
// either: macOS hands over a NEW IOSurface per paint, blitted GPU-side into the imported texture.
// Skipping this refresh would freeze the UI on its first frame.
//
// Returns true when the frame is DONE. False means "degraded, carry on down the CPU path" — and the
// degrade is why: on macOS the import itself only allocates, so every genuine IOSurface/Metal
// problem surfaces here. Without it the importer would take this same branch and fail identically
// on every later frame, freezing the UI on the last good composite forever.
bool OsrTextureImporter::refresh_accelerated(IDevice& device, const OsrFrame& frame)
{
    ExternalTextureDesc desc;
    desc.source = decision_.source;
    desc.coded_size = coded_size_;
    desc.handle = frame.shared_handle;

    const std::string error = device.refresh_external_texture(*texture_, desc);
    if (error.empty())
    {
        ++refresh_count_;
        return true;
    }

    degrade_to_cpu_upload(std::string("accelerated refresh (") + to_string(decision_.source) +
                          " on " + to_string(platform_) + ") failed: " + error);
    return false;
}

// Upload ONE damaged sub-rect. Returns false only on a malformed frame (a rect that runs off the
// end of the producer's buffer); a rect that clips away to nothing is skipped, not an error.
bool OsrTextureImporter::upload_rect(IDevice& device, const OsrFrame& frame, const Rect2D& raw)
{
    const Rect2D rect = clip_rect(raw, frame.coded_size);
    if (is_empty(rect.size))
    {
        return true;
    }
    const std::size_t offset = static_cast<std::size_t>(rect.origin.y) * frame.bytes_per_row +
                               static_cast<std::size_t>(rect.origin.x) * 4u;
    // The rect's rows must lie inside the buffer the producer handed us — the same rule the present
    // blitters apply to a whole image, which is why it is one shared predicate.
    if (frame.byte_size < offset || !rows_fit(rect.size, frame.bytes_per_row, frame.byte_size - offset))
    {
        diagnostic_ = "malformed OSR frame: a dirty rect runs past the end of the pixel buffer";
        return false;
    }
    TexelCopyBufferLayout layout;
    // The SOURCE stride, not the rect width: the rect is a window onto the producer's frame.
    layout.bytes_per_row = frame.bytes_per_row;
    layout.rows_per_image = rect.size.height;
    device.queue().write_texture_region(*texture_, rect.origin,
                                        static_cast<const std::uint8_t*>(frame.pixels) + offset,
                                        frame.byte_size - offset, layout, rect.size);
    ++upload_count_;
    return true;
}

bool OsrTextureImporter::upload_frame(IDevice& device, const OsrFrame& frame)
{
    if (frame.pixels == nullptr)
    {
        if (degraded())
        {
            // Keep WHY we are on the CPU path (diagnostic_ already holds it) and add what to do
            // about it — a degraded producer still sending accelerated-shaped frames is the whole
            // reason this branch exists.
            diagnostic_ += "; this frame carries no pixels — resend it with its BGRA buffer";
        }
        else
        {
            diagnostic_ = "malformed OSR frame: the CPU upload path needs pixels";
        }
        return false;
    }
    if (frame.bytes_per_row < frame.coded_size.width * 4u)
    {
        diagnostic_ = "malformed OSR frame: bytes_per_row is narrower than coded_size";
        return false;
    }

    // Which rects to upload: the reported damage, else the visible rect, else the whole frame. Fed
    // to upload_rect directly rather than gathered into a vector first — the two fallbacks are a
    // single rect each, so the container would be a per-paint allocation carrying one element.
    if (!frame.dirty.empty())
    {
        for (const Rect2D& raw : frame.dirty)
        {
            if (!upload_rect(device, frame, raw))
            {
                return false;
            }
        }
        return true;
    }
    if (!is_empty(frame.visible_rect.size))
    {
        return upload_rect(device, frame, frame.visible_rect);
    }
    Rect2D whole;
    whole.size = frame.coded_size;
    return upload_rect(device, frame, whole);
}

} // namespace context::render::present

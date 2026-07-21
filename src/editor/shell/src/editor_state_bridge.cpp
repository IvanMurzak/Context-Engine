// The editor-state + region-map bridge surface (M9 e05d2) — see editor_state_bridge.h for the
// single-writer split, the untrusted-input posture, and why the collaborators are bound after
// construction rather than taken by the constructor.

#include "context/editor/shell/editor_state_bridge.h"

#include "json_number_read.h" // the shared range-guarded numeric read (float-cast-overflow UB guard)

#include <string_view>
#include <utility>

namespace context::editor::shell
{
namespace
{

using contract::Json;

// Read a NON-NEGATIVE physical-pixel field off a rect object. A missing / non-numeric / negative /
// out-of-range value reads as 0 — a region rect is an area of the client area, so a negative origin
// or extent is corruption, and clamping is the honest read (the same rule editor_state's read_u32
// applies to a hand-edited window extent). Mirrors input.h: region rects are physical client pixels.
// Region rects are UNTRUSTED renderer wire input; the double-range guard (why it must run BEFORE any
// integral cast) lives in json_number_read.h — the ONE reader editor_state.cpp shares (M9 e05d3).
[[nodiscard]] std::uint32_t read_pixel(const Json& rect, const char* key)
{
    const std::optional<double> raw = detail::number_in_range(rect, key, 0.0, 4294967295.0);
    return raw.has_value() ? static_cast<std::uint32_t>(*raw) : 0;
}

} // namespace

const char* region_kind_token(RegionKind kind)
{
    switch (kind)
    {
    case RegionKind::viewport:
        return kRegionKindViewport;
    case RegionKind::native:
        return kRegionKindNative;
    }
    // Unreachable for the closed enum; a token no side accepts is safer than a plausible-but-wrong one
    // if the enum ever grows without this switch being updated (the -Werror -Wswitch build catches
    // that at compile time first).
    return "";
}

std::optional<RegionKind> parse_region_kind(std::string_view token)
{
    if (token == kRegionKindViewport)
    {
        return RegionKind::viewport;
    }
    if (token == kRegionKindNative)
    {
        return RegionKind::native;
    }
    return std::nullopt;
}

bool parse_shell_region(const Json& element, ShellRegion& out)
{
    if (!element.is_object())
    {
        return false;
    }
    // id: a non-empty string. The publisher names each region so the Shell (and e11's viewport
    // damage path) can find one by id without re-deriving it from a naming convention.
    if (!element.contains("id") || !element.at("id").is_string())
    {
        return false;
    }
    const std::string id = element.at("id").as_string();
    if (id.empty())
    {
        return false;
    }
    // kind: a token from the closed set. An unrecognised kind is dropped rather than defaulted — a
    // renderer that means a kind the Shell does not know cannot have its input routed correctly.
    if (!element.contains("kind") || !element.at("kind").is_string())
    {
        return false;
    }
    const std::optional<RegionKind> kind = parse_region_kind(element.at("kind").as_string());
    if (!kind.has_value())
    {
        return false;
    }
    // rect: an object of non-negative physical pixels. Its absence is a malformed region — a region
    // with no area cannot be hit-tested.
    if (!element.contains("rect") || !element.at("rect").is_object())
    {
        return false;
    }
    const Json& rect = element.at("rect");

    ShellRegion region;
    region.id = id;
    region.kind = *kind;
    region.rect.origin.x = read_pixel(rect, "x");
    region.rect.origin.y = read_pixel(rect, "y");
    region.rect.size.width = read_pixel(rect, "width");
    region.rect.size.height = read_pixel(rect, "height");
    out = std::move(region);
    return true;
}

// -------------------------------------------------------------------------------- the bridge

void EditorStateBridge::bind_store(EditorStateStore* store, Clock clock)
{
    store_ = store;
    clock_ = std::move(clock);
}

void EditorStateBridge::bind_regions(RegionSink sink) { region_sink_ = std::move(sink); }

Json EditorStateBridge::snapshot() const
{
    Json out = Json::object();
    if (store_ == nullptr)
    {
        // Not wired yet: answer with an empty document rather than erroring, so a renderer that
        // raced ahead of the wiring restores nothing instead of failing its boot. In practice the
        // renderer starts only after everything is bound.
        out.set("layout", Json::object());
        out.set("panels", Json::object());
        return out;
    }
    const EditorState& state = store_->state();
    // Handed back VERBATIM — the Shell round-trips these blobs without interpreting them, which is
    // what lets editor-core evolve its layout format with no Shell change (editor_state.h states the
    // rule). A fresh project's null blob is passed through as null; the TS side treats a non-object
    // layout as "nothing to restore".
    out.set("layout", state.layout);
    out.set("panels", state.panels);
    return out;
}

bool EditorStateBridge::publish_state(const Json& params, std::string& error_code)
{
    error_code.clear();
    if (!params.is_object())
    {
        error_code = kErrEditorBadParams;
        return false;
    }
    const bool has_layout = params.contains("layout");
    const bool has_panels = params.contains("panels");
    if (!has_layout && !has_panels)
    {
        // A publish that carries neither blob is a caller bug, not an empty layout: an editor with a
        // docking root ALWAYS has a `toJSON`. Refusing it here surfaces the wiring error rather than
        // recording an empty document over the user's saved layout.
        error_code = kErrEditorBadParams;
        return false;
    }
    if (store_ == nullptr || !clock_)
    {
        error_code = kErrEditorNotReady;
        return false;
    }

    // The store applies its own identical-value short-circuit + debounce + atomic write, so a
    // stream of publishes during a drag becomes one crash-safe file per quiet period. The clock is
    // stamped once per call so both setters share the same dirty-run start.
    const std::uint64_t now = clock_();
    if (has_layout)
    {
        store_->set_layout(params.at("layout"), now);
    }
    if (has_panels)
    {
        store_->set_panels(params.at("panels"), now);
    }
    ++states_published_;
    return true;
}

bool EditorStateBridge::publish_regions(const Json& params, std::string& error_code,
                                        std::vector<ShellRegion>* out)
{
    error_code.clear();
    if (!params.is_object() || !params.contains("regions") || !params.at("regions").is_array())
    {
        // A publish with no `regions` ARRAY is malformed. An empty array is NOT — see below.
        error_code = kErrEditorBadParams;
        return false;
    }
    if (!region_sink_)
    {
        error_code = kErrEditorNotReady;
        return false;
    }

    const Json& array = params.at("regions");
    std::vector<ShellRegion> regions;
    regions.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i)
    {
        ShellRegion region;
        // A malformed ELEMENT is SKIPPED, not fatal: a region map is replaced wholesale, and one bad
        // rect from a hostile renderer must not be able to deny the Shell every valid region in the
        // same publish. An empty result (every element bad, or a legitimately empty array) still
        // publishes — that CLEARS the map, which is the correct response to "no regions this layout".
        if (parse_shell_region(array.at(i), region))
        {
            regions.push_back(std::move(region));
        }
    }
    if (out != nullptr)
    {
        *out = regions;
    }
    region_sink_(std::move(regions));
    ++regions_published_;
    return true;
}

bool EditorStateBridge::install(BridgeRouter& router)
{
    bool ok = true;

    ok = router.register_method(
             kEditorStateGetMethod,
             [this](const BridgeRequest&) -> BridgeResult
             {
                 ++state_reads_;
                 return BridgeResult::ok(snapshot());
             }) &&
         ok;

    ok = router.register_method(
             kEditorStatePublishMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::string error_code;
                 if (!publish_state(request.params, error_code))
                 {
                     return BridgeResult::error(error_code,
                                                "editor.state.publish refused: " + error_code);
                 }
                 Json out = Json::object();
                 out.set("stored", Json(true));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    ok = router.register_method(
             kEditorRegionsPublishMethod,
             [this](const BridgeRequest& request) -> BridgeResult
             {
                 std::vector<ShellRegion> published;
                 std::string error_code;
                 if (!publish_regions(request.params, error_code, &published))
                 {
                     return BridgeResult::error(error_code,
                                                "editor.regions.publish refused: " + error_code);
                 }
                 Json out = Json::object();
                 // The COUNT the Shell accepted, so the renderer can tell a fully-parsed publish from
                 // one the Shell silently dropped elements out of — a stale rect otherwise routes
                 // input to a viewport that is no longer there.
                 out.set("regions", Json(static_cast<std::int64_t>(published.size())));
                 return BridgeResult::ok(std::move(out));
             }) &&
         ok;

    return ok;
}

} // namespace context::editor::shell

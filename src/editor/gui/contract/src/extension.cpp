// R-EDIT-001 extension contract: the manifest-v2 token tables + the capability allowlist.

#include "context/editor/gui/contract/extension.h"

#include <string>

namespace context::editor::gui::contract
{

const char* contribution_kind_token(ContributionKind kind)
{
    switch (kind)
    {
    case ContributionKind::panel:
        return "panel";
    case ContributionKind::inspector:
        return "inspector";
    case ContributionKind::gizmo:
        return "gizmo";
    case ContributionKind::asset_kind_editor:
        return "asset-kind-editor";
    }
    return "panel";
}

const char* dock_zone_token(DockZone zone)
{
    switch (zone)
    {
    case DockZone::left:
        return "left";
    case DockZone::right:
        return "right";
    case DockZone::top:
        return "top";
    case DockZone::bottom:
        return "bottom";
    case DockZone::center:
        return "center";
    }
    return "center";
}

const char* content_type_token(ContentType type)
{
    switch (type)
    {
    case ContentType::uitree:
        return "uitree";
    case ContentType::iframe:
        return "iframe";
    }
    return "uitree";
}

bool capability_supported(const std::string& capability)
{
    // Deny-by-default: the closed manifest capability vocabulary (extension.h). An unknown token is
    // NOT silently dropped to a weaker grant — the registry refuses the whole contribution.
    return capability == kCapabilityReadQuery || capability == kCapabilityFileWrite ||
           capability == kCapabilitySessionControl || capability == kCapabilityBuildInstall ||
           capability == kCapabilityUiEvents;
}

} // namespace context::editor::gui::contract

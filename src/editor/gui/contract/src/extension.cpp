// R-EDIT-001 extension contract: the contribution-kind token table.

#include "context/editor/gui/contract/extension.h"

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

} // namespace context::editor::gui::contract

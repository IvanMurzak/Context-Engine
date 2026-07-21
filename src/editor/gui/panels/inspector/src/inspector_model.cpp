// Inspector view-model queries. The kernel-typed BUILDER (schema introspection intersected with the
// composed value) and the compose::WriteRequest construction moved to
// gui/panels/builders/inspector_builder.cpp (M9 e05d3, D10) so this library stays boundary-clean;
// what remains here is pure over the plain model.

#include "context/editor/gui/panels/inspector/inspector_model.h"

#include <string>

namespace context::editor::gui::panels::inspector
{

const InspectorField* find_field(const InspectorModel& model, const std::string& pointer)
{
    for (const InspectorField& field : model.fields)
    {
        if (field.pointer == pointer)
        {
            return &field;
        }
    }
    return nullptr;
}

} // namespace context::editor::gui::panels::inspector

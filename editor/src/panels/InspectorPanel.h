#pragma once

#include <VengEditor/EditorPanel.h>

namespace VengEditor
{
    // The property inspector. A stub: it shows "Nothing selected" until the
    // reflection-driven inspector wires it to a selected entity's components.
    class InspectorPanel final : public EditorPanel
    {
    public:
        [[nodiscard]] Veng::string_view Title() const override { return "Inspector"; }
        void OnImGui() override;
    };
}

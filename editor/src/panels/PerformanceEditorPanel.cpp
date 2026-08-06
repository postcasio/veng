#include "PerformanceEditorPanel.h"

#include <VengEditor/AssetEditorPanel.h>

#include <VengEditor/EditorHost.h>

#include <Veng/Renderer/Viewport.h>
#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    void PerformanceEditorPanel::OnUI()
    {
        // Resolve the scene viewport to report on: the focused document's, else the first open one.
        Renderer::Viewport* viewport = nullptr;
        if (AssetEditorPanel* focused = m_Host.GetFocusedDocument())
        {
            viewport = focused->GetDocumentViewport();
        }
        if (viewport == nullptr)
        {
            const vector<string> names = m_Host.GetSceneViewportNames();
            if (!names.empty())
            {
                viewport = m_Host.GetPanelViewport(names.front());
            }
        }

        if (viewport != nullptr)
        {
            m_Panel.Draw(*viewport);
        }
        else
        {
            UI::TextDisabled("No scene viewport open — open a prefab or level to see performance.");
        }
    }
}

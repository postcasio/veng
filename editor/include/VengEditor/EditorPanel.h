#pragma once

#include <Veng/Veng.h>
#include <Veng/UI/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;
}

namespace VengEditor
{
    // A dockable editor window. The host owns the open/close toggle, the dock id,
    // and the Window-menu wiring; a panel carries only its title and the body it
    // draws each frame inside the host-managed UI::Window scope.
    class EditorPanel
    {
    public:
        virtual ~EditorPanel() = default;

        [[nodiscard]] virtual Veng::string_view GetTitle() const = 0;
        [[nodiscard]] virtual Veng::UI::WindowFlags GetWindowFlags() const
        {
            return Veng::UI::WindowFlags::None;
        }
        virtual void OnImGui() = 0;

        // Record this frame's offscreen render (a panel that owns a SceneRenderer
        // or a preview target). The host calls it on every open panel before the
        // ImGui frame is built, so the output is sampleable when OnImGui draws it.
        // The default is a no-op; only render-owning panels override it.
        virtual void OnRender(Veng::Renderer::CommandBuffer& cmd) {}
    };
}

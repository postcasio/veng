#pragma once
#include <Veng/Veng.h>

namespace Veng
{
    class ImGuiLayer;

    // A sampled image registered with the ImGui Vulkan backend so it can be drawn
    // with ImGui::Image. Created and owned through ImGuiLayer::CreateTexture; the
    // raw descriptor set lives in Native (see Veng/Renderer/Native.h).
    class ImGuiTexture
    {
    public:
        ~ImGuiTexture();

        [[nodiscard]] u64 GetTextureId() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        ImGuiTexture(Unique<Native> native, ImGuiLayer& layer);

        Unique<Native> m_Native;
        ImGuiLayer& m_Layer;

        friend class ImGuiLayer;
    };
}

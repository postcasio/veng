#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderPass.h>

namespace Veng::Renderer
{
    struct FramebufferInfo
    {
        string Name;
        Ref<RenderPass> RenderPass;
        vector<Ref<ImageView>> Attachments = {};
        u32 Width;
        u32 Height;
        u32 Layers = 1;
    };

    class Framebuffer
    {
    public:
        static Ref<Framebuffer> Create(const FramebufferInfo& info)
        {
            return CreateRef<Framebuffer>(info);
        }

        ~Framebuffer();
        explicit Framebuffer(const FramebufferInfo& info);

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const vector<Ref<ImageView>>& GetAttachments() const { return m_Attachments; }
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Ref<RenderPass> m_RenderPass;
        string m_Name;
        vector<Ref<ImageView>> m_Attachments;
        Unique<Native> m_Native;
        uvec2 m_Extent{};
        u32 m_Layers{1};
    };
}

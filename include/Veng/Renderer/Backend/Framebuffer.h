#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/ImageView.h>
#include <Veng/Renderer/Backend/RenderPass.h>
#include <Veng/Renderer/Backend/Vulkan.h>

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
        [[nodiscard]] vk::Framebuffer GetVkFramebuffer() const { return m_VkFramebuffer; }
        [[nodiscard]] const vector<Ref<ImageView>>& GetAttachments() const { return m_Attachments; }
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }
        [[nodiscard]] u32 GetLayers() const { return m_Layers; }

    private:
        Ref<RenderPass> m_RenderPass;
        string m_Name;
        vector<Ref<ImageView>> m_Attachments;
        vk::Framebuffer m_VkFramebuffer;
        uvec2 m_Extent{};
        u32 m_Layers{1};
    };
}

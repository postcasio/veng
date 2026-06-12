#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    struct RenderPassInfo
    {
        string Name;
        vector<vk::AttachmentDescription> Attachments;
    };

    class RenderPass
    {
    public:
        static Ref<RenderPass> Create(const RenderPassInfo& info)
        {
            return CreateRef<RenderPass>(info);
        }

        explicit RenderPass(const RenderPassInfo& info);
        ~RenderPass();

        [[nodiscard]] string GetName() const { return m_Name; }
        [[nodiscard]] vk::RenderPass GetVkRenderPass() const { return m_VkRenderPass; }
        [[nodiscard]] const vector<vk::AttachmentDescription>& GetAttachments() const { return m_Attachments; }

    private:
        string m_Name;
        vk::RenderPass m_VkRenderPass;
        vector<vk::AttachmentDescription> m_Attachments;
    };
}

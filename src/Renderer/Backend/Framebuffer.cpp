#include <Veng/Renderer/Backend/Framebuffer.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    Framebuffer::~Framebuffer()
    {
        Context::Instance().GetVkDevice().destroyFramebuffer(m_VkFramebuffer);
    }

    Framebuffer::Framebuffer(const FramebufferInfo& info) : m_Name(info.Name), m_Attachments(info.Attachments),
                                                            m_Extent(info.Width, info.Height),
                                                            m_RenderPass(info.RenderPass), m_Layers(info.Layers)
    {
        vector<vk::ImageView> attachments;

        attachments.reserve(info.Attachments.size());


        for (const auto& attachment : info.Attachments)
        {
            attachments.push_back(attachment->GetVkImageView());
        }

        vk::FramebufferCreateInfo framebufferCreateInfo{
            .renderPass = info.RenderPass->GetVkRenderPass(),
            .attachmentCount = static_cast<u32>(attachments.size()),
            .pAttachments = attachments.data(),
            .width = info.Width,
            .height = info.Height,
            .layers = info.Layers
        };

        m_VkFramebuffer = Context::Instance().GetVkDevice().createFramebuffer(framebufferCreateInfo).value;

        DebugMarkers::MarkFramebuffer(m_VkFramebuffer, m_Name);
    }
}

#include <Veng/Renderer/Framebuffer.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    Framebuffer::Native& Framebuffer::GetNative() const { return *m_Native; }

    Framebuffer::~Framebuffer()
    {
        Context::Instance().GetNative().Retire(m_Native->Framebuffer);
    }

    Framebuffer::Framebuffer(const FramebufferInfo& info) : m_Name(info.Name), m_Attachments(info.Attachments),
                                                            m_Native(CreateUnique<Native>()),
                                                            m_Extent(info.Width, info.Height),
                                                            m_RenderPass(info.RenderPass), m_Layers(info.Layers)
    {
        vector<vk::ImageView> attachments;

        attachments.reserve(info.Attachments.size());


        for (const auto& attachment : info.Attachments)
        {
            attachments.push_back(attachment->GetNative().ImageView);
        }

        vk::FramebufferCreateInfo framebufferCreateInfo{
            .renderPass = info.RenderPass->GetNative().RenderPass,
            .attachmentCount = static_cast<u32>(attachments.size()),
            .pAttachments = attachments.data(),
            .width = info.Width,
            .height = info.Height,
            .layers = info.Layers
        };

        m_Native->Framebuffer = GetVkDevice(Context::Instance()).createFramebuffer(framebufferCreateInfo).value;

        DebugMarkers::MarkFramebuffer(m_Native->Framebuffer, m_Name);
    }
}

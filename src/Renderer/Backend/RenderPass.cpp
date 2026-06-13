#include <Veng/Renderer/RenderPass.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer
{
    RenderPass::Native& RenderPass::GetNative() const { return *m_Native; }

    RenderPass::RenderPass(const RenderPassInfo& info) : m_Name(info.Name), m_Native(CreateUnique<Native>()),
                                                          m_Attachments(info.Attachments)
    {
        vector<vk::AttachmentDescription> vkAttachments;
        vkAttachments.reserve(m_Attachments.size());

        for (const auto& attachment : m_Attachments)
        {
            vkAttachments.push_back({
                .format = ToVk(attachment.Format),
                .samples = vk::SampleCountFlagBits::e1,
                .loadOp = ToVk(attachment.LoadOp),
                .storeOp = ToVk(attachment.StoreOp),
                .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
                .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
                .initialLayout = ToVk(attachment.InitialLayout),
                .finalLayout = ToVk(attachment.FinalLayout),
            });
        }

        vector<vk::AttachmentReference> colorAttachmentRefs;
        vk::AttachmentReference depthAttachmentRef;
        bool hasDepth = false;

        for (u32 i = 0; i < vkAttachments.size(); i++)
        {
            const auto& attachment = vkAttachments[i];

            if (Utils::IsDepthFormat(attachment.format))
            {
                depthAttachmentRef = {
                    .attachment = i,
                    .layout = Utils::GetFormatAttachmentImageLayout(attachment.format)
                };

                hasDepth = true;
            }
            else
            {
                colorAttachmentRefs.push_back({
                    .attachment = i,
                    .layout = Utils::GetFormatAttachmentImageLayout(attachment.format)
                });
            }
        }

        constexpr vk::AttachmentReference unusedRef = {
            .attachment = VK_ATTACHMENT_UNUSED,
            .layout = vk::ImageLayout::eUndefined
        };

        const vk::SubpassDescription subpass = {
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = static_cast<u32>(colorAttachmentRefs.size()),
            .pColorAttachments = colorAttachmentRefs.data(),
            .pDepthStencilAttachment = hasDepth ? &depthAttachmentRef : &unusedRef
        };

        std::array<vk::SubpassDependency, 2> dependencies{};

        dependencies[0].srcSubpass = vk::SubpassExternal;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests |
            vk::PipelineStageFlagBits::eLateFragmentTests;
        dependencies[0].dstStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests |
            vk::PipelineStageFlagBits::eLateFragmentTests;
        dependencies[0].srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        dependencies[0].dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite |
            vk::AccessFlagBits::eDepthStencilAttachmentRead;
        dependencies[0].dependencyFlags = {};

        dependencies[1].srcSubpass = vk::SubpassExternal;
        dependencies[1].dstSubpass = 0;
        dependencies[1].srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        dependencies[1].dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        dependencies[1].srcAccessMask = {};
        dependencies[1].dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
            vk::AccessFlagBits::eColorAttachmentRead;
        dependencies[1].dependencyFlags = {};

        const vk::RenderPassCreateInfo renderPassInfo = {
            .attachmentCount = static_cast<u32>(vkAttachments.size()),
            .pAttachments = vkAttachments.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass,
            .dependencyCount = 0,
            .pDependencies = nullptr
        };

        m_Native->RenderPass = GetVkDevice(Context::Instance()).createRenderPass(renderPassInfo).value;

        DebugMarkers::MarkRenderPass(m_Native->RenderPass, m_Name);
    }

    RenderPass::~RenderPass()
    {
        Context::Instance().GetNative().Retire(m_Native->RenderPass);
    }
}

#include <Veng/Renderer/Backend/Barrier.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer::Backend
{
    static bool IsWriteAccess(const vk::AccessFlags access)
    {
        constexpr auto writes =
            vk::AccessFlagBits::eColorAttachmentWrite |
            vk::AccessFlagBits::eDepthStencilAttachmentWrite |
            vk::AccessFlagBits::eShaderWrite |
            vk::AccessFlagBits::eTransferWrite |
            vk::AccessFlagBits::eHostWrite |
            vk::AccessFlagBits::eMemoryWrite;
        return static_cast<bool>(access & writes);
    }

    void TransitionImage(CommandBuffer& cmd, Image& image,
                         const vk::ImageLayout newLayout,
                         const vk::PipelineStageFlags dstStage, const vk::AccessFlags dstAccess,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip, const u32 mipCount)
    {
        auto& native = image.GetNative();

        // The base subresource stands in for the whole declared range; views the
        // graph declares cover a range that is uniform in tracked state.
        const auto& src = native.At(baseLayer, baseMip);

        const bool layoutChange = src.Layout != newLayout;
        const bool hazard = layoutChange || IsWriteAccess(src.Access) || IsWriteAccess(dstAccess);

        if (!hazard)
        {
            // Read-after-read, same layout: no barrier needed. Widen the tracked
            // read scope so a later write waits on every prior read.
            for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
                for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
                {
                    auto& s = native.At(layer, mip);
                    s.Stage |= dstStage;
                    s.Access |= dstAccess;
                }
            return;
        }

        const vk::ImageMemoryBarrier barrier{
            .srcAccessMask = src.Access,
            .dstAccessMask = dstAccess,
            .oldLayout = src.Layout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = native.Image,
            .subresourceRange = {
                Utils::GetAspectFlags(ToVk(image.GetFormat())),
                baseMip, mipCount, baseLayer, layerCount,
            },
        };

        cmd.GetNative().CommandBuffer.pipelineBarrier(
            src.Stage, dstStage, vk::DependencyFlags{}, 0, nullptr, 0, nullptr, 1, &barrier);

        for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
            for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
            {
                auto& s = native.At(layer, mip);
                s.Layout = newLayout;
                s.Stage = dstStage;
                s.Access = dstAccess;
            }
    }

    void TransitionImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip, const u32 mipCount)
    {
        const auto vkLayout = ToVk(newLayout);
        TransitionImage(cmd, image, vkLayout,
                        Utils::GetDestinationStageMask(vkLayout),
                        Utils::GetAccessMask(vkLayout),
                        baseLayer, layerCount, baseMip, mipCount);
    }
}

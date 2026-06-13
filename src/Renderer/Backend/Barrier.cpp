#include <Veng/Renderer/Backend/Barrier.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer::Backend
{
    void TransitionImage(CommandBuffer& cmd, Image& image,
                         const vk::ImageLayout newLayout,
                         const vk::PipelineStageFlags dstStage, const vk::AccessFlags dstAccess,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip, const u32 mipCount)
    {
        auto& native = image.GetNative();

        // The base subresource stands in for the whole declared range; views the
        // graph declares cover a range that is uniform in tracked state. The pure
        // hazard rule lives in DecideBarrier (Backend/BarrierDecision.h); here we
        // only read/write the device-bound tracked state and emit the barrier.
        const auto& tracked = native.At(baseLayer, baseMip);
        const SubresourceState current{tracked.Layout, tracked.Stage, tracked.Access};

        const BarrierDecision decision = DecideBarrier(current, newLayout, dstStage, dstAccess);

        if (decision.NeedsBarrier)
        {
            const vk::ImageMemoryBarrier barrier{
                .srcAccessMask = decision.Src.Access,
                .dstAccessMask = decision.Dst.Access,
                .oldLayout = decision.Src.Layout,
                .newLayout = decision.Dst.Layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = native.Image,
                .subresourceRange = {
                    Utils::GetAspectFlags(ToVk(image.GetFormat())),
                    baseMip, mipCount, baseLayer, layerCount,
                },
            };

            cmd.GetNative().CommandBuffer.pipelineBarrier(
                decision.Src.Stage, decision.Dst.Stage,
                vk::DependencyFlags{}, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // Record the resulting state across the subresource range: a barrier resets
        // it to the desired state; a no-hazard use carries the widened read scope.
        for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
            for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
            {
                auto& s = native.At(layer, mip);
                s.Layout = decision.NewState.Layout;
                s.Stage = decision.NewState.Stage;
                s.Access = decision.NewState.Access;
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

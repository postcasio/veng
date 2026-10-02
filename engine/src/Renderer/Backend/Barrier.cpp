#include <Veng/Renderer/Backend/Barrier.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Utils.h>

namespace Veng::Renderer::Backend
{
    void TransitionImage(CommandBuffer& cmd, Image& image, const vk::ImageLayout newLayout,
                         const vk::PipelineStageFlags dstStage, const vk::AccessFlags dstAccess,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip,
                         const u32 mipCount)
    {
        BarrierBatch batch;
        batch.AddImage(cmd, image, newLayout, dstStage, dstAccess, baseLayer, layerCount, baseMip,
                       mipCount);
        batch.Record(cmd);
    }

    void BarrierBatch::AddImage(CommandBuffer& cmd, Image& image, const vk::ImageLayout newLayout,
                                const vk::PipelineStageFlags dstStage,
                                const vk::AccessFlags dstAccess, const u32 baseLayer,
                                const u32 layerCount, const u32 baseMip, const u32 mipCount)
    {
        auto& native = image.GetNative();

        // The base subresource stands in for the whole declared range; views the
        // graph declares cover a uniform-state range. The hazard rule lives in
        // DecideBarrier (Backend/BarrierDecision.h).
        const auto& tracked = native.At(baseLayer, baseMip);
        const SubresourceState current{.Layout = tracked.Layout,
                                       .Stage = tracked.Stage,
                                       .Access = tracked.Access,
                                       .ProducingFamily = tracked.ProducingFamily};

        Context& context = image.GetContext();
        const QueueFamilyIndices& families = context.GetQueueFamilies();
        const u32 transferFamily = families.TransferFamily.value_or(VK_QUEUE_FAMILY_IGNORED);
        const u32 graphicsFamily = families.GraphicsFamily.value_or(VK_QUEUE_FAMILY_IGNORED);

        // The acquire half of a queue-family ownership transfer is recorded at the
        // first graphics use of a transfer-produced subresource. When the families
        // differ the decision carries the ownership indices; when they collapse
        // (MoltenVK) it degenerates to the ordinary same-queue transition. Either
        // way, a transfer-produced subresource carries a pending transfer-timeline
        // value the frame submit must wait — folded in below.
        const BarrierDecision decision =
            DecideBarrier(current, newLayout, dstStage, dstAccess, transferFamily, graphicsFamily);

        // First graphics use of an async-uploaded subresource: fold its
        // transfer-timeline value into the next frame submit so the sample waits
        // for the copy, then clear the marker so a later use never re-waits. The
        // timeline wait is required even when the families collapse — the copy and
        // the sample are distinct submits on the (shared) queue and only the
        // timeline orders them.
        const bool consumesTransfer =
            tracked.ProducingFamily == transferFamily && tracked.PendingTransferValue != 0;

        if (consumesTransfer)
        {
            context.AddFrameTransferWait(context.GetTransferTimeline(),
                                         tracked.PendingTransferValue);
        }

        if (decision.NeedsBarrier)
        {
            // A pending barrier on an overlapping range of this image must be recorded first, or
            // one command would carry two transitions of the same subresource in no defined order.
            for (const vk::ImageMemoryBarrier& pending : m_Images)
            {
                const vk::ImageSubresourceRange& range = pending.subresourceRange;
                const bool overlaps = pending.image == native.Image &&
                                      baseMip < range.baseMipLevel + range.levelCount &&
                                      range.baseMipLevel < baseMip + mipCount &&
                                      baseLayer < range.baseArrayLayer + range.layerCount &&
                                      range.baseArrayLayer < baseLayer + layerCount;
                if (overlaps)
                {
                    Record(cmd);
                    break;
                }
            }

            m_Images.push_back(vk::ImageMemoryBarrier{
                .srcAccessMask = decision.Src.Access,
                .dstAccessMask = decision.Dst.Access,
                .oldLayout = decision.Src.Layout,
                .newLayout = decision.Dst.Layout,
                .srcQueueFamilyIndex = decision.SrcQueueFamilyIndex,
                .dstQueueFamilyIndex = decision.DstQueueFamilyIndex,
                .image = native.Image,
                .subresourceRange =
                    {
                        .aspectMask = Utils::GetAspectFlags(ToVk(image.GetFormat())),
                        .baseMipLevel = baseMip,
                        .levelCount = mipCount,
                        .baseArrayLayer = baseLayer,
                        .layerCount = layerCount,
                    },
            });
            m_SrcStages |= decision.Src.Stage;
            m_DstStages |= decision.Dst.Stage;
        }

        // Update tracked state across the range: a barrier resets it to the desired
        // state; a no-hazard read widens the scope. Clear the pending transfer value
        // once consumed so a later use never re-waits.
        for (u32 layer = baseLayer; layer < baseLayer + layerCount; layer++)
        {
            for (u32 mip = baseMip; mip < baseMip + mipCount; mip++)
            {
                auto& s = native.At(layer, mip);
                s.Layout = decision.NewState.Layout;
                s.Stage = decision.NewState.Stage;
                s.Access = decision.NewState.Access;
                s.ProducingFamily = decision.NewState.ProducingFamily;
                if (consumesTransfer)
                {
                    s.PendingTransferValue = 0;
                }
            }
        }
    }

    void BarrierBatch::AddBuffer(Buffer& buffer, const vk::PipelineStageFlags srcStage,
                                 const vk::AccessFlags srcAccess,
                                 const vk::PipelineStageFlags dstStage,
                                 const vk::AccessFlags dstAccess)
    {
        m_Buffers.push_back(vk::BufferMemoryBarrier{
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = GetVkBuffer(buffer),
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        });
        m_SrcStages |= srcStage;
        m_DstStages |= dstStage;
    }

    void BarrierBatch::Record(CommandBuffer& cmd)
    {
        if (m_Images.empty() && m_Buffers.empty())
        {
            return;
        }
        cmd.GetNative().CommandBuffer.pipelineBarrier(
            m_SrcStages, m_DstStages, vk::DependencyFlags{}, 0, nullptr,
            static_cast<u32>(m_Buffers.size()), m_Buffers.data(), static_cast<u32>(m_Images.size()),
            m_Images.data());
        m_Images.clear();
        m_Buffers.clear();
        m_SrcStages = {};
        m_DstStages = {};
    }

    void TransitionImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip,
                         const u32 mipCount)
    {
        const auto vkLayout = ToVk(newLayout);
        TransitionImage(cmd, image, vkLayout, Utils::GetDestinationStageMask(vkLayout),
                        Utils::GetAccessMask(vkLayout), baseLayer, layerCount, baseMip, mipCount);
    }

    void TransitionBuffer(CommandBuffer& cmd, Buffer& buffer, const vk::PipelineStageFlags srcStage,
                          const vk::AccessFlags srcAccess, const vk::PipelineStageFlags dstStage,
                          const vk::AccessFlags dstAccess)
    {
        BarrierBatch batch;
        batch.AddBuffer(buffer, srcStage, srcAccess, dstStage, dstAccess);
        batch.Record(cmd);
    }

    void MarkProducedOn(Image& image, const u32 producingFamily, const u64 transferValue)
    {
        auto& native = image.GetNative();
        for (u32 layer = 0; layer < native.Layers; layer++)
        {
            for (u32 mip = 0; mip < native.MipLevels; mip++)
            {
                auto& s = native.At(layer, mip);
                s.ProducingFamily = producingFamily;
                s.PendingTransferValue = transferValue;
            }
        }
    }

    void ReleaseImageToGraphicsQueue(CommandBuffer& cmd, Image& image, const u32 transferFamily,
                                     const u32 graphicsFamily)
    {
        // The single-queue collapse needs no ownership move; the acquire half is
        // likewise skipped (DecideBarrier collapses both indices to IGNORED).
        if (transferFamily == graphicsFamily)
        {
            return;
        }

        auto& native = image.GetNative();

        // Release half of the transfer→graphics ownership transfer. Both halves
        // name the same old/new layout (TransferDst → ShaderReadOnly); the acquire
        // on first graphics use completes the transition. The release makes no
        // memory available (dstStage = BottomOfPipe, dstAccess = none) — that is
        // the acquire's job. Tracked layout stays TransferDst so the acquire sees
        // the matching old layout.
        const vk::ImageMemoryBarrier barrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = {},
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = transferFamily,
            .dstQueueFamilyIndex = graphicsFamily,
            .image = native.Image,
            .subresourceRange =
                {
                    .aspectMask = Utils::GetAspectFlags(ToVk(image.GetFormat())),
                    .baseMipLevel = 0,
                    .levelCount = native.MipLevels,
                    .baseArrayLayer = 0,
                    .layerCount = native.Layers,
                },
        };

        cmd.GetNative().CommandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::DependencyFlags{}, 0, nullptr, 0, nullptr, 1, &barrier);
    }
}

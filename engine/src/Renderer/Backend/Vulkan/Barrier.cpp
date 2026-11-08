#include "Barrier.h"

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include "SyncMapping.h"
#include "Natives.h"
#include "TypeMapping.h"
#include "Utils.h"

namespace Veng::Renderer
{
    void BarrierBatch::AddImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                                const SyncStage dstStage, const SyncAccess dstAccess,
                                const u32 baseLayer, const u32 layerCount, const u32 baseMip,
                                const u32 mipCount)
    {
        auto& native = image.GetNative();

        // The base subresource stands in for the whole declared range; views the
        // graph declares cover a uniform-state range.
        const auto& tracked = native.At(baseLayer, baseMip);
        const SubresourceState current{.Layout = tracked.Layout,
                                       .Stage = tracked.Stage,
                                       .Access = tracked.Access,
                                       .ProducingFamily = tracked.ProducingFamily};

        Context& context = image.GetContext();
        const QueueFamilyIndices& families = context.GetQueueFamilies();
        const u32 transferFamily = families.TransferFamily.value_or(QueueFamilyIgnored);
        const u32 graphicsFamily = families.GraphicsFamily.value_or(QueueFamilyIgnored);

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
            for (const PendingImage& pending : m_Images)
            {
                const bool overlaps = pending.Target->GetNative().Image == native.Image &&
                                      baseMip < pending.BaseMip + pending.MipCount &&
                                      pending.BaseMip < baseMip + mipCount &&
                                      baseLayer < pending.BaseLayer + pending.LayerCount &&
                                      pending.BaseLayer < baseLayer + layerCount;
                if (overlaps)
                {
                    Record(cmd);
                    break;
                }
            }

            m_Images.push_back({
                .Target = &image,
                .Src = decision.Src,
                .Dst = decision.Dst,
                .SrcQueueFamily = decision.SrcQueueFamilyIndex,
                .DstQueueFamily = decision.DstQueueFamilyIndex,
                .BaseLayer = baseLayer,
                .LayerCount = layerCount,
                .BaseMip = baseMip,
                .MipCount = mipCount,
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

    void BarrierBatch::AddBuffer(Buffer& buffer, const SyncStage srcStage,
                                 const SyncAccess srcAccess, const SyncStage dstStage,
                                 const SyncAccess dstAccess)
    {
        m_Buffers.push_back({
            .Target = &buffer,
            .SrcStage = srcStage,
            .SrcAccess = srcAccess,
            .DstStage = dstStage,
            .DstAccess = dstAccess,
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

        // Scratch kept per thread so a replay allocates nothing once warm.
        thread_local vector<vk::ImageMemoryBarrier> images;
        thread_local vector<vk::BufferMemoryBarrier> buffers;
        images.clear();
        buffers.clear();

        for (const PendingImage& pending : m_Images)
        {
            images.push_back(vk::ImageMemoryBarrier{
                .srcAccessMask = ToVk(pending.Src.Access),
                .dstAccessMask = ToVk(pending.Dst.Access),
                .oldLayout = ToVk(pending.Src.Layout),
                .newLayout = ToVk(pending.Dst.Layout),
                .srcQueueFamilyIndex = pending.SrcQueueFamily,
                .dstQueueFamilyIndex = pending.DstQueueFamily,
                .image = pending.Target->GetNative().Image,
                .subresourceRange =
                    {
                        .aspectMask = Utils::GetAspectFlags(ToVk(pending.Target->GetFormat())),
                        .baseMipLevel = pending.BaseMip,
                        .levelCount = pending.MipCount,
                        .baseArrayLayer = pending.BaseLayer,
                        .layerCount = pending.LayerCount,
                    },
            });
        }
        for (const PendingBuffer& pending : m_Buffers)
        {
            buffers.push_back(vk::BufferMemoryBarrier{
                .srcAccessMask = ToVk(pending.SrcAccess),
                .dstAccessMask = ToVk(pending.DstAccess),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = GetVkBuffer(*pending.Target),
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            });
        }

        cmd.GetNative().CommandBuffer.pipelineBarrier(
            ToVk(m_SrcStages), ToVk(m_DstStages), vk::DependencyFlags{}, 0, nullptr,
            static_cast<u32>(buffers.size()), buffers.data(), static_cast<u32>(images.size()),
            images.data());
        m_Images.clear();
        m_Buffers.clear();
        m_SrcStages = SyncStage::None;
        m_DstStages = SyncStage::None;
    }
}

namespace Veng::Renderer::Backend
{
    void TransitionImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                         const SyncStage dstStage, const SyncAccess dstAccess, const u32 baseLayer,
                         const u32 layerCount, const u32 baseMip, const u32 mipCount)
    {
        BarrierBatch batch;
        batch.AddImage(cmd, image, newLayout, dstStage, dstAccess, baseLayer, layerCount, baseMip,
                       mipCount);
        batch.Record(cmd);
    }

    void TransitionImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                         const u32 baseLayer, const u32 layerCount, const u32 baseMip,
                         const u32 mipCount)
    {
        const SubresourceState scope = ScopeForLayout(newLayout);
        TransitionImage(cmd, image, newLayout, scope.Stage, scope.Access, baseLayer, layerCount,
                        baseMip, mipCount);
    }

    void TransitionBuffer(CommandBuffer& cmd, Buffer& buffer, const SyncStage srcStage,
                          const SyncAccess srcAccess, const SyncStage dstStage,
                          const SyncAccess dstAccess)
    {
        BarrierBatch batch;
        batch.AddBuffer(buffer, srcStage, srcAccess, dstStage, dstAccess);
        batch.Record(cmd);
    }

    void MarkProducedOn(const Image& image, const u32 producingFamily, const u64 transferValue)
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

    void ReleaseImageToGraphicsQueue(const CommandBuffer& cmd, const Image& image,
                                     const u32 transferFamily, const u32 graphicsFamily)
    {
        // The single-queue collapse needs no ownership move; the acquire half is
        // likewise skipped (DecideBarrier collapses both indices to IGNORED).
        if (transferFamily == graphicsFamily)
        {
            return;
        }

        const auto& native = image.GetNative();

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

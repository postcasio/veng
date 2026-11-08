#include "Barrier.h"

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/TimelineSemaphore.h>

#include "Natives.h"
#include "Recording.h"

namespace Veng::Renderer
{
    void BarrierBatch::AddImage(CommandBuffer& cmd, Image& image, const ImageLayout newLayout,
                                const SyncStage dstStage, const SyncAccess dstAccess,
                                const u32 baseLayer, const u32 layerCount, const u32 baseMip,
                                const u32 mipCount)
    {
        auto& native = image.GetNative();

        // The base subresource stands in for the whole declared range, as on every backend.
        const auto& tracked = native.At(baseLayer, baseMip);
        const SubresourceState current{.Layout = tracked.Layout,
                                       .Stage = tracked.Stage,
                                       .Access = tracked.Access,
                                       .ProducingFamily = tracked.ProducingFamily};

        Context& context = image.GetContext();
        const QueueFamilyIndices& families = context.GetQueueFamilies();
        const u32 transferFamily = families.TransferFamily.value_or(QueueFamilyIgnored);
        const u32 graphicsFamily = families.GraphicsFamily.value_or(QueueFamilyIgnored);
        const BarrierDecision decision =
            DecideBarrier(current, newLayout, dstStage, dstAccess, transferFamily, graphicsFamily);

        // The first use of an uploaded subresource makes the next submission wait for the upload's
        // transfer value; the upload and this use are separate submissions.
        const bool consumesTransfer =
            tracked.ProducingFamily == transferFamily && tracked.PendingTransferValue != 0;
        if (consumesTransfer)
        {
            context.AddFrameTransferWait(context.GetTransferTimeline(),
                                         tracked.PendingTransferValue);
        }

        // With no layouts, a decision that only changes layout between two reads orders nothing.
        const Metal::MetalBarrier barrier = Metal::MapBarrier(
            decision.Src.Stage, decision.Src.Access, decision.Dst.Stage, decision.Dst.Access);
        if (decision.NeedsBarrier && barrier.IsNeeded())
        {
            // A pending barrier on an overlapping range of this image is recorded first, so two
            // transitions of one subresource keep their order.
            for (const PendingImage& pending : m_Images)
            {
                const bool overlaps = pending.Target == &image &&
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
                .BaseLayer = baseLayer,
                .LayerCount = layerCount,
                .BaseMip = baseMip,
                .MipCount = mipCount,
            });
            m_SrcStages |= decision.Src.Stage;
            m_DstStages |= decision.Dst.Stage;
        }

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
        if (!Metal::MapBarrier(srcStage, srcAccess, dstStage, dstAccess).IsNeeded())
        {
            return;
        }
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
        // Metal barriers name stages, not resources, so the batch is one barrier over the union.
        Metal::Recording::Barrier(cmd, Metal::ToMetalStages(m_SrcStages, true),
                                  Metal::ToMetalStages(m_DstStages, false));
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
}

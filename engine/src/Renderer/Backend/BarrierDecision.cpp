#include <Veng/Renderer/Backend/BarrierDecision.h>

#include <Veng/Assert.h>

namespace Veng::Renderer::Backend
{
    bool IsWriteAccess(const vk::AccessFlags access)
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

    BarrierDecision DecideBarrier(const SubresourceState& current,
                                  const vk::ImageLayout newLayout,
                                  const vk::PipelineStageFlags dstStage,
                                  const vk::AccessFlags dstAccess)
    {
        const bool layoutChange = current.Layout != newLayout;
        const bool hazard = layoutChange || IsWriteAccess(current.Access) || IsWriteAccess(dstAccess);

        if (!hazard)
        {
            // Read-after-read, same layout: no barrier. Widen the tracked read
            // scope so a later write waits on every prior read; keep the layout.
            return {
                .NeedsBarrier = false,
                .NewState = {current.Layout, current.Stage | dstStage, current.Access | dstAccess},
            };
        }

        const SubresourceState desired{newLayout, dstStage, dstAccess};
        return {
            .NeedsBarrier = true,
            .NewState = desired,
            .Src = current,
            .Dst = desired,
        };
    }

    SubresourceState ScopeFor(const AccessKind kind)
    {
        using Kind = AccessKind;
        switch (kind)
        {
        case Kind::ColorAttachment:
            return {
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead,
            };
        case Kind::DepthAttachment:
            return {
                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead,
            };
        case Kind::Sample:
            return {
                vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits::eFragmentShader,
                vk::AccessFlagBits::eShaderRead,
            };
        case Kind::StorageRead:
            return {
                vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderRead,
            };
        case Kind::StorageWrite:
            return {
                vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eShaderWrite,
            };
        case Kind::TransferSrc:
            return {
                vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits::eTransfer,
                vk::AccessFlagBits::eTransferRead,
            };
        case Kind::TransferDst:
            return {
                vk::ImageLayout::eTransferDstOptimal,
                vk::PipelineStageFlagBits::eTransfer,
                vk::AccessFlagBits::eTransferWrite,
            };
        }
        VE_ASSERT(false, "unhandled AccessKind {}", static_cast<u32>(kind));
    }
}

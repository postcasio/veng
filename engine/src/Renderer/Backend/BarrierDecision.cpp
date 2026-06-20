#include <Veng/Renderer/Backend/BarrierDecision.h>

#include <Veng/Assert.h>

namespace Veng::Renderer::Backend
{
    bool IsWriteAccess(const vk::AccessFlags access)
    {
        constexpr auto writes = vk::AccessFlagBits::eColorAttachmentWrite |
                                vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                                vk::AccessFlagBits::eShaderWrite |
                                vk::AccessFlagBits::eTransferWrite |
                                vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eMemoryWrite;
        return static_cast<bool>(access & writes);
    }

    BarrierDecision DecideBarrier(const SubresourceState& current, const vk::ImageLayout newLayout,
                                  const vk::PipelineStageFlags dstStage,
                                  const vk::AccessFlags dstAccess, const u32 transferFamily,
                                  const u32 graphicsFamily)
    {
        // An acquire is needed only when the subresource was produced on the
        // transfer family and the two families are genuinely distinct. The
        // single-queue collapse (transfer == graphics) leaves both indices
        // IGNORED — an ordinary same-queue transition with no ownership move.
        const bool acquire =
            current.ProducingFamily == transferFamily && transferFamily != graphicsFamily;
        const u32 srcFamily = acquire ? transferFamily : VK_QUEUE_FAMILY_IGNORED;
        const u32 dstFamily = acquire ? graphicsFamily : VK_QUEUE_FAMILY_IGNORED;

        const bool layoutChange = current.Layout != newLayout;
        const bool hazard =
            layoutChange || IsWriteAccess(current.Access) || IsWriteAccess(dstAccess);

        if (!hazard && !acquire)
        {
            // Read-after-read, same layout, same queue: no barrier. Widen the
            // tracked read scope so a later write waits on every prior read; keep
            // the layout. The subresource stays graphics-produced.
            return {
                .NeedsBarrier = false,
                .NewState = {.Layout = current.Layout,
                             .Stage = current.Stage | dstStage,
                             .Access = current.Access | dstAccess},
            };
        }

        // After a graphics-queue use the subresource is graphics-produced, so a
        // later use never re-acquires.
        const SubresourceState desired{.Layout = newLayout,
                                       .Stage = dstStage,
                                       .Access = dstAccess,
                                       .ProducingFamily = graphicsFamily};
        return {
            .NeedsBarrier = true,
            .NewState = desired,
            .Src = current,
            .Dst = desired,
            .SrcQueueFamilyIndex = srcFamily,
            .DstQueueFamilyIndex = dstFamily,
        };
    }

    SubresourceState ScopeFor(const AccessKind kind)
    {
        using Kind = AccessKind;
        switch (kind)
        {
        case Kind::ColorAttachment:
            return {
                .Layout = vk::ImageLayout::eColorAttachmentOptimal,
                .Stage = vk::PipelineStageFlagBits::eColorAttachmentOutput,
                .Access = vk::AccessFlagBits::eColorAttachmentWrite |
                          vk::AccessFlagBits::eColorAttachmentRead,
            };
        case Kind::DepthAttachment:
            return {
                .Layout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
                .Stage = vk::PipelineStageFlagBits::eEarlyFragmentTests |
                         vk::PipelineStageFlagBits::eLateFragmentTests,
                .Access = vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits::eDepthStencilAttachmentRead,
            };
        case Kind::Sample:
            return {
                .Layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .Stage = vk::PipelineStageFlagBits::eFragmentShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        case Kind::StorageRead:
            return {
                .Layout = vk::ImageLayout::eGeneral,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        case Kind::StorageWrite:
            return {
                .Layout = vk::ImageLayout::eGeneral,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderWrite,
            };
        case Kind::TransferSrc:
            return {
                .Layout = vk::ImageLayout::eTransferSrcOptimal,
                .Stage = vk::PipelineStageFlagBits::eTransfer,
                .Access = vk::AccessFlagBits::eTransferRead,
            };
        case Kind::TransferDst:
            return {
                .Layout = vk::ImageLayout::eTransferDstOptimal,
                .Stage = vk::PipelineStageFlagBits::eTransfer,
                .Access = vk::AccessFlagBits::eTransferWrite,
            };
        // Buffer access kinds carry no layout — a buffer has none. The layout
        // stays Undefined and the buffer-barrier path uses only stage/access.
        case Kind::IndirectRead:
            return {
                .Layout = vk::ImageLayout::eUndefined,
                .Stage = vk::PipelineStageFlagBits::eDrawIndirect,
                .Access = vk::AccessFlagBits::eIndirectCommandRead,
            };
        case Kind::StorageBufferRead:
            return {
                .Layout = vk::ImageLayout::eUndefined,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        case Kind::StorageBufferWrite:
            return {
                .Layout = vk::ImageLayout::eUndefined,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderWrite,
            };
        }
        VE_ASSERT(false, "unhandled AccessKind {}", static_cast<u32>(kind));
    }
}

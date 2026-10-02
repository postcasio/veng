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
            // Read-after-read, same layout, same queue. The tracked scope is the union of every
            // read since the last write, each of which sat in the destination scope of a barrier
            // chained after that write, so a read inside that union is already ordered and
            // visible. Widen the scope either way, so a later write waits on every read.
            const SubresourceState widened{.Layout = current.Layout,
                                           .Stage = current.Stage | dstStage,
                                           .Access = current.Access | dstAccess,
                                           .ProducingFamily = current.ProducingFamily};
            const bool covered = !(dstStage & ~current.Stage) && !(dstAccess & ~current.Access);
            if (covered)
            {
                return {.NeedsBarrier = false, .NewState = widened};
            }

            // A read in a stage (or access) no earlier barrier named is unordered against the
            // last write: a barrier into fragment-shader reads does not order a later compute
            // read. The source is the earlier readers' stages rather than the write's own stage
            // — those stages are the previous barrier's destination, so this one chains after
            // it, and that chain is the only thing ordering the new read after the previous
            // barrier's layout transition, which a source naming just the writing stage would
            // not reach. That barrier already made the write available, so the source needs no
            // access; the destination makes it visible to the new read.
            return {
                .NeedsBarrier = true,
                .NewState = widened,
                .Src = {.Layout = current.Layout, .Stage = current.Stage, .Access = {}},
                .Dst = {.Layout = current.Layout, .Stage = dstStage, .Access = dstAccess},
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
        case Kind::SampleGraphics:
            return {
                .Layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .Stage = vk::PipelineStageFlagBits::eFragmentShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        case Kind::SampleCompute:
            return {
                .Layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        case Kind::SampleAny:
            return {
                .Layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .Stage = vk::PipelineStageFlagBits::eFragmentShader |
                         vk::PipelineStageFlagBits::eComputeShader,
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
        case Kind::StorageReadWrite:
            return {
                .Layout = vk::ImageLayout::eGeneral,
                .Stage = vk::PipelineStageFlagBits::eComputeShader,
                .Access = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
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
        case Kind::StorageBufferReadGraphics:
            return {
                .Layout = vk::ImageLayout::eUndefined,
                .Stage = vk::PipelineStageFlagBits::eFragmentShader,
                .Access = vk::AccessFlagBits::eShaderRead,
            };
        }
        VE_ASSERT(false, "unhandled AccessKind {}", static_cast<u32>(kind));
    }
}

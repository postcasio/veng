#include "BarrierDecision.h"

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    bool IsWriteAccess(const SyncAccess access)
    {
        constexpr auto writes = SyncAccess::ColorAttachmentWrite |
                                SyncAccess::DepthStencilAttachmentWrite | SyncAccess::ShaderWrite |
                                SyncAccess::TransferWrite | SyncAccess::HostWrite |
                                SyncAccess::MemoryWrite;
        return HasFlag(access, writes);
    }

    BarrierDecision DecideBarrier(const SubresourceState& current, const ImageLayout newLayout,
                                  const SyncStage dstStage, const SyncAccess dstAccess,
                                  const u32 transferFamily, const u32 graphicsFamily)
    {
        // An acquire is needed only when the subresource was produced on the
        // transfer family and the two families are genuinely distinct. The
        // single-queue collapse (transfer == graphics) leaves both indices
        // ignored — an ordinary same-queue transition with no ownership move.
        const bool acquire =
            current.ProducingFamily == transferFamily && transferFamily != graphicsFamily;
        const u32 srcFamily = acquire ? transferFamily : QueueFamilyIgnored;
        const u32 dstFamily = acquire ? graphicsFamily : QueueFamilyIgnored;

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
            const bool covered =
                Covers(current.Stage, dstStage) && Covers(current.Access, dstAccess);
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
                .Src = {.Layout = current.Layout, .Stage = current.Stage},
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
                .Layout = ImageLayout::ColorAttachment,
                .Stage = SyncStage::ColorAttachmentOutput,
                .Access = SyncAccess::ColorAttachmentWrite | SyncAccess::ColorAttachmentRead,
            };
        case Kind::DepthAttachment:
            return {
                .Layout = ImageLayout::DepthAttachment,
                .Stage = SyncStage::EarlyFragmentTests | SyncStage::LateFragmentTests,
                .Access = SyncAccess::DepthStencilAttachmentWrite |
                          SyncAccess::DepthStencilAttachmentRead,
            };
        case Kind::SampleGraphics:
            return {
                .Layout = ImageLayout::ShaderReadOnly,
                .Stage = SyncStage::FragmentShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::SampleCompute:
            return {
                .Layout = ImageLayout::ShaderReadOnly,
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::SampleAny:
            return {
                .Layout = ImageLayout::ShaderReadOnly,
                .Stage =
                    SyncStage::VertexShader | SyncStage::FragmentShader | SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::SampleVertexFragment:
            return {
                .Layout = ImageLayout::ShaderReadOnly,
                .Stage = SyncStage::VertexShader | SyncStage::FragmentShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::StorageRead:
            return {
                .Layout = ImageLayout::General,
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::StorageWrite:
            return {
                .Layout = ImageLayout::General,
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderWrite,
            };
        case Kind::StorageReadWrite:
            return {
                .Layout = ImageLayout::General,
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderRead | SyncAccess::ShaderWrite,
            };
        case Kind::TransferSrc:
            return {
                .Layout = ImageLayout::TransferSrc,
                .Stage = SyncStage::Transfer,
                .Access = SyncAccess::TransferRead,
            };
        case Kind::TransferDst:
            return {
                .Layout = ImageLayout::TransferDst,
                .Stage = SyncStage::Transfer,
                .Access = SyncAccess::TransferWrite,
            };
        // Buffer access kinds carry no layout — a buffer has none. The layout
        // stays Undefined and the buffer-barrier path uses only stage/access.
        case Kind::IndirectRead:
            return {
                .Stage = SyncStage::DrawIndirect,
                .Access = SyncAccess::IndirectCommandRead,
            };
        case Kind::StorageBufferRead:
            return {
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderRead,
            };
        case Kind::StorageBufferWrite:
            return {
                .Stage = SyncStage::ComputeShader,
                .Access = SyncAccess::ShaderWrite,
            };
        case Kind::StorageBufferReadGraphics:
            return {
                .Stage = SyncStage::VertexShader | SyncStage::FragmentShader,
                .Access = SyncAccess::ShaderRead,
            };
        }
        VE_ASSERT(false, "unhandled AccessKind {}", static_cast<u32>(kind));
    }

    SubresourceState ScopeForLayout(const ImageLayout layout)
    {
        switch (layout)
        {
        case ImageLayout::Undefined:
            return {.Layout = layout, .Stage = SyncStage::TopOfPipe};
        case ImageLayout::General:
            // Storage images are produced and consumed by compute; a transition with no declared
            // use covers both read and write.
            return {.Layout = layout,
                    .Stage = SyncStage::ComputeShader,
                    .Access = SyncAccess::ShaderRead | SyncAccess::ShaderWrite};
        case ImageLayout::ColorAttachment:
            return {.Layout = layout,
                    .Stage = SyncStage::ColorAttachmentOutput,
                    .Access = SyncAccess::ColorAttachmentWrite | SyncAccess::ColorAttachmentRead};
        case ImageLayout::DepthAttachment:
            return {.Layout = layout,
                    .Stage = SyncStage::EarlyFragmentTests,
                    .Access = SyncAccess::DepthStencilAttachmentWrite};
        case ImageLayout::ShaderReadOnly:
            return {.Layout = layout,
                    .Stage = SyncStage::FragmentShader,
                    .Access = SyncAccess::ShaderRead};
        case ImageLayout::TransferSrc:
            return {
                .Layout = layout, .Stage = SyncStage::Transfer, .Access = SyncAccess::TransferRead};
        case ImageLayout::TransferDst:
            return {.Layout = layout,
                    .Stage = SyncStage::Transfer,
                    .Access = SyncAccess::TransferWrite};
        case ImageLayout::PresentSrc:
            // Present is not a pipeline stage; every command is the conservative endpoint.
            return {.Layout = layout,
                    .Stage = SyncStage::AllCommands,
                    .Access = SyncAccess::MemoryRead};
        }
        VE_ASSERT(false, "unhandled ImageLayout {}", static_cast<u32>(layout));
    }
}

// Barrier-decision unit cases. DecideBarrier and ScopeFor live in
// Backend/BarrierDecision.h, separate from the device-coupled TransitionImage
// path, so the hazard rule is testable without a GPU. These are pure data →
// data; no Context, no driver.

#include <doctest/doctest.h>

#include <Veng/Renderer/Backend/BarrierDecision.h>

using namespace Veng;
using namespace Veng::Renderer;
using namespace Veng::Renderer::Backend;

namespace
{
    // Shorthand for the read-only sampled state used by several cases.
    const SubresourceState ShaderRead{
        .Layout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .Stage = vk::PipelineStageFlagBits::eFragmentShader,
        .Access = vk::AccessFlagBits::eShaderRead,
    };

    // Distinct family indices for the discrete-queue cases. On the dev box
    // (MoltenVK) these collapse to the same value, which the family-match cases
    // model with a shared index.
    constexpr u32 TransferFamily = 2;
    constexpr u32 GraphicsFamily = 0;

    // Most cases are single-queue: passing equal families leaves the queue
    // dimension inert, so the decision is the plain hazard rule.
    [[nodiscard]] BarrierDecision DecideSameQueue(const SubresourceState& current,
                                                  vk::ImageLayout newLayout,
                                                  vk::PipelineStageFlags dstStage,
                                                  vk::AccessFlags dstAccess)
    {
        return DecideBarrier(current, newLayout, dstStage, dstAccess, GraphicsFamily,
                             GraphicsFamily);
    }
}

TEST_CASE("DecideBarrier: first use from Undefined is a layout-change barrier")
{
    const SubresourceState current{
        .Layout = vk::ImageLayout::eUndefined,
        .Stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .Access = vk::AccessFlags{},
    };

    const auto d = DecideSameQueue(current, vk::ImageLayout::eColorAttachmentOptimal,
                                   vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::AccessFlagBits::eColorAttachmentWrite);

    CHECK(d.NeedsBarrier);
    CHECK(d.Src.Layout == vk::ImageLayout::eUndefined);
    CHECK(d.Dst.Layout == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(d.Dst.Stage == vk::PipelineStageFlagBits::eColorAttachmentOutput);
    CHECK(d.Dst.Access == vk::AccessFlagBits::eColorAttachmentWrite);
    // NewState is the desired state.
    CHECK(d.NewState.Layout == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(d.NewState.Stage == vk::PipelineStageFlagBits::eColorAttachmentOutput);
    CHECK(d.NewState.Access == vk::AccessFlagBits::eColorAttachmentWrite);
    // Same-queue: no ownership transfer.
    CHECK(d.SrcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(d.DstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
}

TEST_CASE("DecideBarrier: read-after-read, same layout, needs no barrier and widens scope")
{
    const auto d =
        DecideSameQueue(ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead);

    CHECK_FALSE(d.NeedsBarrier);
    // Layout unchanged; stage/access are the OR of current and desired.
    CHECK(d.NewState.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(d.NewState.Stage ==
          (vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader));
    CHECK(d.NewState.Access == vk::AccessFlagBits::eShaderRead); // read | read == read
}

TEST_CASE("DecideBarrier: read -> write at same layout is a write hazard")
{
    const auto d = DecideSameQueue(ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal,
                                   vk::PipelineStageFlagBits::eComputeShader,
                                   vk::AccessFlagBits::eShaderWrite);

    CHECK(d.NeedsBarrier); // dstAccess is a write, despite no layout change
    CHECK(d.Src.Access == vk::AccessFlagBits::eShaderRead);
    CHECK(d.Dst.Access == vk::AccessFlagBits::eShaderWrite);
    CHECK(d.NewState.Access == vk::AccessFlagBits::eShaderWrite);
}

TEST_CASE("DecideBarrier: write -> read at same layout is a source-write hazard")
{
    const SubresourceState current{
        .Layout = vk::ImageLayout::eGeneral,
        .Stage = vk::PipelineStageFlagBits::eComputeShader,
        .Access = vk::AccessFlagBits::eShaderWrite,
    };

    const auto d =
        DecideSameQueue(current, vk::ImageLayout::eGeneral,
                        vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead);

    CHECK(d.NeedsBarrier); // current access is a write
    CHECK(d.Src.Access == vk::AccessFlagBits::eShaderWrite);
    CHECK(d.Dst.Access == vk::AccessFlagBits::eShaderRead);
}

TEST_CASE("DecideBarrier: transfer-produced, families differ, acquires on first graphics use")
{
    // A texture uploaded on the transfer queue (transfer-produced, transfer-dst
    // layout) first sampled on the graphics queue with a discrete transfer
    // family present.
    const SubresourceState produced{
        .Layout = vk::ImageLayout::eTransferDstOptimal,
        .Stage = vk::PipelineStageFlagBits::eTransfer,
        .Access = vk::AccessFlagBits::eTransferWrite,
        .ProducingFamily = TransferFamily,
    };

    const auto d = DecideBarrier(produced, vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::PipelineStageFlagBits::eFragmentShader,
                                 vk::AccessFlagBits::eShaderRead, TransferFamily, GraphicsFamily);

    CHECK(d.NeedsBarrier);
    // The acquire half: transfer releases, graphics acquires.
    CHECK(d.SrcQueueFamilyIndex == TransferFamily);
    CHECK(d.DstQueueFamilyIndex == GraphicsFamily);
    // Layout/stage/access still go transfer-dst -> shader-read.
    CHECK(d.Src.Layout == vk::ImageLayout::eTransferDstOptimal);
    CHECK(d.Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(d.Dst.Stage == vk::PipelineStageFlagBits::eFragmentShader);
    CHECK(d.Dst.Access == vk::AccessFlagBits::eShaderRead);
    // After the graphics use the subresource is graphics-produced.
    CHECK(d.NewState.ProducingFamily == GraphicsFamily);
}

TEST_CASE("DecideBarrier: transfer-produced, families match, degenerates to a plain transition")
{
    // The MoltenVK single-queue collapse: transfer and graphics share a family,
    // so the same scenario carries no ownership transfer.
    const SubresourceState produced{
        .Layout = vk::ImageLayout::eTransferDstOptimal,
        .Stage = vk::PipelineStageFlagBits::eTransfer,
        .Access = vk::AccessFlagBits::eTransferWrite,
        .ProducingFamily = GraphicsFamily,
    };

    const auto d = DecideBarrier(produced, vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::PipelineStageFlagBits::eFragmentShader,
                                 vk::AccessFlagBits::eShaderRead, GraphicsFamily, GraphicsFamily);

    CHECK(d.NeedsBarrier); // still a layout change
    // No ownership transfer — both halves collapse to IGNORED.
    CHECK(d.SrcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(d.DstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(d.Src.Layout == vk::ImageLayout::eTransferDstOptimal);
    CHECK(d.Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_CASE("DecideBarrier: graphics-produced read-after-read is unchanged by the queue dimension")
{
    // Regression guard: a resource that was never on the transfer queue (default
    // graphics-produced) takes the plain hazard path even with a discrete
    // transfer family present — no acquire, no spurious barrier.
    const auto d = DecideBarrier(ShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::PipelineStageFlagBits::eComputeShader,
                                 vk::AccessFlagBits::eShaderRead, TransferFamily, GraphicsFamily);

    CHECK_FALSE(d.NeedsBarrier);
    CHECK(d.SrcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(d.DstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    CHECK(d.NewState.Stage ==
          (vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader));
}

TEST_CASE("DecideBarrier: per-mip storage write -> sampled read of the same mip is a RAW barrier")
{
    // The hi-Z reduction's core hazard: dispatch k writes mip k as a storage image
    // (General/ShaderWrite), dispatch k+1 reads that mip as a sampled source
    // (ShaderReadOnly/ShaderRead). Tracked state is per-(image, mipLevel), so this is
    // exactly the state mip k carries when the read arrives — both the source write
    // and the layout change make it a barrier.
    const SubresourceState mipWritten{
        .Layout = vk::ImageLayout::eGeneral,
        .Stage = vk::PipelineStageFlagBits::eComputeShader,
        .Access = vk::AccessFlagBits::eShaderWrite,
    };

    const auto d =
        DecideSameQueue(mipWritten, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits::eComputeShader, vk::AccessFlagBits::eShaderRead);

    CHECK(d.NeedsBarrier); // source write + layout change
    CHECK(d.Src.Layout == vk::ImageLayout::eGeneral);
    CHECK(d.Src.Access == vk::AccessFlagBits::eShaderWrite);
    CHECK(d.Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(d.Dst.Access == vk::AccessFlagBits::eShaderRead);
}

TEST_CASE("DecideBarrier: a different mip's state is tracked independently")
{
    // Per-(image, mipLevel) tracking: a mip the reduction has not yet written this
    // pass (its slot is still General/ShaderWrite from the prior frame's last write)
    // re-written this frame derives only the write-after-write barrier on its own
    // state — it never inherits a neighbouring mip's just-applied ShaderReadOnly
    // layout, so the chain's per-mip read-after-write does not spill across levels.
    const SubresourceState mipWritten{
        .Layout = vk::ImageLayout::eGeneral,
        .Stage = vk::PipelineStageFlagBits::eComputeShader,
        .Access = vk::AccessFlagBits::eShaderWrite,
    };

    const auto d = DecideSameQueue(mipWritten, vk::ImageLayout::eGeneral,
                                   vk::PipelineStageFlagBits::eComputeShader,
                                   vk::AccessFlagBits::eShaderWrite);

    CHECK(d.NeedsBarrier);                            // write-after-write on this mip
    CHECK(d.Src.Layout == vk::ImageLayout::eGeneral); // no layout change
    CHECK(d.Dst.Layout == vk::ImageLayout::eGeneral);
    CHECK(d.NewState.Access == vk::AccessFlagBits::eShaderWrite);
}

TEST_CASE("IsWriteAccess classifies write bits and reads")
{
    CHECK(IsWriteAccess(vk::AccessFlagBits::eColorAttachmentWrite));
    CHECK(IsWriteAccess(vk::AccessFlagBits::eDepthStencilAttachmentWrite));
    CHECK(IsWriteAccess(vk::AccessFlagBits::eShaderWrite));
    CHECK(IsWriteAccess(vk::AccessFlagBits::eTransferWrite));
    CHECK(IsWriteAccess(vk::AccessFlagBits::eHostWrite));
    CHECK(IsWriteAccess(vk::AccessFlagBits::eMemoryWrite));
    // A write OR'd with reads is still a write.
    CHECK(IsWriteAccess(vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite));

    CHECK_FALSE(IsWriteAccess(vk::AccessFlags{}));
    CHECK_FALSE(IsWriteAccess(vk::AccessFlagBits::eShaderRead));
    CHECK_FALSE(IsWriteAccess(vk::AccessFlagBits::eColorAttachmentRead));
    CHECK_FALSE(IsWriteAccess(vk::AccessFlagBits::eTransferRead));
}

TEST_CASE("ScopeFor maps each AccessKind to its documented scope")
{
    using Kind = AccessKind;

    const auto color = ScopeFor(Kind::ColorAttachment);
    CHECK(color.Layout == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(color.Stage == vk::PipelineStageFlagBits::eColorAttachmentOutput);
    CHECK(color.Access ==
          (vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead));

    const auto depth = ScopeFor(Kind::DepthAttachment);
    CHECK(depth.Layout == vk::ImageLayout::eDepthStencilAttachmentOptimal);
    CHECK(depth.Stage == (vk::PipelineStageFlagBits::eEarlyFragmentTests |
                          vk::PipelineStageFlagBits::eLateFragmentTests));
    CHECK(depth.Access == (vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                           vk::AccessFlagBits::eDepthStencilAttachmentRead));

    const auto sample = ScopeFor(Kind::Sample);
    CHECK(sample.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(sample.Stage == vk::PipelineStageFlagBits::eFragmentShader);
    CHECK(sample.Access == vk::AccessFlagBits::eShaderRead);

    const auto sread = ScopeFor(Kind::StorageRead);
    CHECK(sread.Layout == vk::ImageLayout::eGeneral);
    CHECK(sread.Stage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(sread.Access == vk::AccessFlagBits::eShaderRead);

    const auto swrite = ScopeFor(Kind::StorageWrite);
    CHECK(swrite.Layout == vk::ImageLayout::eGeneral);
    CHECK(swrite.Stage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(swrite.Access == vk::AccessFlagBits::eShaderWrite);

    const auto tsrc = ScopeFor(Kind::TransferSrc);
    CHECK(tsrc.Layout == vk::ImageLayout::eTransferSrcOptimal);
    CHECK(tsrc.Stage == vk::PipelineStageFlagBits::eTransfer);
    CHECK(tsrc.Access == vk::AccessFlagBits::eTransferRead);

    const auto tdst = ScopeFor(Kind::TransferDst);
    CHECK(tdst.Layout == vk::ImageLayout::eTransferDstOptimal);
    CHECK(tdst.Stage == vk::PipelineStageFlagBits::eTransfer);
    CHECK(tdst.Access == vk::AccessFlagBits::eTransferWrite);

    // Buffer access kinds carry no layout (a buffer has none): the layout stays
    // Undefined and only stage/access are meaningful.
    const auto indirect = ScopeFor(Kind::IndirectRead);
    CHECK(indirect.Layout == vk::ImageLayout::eUndefined);
    CHECK(indirect.Stage == vk::PipelineStageFlagBits::eDrawIndirect);
    CHECK(indirect.Access == vk::AccessFlagBits::eIndirectCommandRead);

    const auto sbr = ScopeFor(Kind::StorageBufferRead);
    CHECK(sbr.Layout == vk::ImageLayout::eUndefined);
    CHECK(sbr.Stage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(sbr.Access == vk::AccessFlagBits::eShaderRead);

    const auto sbw = ScopeFor(Kind::StorageBufferWrite);
    CHECK(sbw.Layout == vk::ImageLayout::eUndefined);
    CHECK(sbw.Stage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(sbw.Access == vk::AccessFlagBits::eShaderWrite);
}

TEST_CASE("Buffer hazard: compute storage-write -> indirect read is the load-bearing barrier")
{
    // The graph derives a buffer barrier from a compute pass declaring StorageBufferWrite
    // followed by a graphics pass declaring IndirectRead on the same buffer: the source
    // is the compute write, the destination the indirect-args read. A buffer carries no
    // layout, so the barrier is purely stage/access (ScopeFor + IsWriteAccess), the
    // device-free pieces the graph composes for the compute -> indirect handoff.
    const auto write = ScopeFor(AccessKind::StorageBufferWrite);
    const auto read = ScopeFor(AccessKind::IndirectRead);

    // The write is the producing scope; a hazard fires because the prior access wrote.
    CHECK(IsWriteAccess(write.Access));
    CHECK_FALSE(IsWriteAccess(read.Access));

    // The derived barrier goes eComputeShader/eShaderWrite -> eDrawIndirect/eIndirectCommandRead.
    CHECK(write.Stage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(write.Access == vk::AccessFlagBits::eShaderWrite);
    CHECK(read.Stage == vk::PipelineStageFlagBits::eDrawIndirect);
    CHECK(read.Access == vk::AccessFlagBits::eIndirectCommandRead);
}

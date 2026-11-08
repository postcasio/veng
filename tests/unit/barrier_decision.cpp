// Barrier-decision unit cases. DecideBarrier and ScopeFor live in
// Renderer/BarrierDecision.h, separate from the device-coupled TransitionImage
// path, so the hazard rule is testable without a GPU. These are pure data →
// data; no Context, no driver.

#include <doctest/doctest.h>

#include <Renderer/BarrierDecision.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Shorthand for the read-only sampled state used by several cases.
    const SubresourceState ShaderRead{
        .Layout = ImageLayout::ShaderReadOnly,
        .Stage = SyncStage::FragmentShader,
        .Access = SyncAccess::ShaderRead,
    };

    // Distinct family indices for the discrete-queue cases. On the dev box
    // (MoltenVK) these collapse to the same value, which the family-match cases
    // model with a shared index.
    constexpr u32 TransferFamily = 2;
    constexpr u32 GraphicsFamily = 0;

    // Most cases are single-queue: passing equal families leaves the queue
    // dimension inert, so the decision is the plain hazard rule.
    [[nodiscard]] BarrierDecision DecideSameQueue(const SubresourceState& current,
                                                  ImageLayout newLayout, SyncStage dstStage,
                                                  SyncAccess dstAccess)
    {
        return DecideBarrier(current, newLayout, dstStage, dstAccess, GraphicsFamily,
                             GraphicsFamily);
    }
}

TEST_CASE("DecideBarrier: first use from Undefined is a layout-change barrier")
{
    const SubresourceState current{
        .Layout = ImageLayout::Undefined,
        .Stage = SyncStage::TopOfPipe,
        .Access = SyncAccess::None,
    };

    const auto d =
        DecideSameQueue(current, ImageLayout::ColorAttachment, SyncStage::ColorAttachmentOutput,
                        SyncAccess::ColorAttachmentWrite);

    CHECK(d.NeedsBarrier);
    CHECK(d.Src.Layout == ImageLayout::Undefined);
    CHECK(d.Dst.Layout == ImageLayout::ColorAttachment);
    CHECK(d.Dst.Stage == SyncStage::ColorAttachmentOutput);
    CHECK(d.Dst.Access == SyncAccess::ColorAttachmentWrite);
    // NewState is the desired state.
    CHECK(d.NewState.Layout == ImageLayout::ColorAttachment);
    CHECK(d.NewState.Stage == SyncStage::ColorAttachmentOutput);
    CHECK(d.NewState.Access == SyncAccess::ColorAttachmentWrite);
    // Same-queue: no ownership transfer.
    CHECK(d.SrcQueueFamilyIndex == QueueFamilyIgnored);
    CHECK(d.DstQueueFamilyIndex == QueueFamilyIgnored);
}

TEST_CASE("DecideBarrier: a read inside the tracked read scope needs no barrier")
{
    const auto d = DecideSameQueue(ShaderRead, ImageLayout::ShaderReadOnly,
                                   SyncStage::FragmentShader, SyncAccess::ShaderRead);

    CHECK_FALSE(d.NeedsBarrier);
    CHECK(d.NewState.Layout == ImageLayout::ShaderReadOnly);
    CHECK(d.NewState.Stage == SyncStage::FragmentShader);
    CHECK(d.NewState.Access == SyncAccess::ShaderRead);
}

TEST_CASE("DecideBarrier: a read in a stage outside the tracked read scope needs a barrier")
{
    // The barrier that ordered the fragment read after the producing write names only the
    // fragment stage, so a compute read of the same layout is unordered against that write until
    // a barrier chains after it: source the earlier readers' stage, destination the new one.
    const auto d = DecideSameQueue(ShaderRead, ImageLayout::ShaderReadOnly,
                                   SyncStage::ComputeShader, SyncAccess::ShaderRead);

    CHECK(d.NeedsBarrier);
    CHECK(d.Src.Layout == ImageLayout::ShaderReadOnly);
    CHECK(d.Dst.Layout == ImageLayout::ShaderReadOnly); // no transition
    CHECK(HasFlag(d.Src.Stage, SyncStage::FragmentShader));
    CHECK(HasFlag(d.Dst.Stage, SyncStage::ComputeShader));
    CHECK(HasFlag(d.Dst.Access, SyncAccess::ShaderRead));
    CHECK_FALSE(IsWriteAccess(d.Src.Access));
    // The tracked scope covers both readers, so a later write waits on each.
    CHECK(d.NewState.Layout == ImageLayout::ShaderReadOnly);
    CHECK(d.NewState.Stage == (SyncStage::FragmentShader | SyncStage::ComputeShader));
    CHECK(d.NewState.Access == SyncAccess::ShaderRead);

    // Once both stages are tracked, a further read in either needs nothing.
    const auto again = DecideSameQueue(d.NewState, ImageLayout::ShaderReadOnly,
                                       SyncStage::ComputeShader, SyncAccess::ShaderRead);
    CHECK_FALSE(again.NeedsBarrier);
}

TEST_CASE("DecideBarrier: read -> write at same layout is a write hazard")
{
    const auto d = DecideSameQueue(ShaderRead, ImageLayout::ShaderReadOnly,
                                   SyncStage::ComputeShader, SyncAccess::ShaderWrite);

    CHECK(d.NeedsBarrier); // dstAccess is a write, despite no layout change
    CHECK(d.Src.Access == SyncAccess::ShaderRead);
    CHECK(d.Dst.Access == SyncAccess::ShaderWrite);
    CHECK(d.NewState.Access == SyncAccess::ShaderWrite);
}

TEST_CASE("DecideBarrier: write -> read at same layout is a source-write hazard")
{
    const SubresourceState current{
        .Layout = ImageLayout::General,
        .Stage = SyncStage::ComputeShader,
        .Access = SyncAccess::ShaderWrite,
    };

    const auto d = DecideSameQueue(current, ImageLayout::General, SyncStage::ComputeShader,
                                   SyncAccess::ShaderRead);

    CHECK(d.NeedsBarrier); // current access is a write
    CHECK(d.Src.Access == SyncAccess::ShaderWrite);
    CHECK(d.Dst.Access == SyncAccess::ShaderRead);
}

TEST_CASE("DecideBarrier: read-write -> read-write makes the earlier writes visible to reads")
{
    // Two passes of atomics on one storage image: the second must see what the first stored.
    const auto rw = ScopeFor(AccessKind::StorageReadWrite);
    const auto d = DecideSameQueue(rw, rw.Layout, rw.Stage, rw.Access);

    CHECK(d.NeedsBarrier);
    CHECK((d.Src.Access & SyncAccess::ShaderWrite) == SyncAccess::ShaderWrite);
    CHECK((d.Dst.Access & SyncAccess::ShaderRead) == SyncAccess::ShaderRead);
    CHECK(d.Dst.Layout == ImageLayout::General);
}

TEST_CASE("DecideBarrier: transfer-produced, families differ, acquires on first graphics use")
{
    // A texture uploaded on the transfer queue (transfer-produced, transfer-dst
    // layout) first sampled on the graphics queue with a discrete transfer
    // family present.
    const SubresourceState produced{
        .Layout = ImageLayout::TransferDst,
        .Stage = SyncStage::Transfer,
        .Access = SyncAccess::TransferWrite,
        .ProducingFamily = TransferFamily,
    };

    const auto d = DecideBarrier(produced, ImageLayout::ShaderReadOnly, SyncStage::FragmentShader,
                                 SyncAccess::ShaderRead, TransferFamily, GraphicsFamily);

    CHECK(d.NeedsBarrier);
    // The acquire half: transfer releases, graphics acquires.
    CHECK(d.SrcQueueFamilyIndex == TransferFamily);
    CHECK(d.DstQueueFamilyIndex == GraphicsFamily);
    // Layout/stage/access still go transfer-dst -> shader-read.
    CHECK(d.Src.Layout == ImageLayout::TransferDst);
    CHECK(d.Dst.Layout == ImageLayout::ShaderReadOnly);
    CHECK(d.Dst.Stage == SyncStage::FragmentShader);
    CHECK(d.Dst.Access == SyncAccess::ShaderRead);
    // After the graphics use the subresource is graphics-produced.
    CHECK(d.NewState.ProducingFamily == GraphicsFamily);
}

TEST_CASE("DecideBarrier: transfer-produced, families match, degenerates to a plain transition")
{
    // The MoltenVK single-queue collapse: transfer and graphics share a family,
    // so the same scenario carries no ownership transfer.
    const SubresourceState produced{
        .Layout = ImageLayout::TransferDst,
        .Stage = SyncStage::Transfer,
        .Access = SyncAccess::TransferWrite,
        .ProducingFamily = GraphicsFamily,
    };

    const auto d = DecideBarrier(produced, ImageLayout::ShaderReadOnly, SyncStage::FragmentShader,
                                 SyncAccess::ShaderRead, GraphicsFamily, GraphicsFamily);

    CHECK(d.NeedsBarrier); // still a layout change
    // No ownership transfer — both halves collapse to IGNORED.
    CHECK(d.SrcQueueFamilyIndex == QueueFamilyIgnored);
    CHECK(d.DstQueueFamilyIndex == QueueFamilyIgnored);
    CHECK(d.Src.Layout == ImageLayout::TransferDst);
    CHECK(d.Dst.Layout == ImageLayout::ShaderReadOnly);
}

TEST_CASE("DecideBarrier: graphics-produced read-after-read is unchanged by the queue dimension")
{
    // Regression guard: a resource that was never on the transfer queue (default
    // graphics-produced) takes the plain hazard path even with a discrete
    // transfer family present — no acquire, no spurious barrier.
    const auto d = DecideBarrier(ShaderRead, ImageLayout::ShaderReadOnly, SyncStage::FragmentShader,
                                 SyncAccess::ShaderRead, TransferFamily, GraphicsFamily);

    CHECK_FALSE(d.NeedsBarrier);
    CHECK(d.SrcQueueFamilyIndex == QueueFamilyIgnored);
    CHECK(d.DstQueueFamilyIndex == QueueFamilyIgnored);

    // A read in a new stage takes its chaining barrier, still with no ownership transfer.
    const auto compute =
        DecideBarrier(ShaderRead, ImageLayout::ShaderReadOnly, SyncStage::ComputeShader,
                      SyncAccess::ShaderRead, TransferFamily, GraphicsFamily);
    CHECK(compute.NeedsBarrier);
    CHECK(compute.SrcQueueFamilyIndex == QueueFamilyIgnored);
    CHECK(compute.DstQueueFamilyIndex == QueueFamilyIgnored);
}

TEST_CASE("DecideBarrier: per-mip storage write -> sampled read of the same mip is a RAW barrier")
{
    // The hi-Z reduction's core hazard: dispatch k writes mip k as a storage image
    // (General/ShaderWrite), dispatch k+1 reads that mip as a sampled source
    // (ShaderReadOnly/ShaderRead). Tracked state is per-(image, mipLevel), so this is
    // exactly the state mip k carries when the read arrives — both the source write
    // and the layout change make it a barrier.
    const SubresourceState mipWritten{
        .Layout = ImageLayout::General,
        .Stage = SyncStage::ComputeShader,
        .Access = SyncAccess::ShaderWrite,
    };

    const auto d = DecideSameQueue(mipWritten, ImageLayout::ShaderReadOnly,
                                   SyncStage::ComputeShader, SyncAccess::ShaderRead);

    CHECK(d.NeedsBarrier); // source write + layout change
    CHECK(d.Src.Layout == ImageLayout::General);
    CHECK(d.Src.Access == SyncAccess::ShaderWrite);
    CHECK(d.Dst.Layout == ImageLayout::ShaderReadOnly);
    CHECK(d.Dst.Access == SyncAccess::ShaderRead);
}

TEST_CASE("DecideBarrier: a different mip's state is tracked independently")
{
    // Per-(image, mipLevel) tracking: a mip the reduction has not yet written this
    // pass (its slot is still General/ShaderWrite from the prior frame's last write)
    // re-written this frame derives only the write-after-write barrier on its own
    // state — it never inherits a neighbouring mip's just-applied ShaderReadOnly
    // layout, so the chain's per-mip read-after-write does not spill across levels.
    const SubresourceState mipWritten{
        .Layout = ImageLayout::General,
        .Stage = SyncStage::ComputeShader,
        .Access = SyncAccess::ShaderWrite,
    };

    const auto d = DecideSameQueue(mipWritten, ImageLayout::General, SyncStage::ComputeShader,
                                   SyncAccess::ShaderWrite);

    CHECK(d.NeedsBarrier);                       // write-after-write on this mip
    CHECK(d.Src.Layout == ImageLayout::General); // no layout change
    CHECK(d.Dst.Layout == ImageLayout::General);
    CHECK(d.NewState.Access == SyncAccess::ShaderWrite);
}

TEST_CASE("IsWriteAccess classifies write bits and reads")
{
    CHECK(IsWriteAccess(SyncAccess::ColorAttachmentWrite));
    CHECK(IsWriteAccess(SyncAccess::DepthStencilAttachmentWrite));
    CHECK(IsWriteAccess(SyncAccess::ShaderWrite));
    CHECK(IsWriteAccess(SyncAccess::TransferWrite));
    CHECK(IsWriteAccess(SyncAccess::HostWrite));
    CHECK(IsWriteAccess(SyncAccess::MemoryWrite));
    // A write OR'd with reads is still a write.
    CHECK(IsWriteAccess(SyncAccess::ShaderRead | SyncAccess::ShaderWrite));

    CHECK_FALSE(IsWriteAccess(SyncAccess::None));
    CHECK_FALSE(IsWriteAccess(SyncAccess::ShaderRead));
    CHECK_FALSE(IsWriteAccess(SyncAccess::ColorAttachmentRead));
    CHECK_FALSE(IsWriteAccess(SyncAccess::TransferRead));
}

TEST_CASE("ScopeFor maps each AccessKind to its documented scope")
{
    using Kind = AccessKind;

    const auto color = ScopeFor(Kind::ColorAttachment);
    CHECK(color.Layout == ImageLayout::ColorAttachment);
    CHECK(color.Stage == SyncStage::ColorAttachmentOutput);
    CHECK(color.Access == (SyncAccess::ColorAttachmentWrite | SyncAccess::ColorAttachmentRead));

    const auto depth = ScopeFor(Kind::DepthAttachment);
    CHECK(depth.Layout == ImageLayout::DepthAttachment);
    CHECK(depth.Stage == (SyncStage::EarlyFragmentTests | SyncStage::LateFragmentTests));
    CHECK(depth.Access ==
          (SyncAccess::DepthStencilAttachmentWrite | SyncAccess::DepthStencilAttachmentRead));

    // The sampled kinds share a layout and an access and differ only in the stages they name —
    // which is the whole point of their being several: a barrier's source scope must name the
    // stage that actually read, or it waits on work that never happened.
    const auto sample = ScopeFor(Kind::SampleGraphics);
    CHECK(sample.Layout == ImageLayout::ShaderReadOnly);
    CHECK(sample.Stage == SyncStage::FragmentShader);
    CHECK(sample.Access == SyncAccess::ShaderRead);

    const auto sampleCompute = ScopeFor(Kind::SampleCompute);
    CHECK(sampleCompute.Layout == ImageLayout::ShaderReadOnly);
    CHECK(sampleCompute.Stage == SyncStage::ComputeShader);
    CHECK(sampleCompute.Access == SyncAccess::ShaderRead);

    const auto sampleVertex = ScopeFor(Kind::SampleVertexFragment);
    CHECK(sampleVertex.Layout == ImageLayout::ShaderReadOnly);
    CHECK(sampleVertex.Stage == (SyncStage::VertexShader | SyncStage::FragmentShader));
    CHECK(sampleVertex.Access == SyncAccess::ShaderRead);

    const auto sampleAny = ScopeFor(Kind::SampleAny);
    CHECK(sampleAny.Layout == ImageLayout::ShaderReadOnly);
    CHECK(sampleAny.Access == SyncAccess::ShaderRead);

    // SampleAny covers every other sampled kind, which is what makes it the safe release label.
    CHECK((sampleAny.Stage & sample.Stage) == sample.Stage);
    CHECK((sampleAny.Stage & sampleCompute.Stage) == sampleCompute.Stage);
    CHECK((sampleAny.Stage & sampleVertex.Stage) == sampleVertex.Stage);

    CHECK(IsSampledAccess(Kind::SampleGraphics));
    CHECK(IsSampledAccess(Kind::SampleCompute));
    CHECK(IsSampledAccess(Kind::SampleAny));
    CHECK(IsSampledAccess(Kind::SampleVertexFragment));
    CHECK_FALSE(IsSampledAccess(Kind::StorageRead));
    CHECK_FALSE(IsSampledAccess(Kind::TransferSrc));

    const auto sread = ScopeFor(Kind::StorageRead);
    CHECK(sread.Layout == ImageLayout::General);
    CHECK(sread.Stage == SyncStage::ComputeShader);
    CHECK(sread.Access == SyncAccess::ShaderRead);

    const auto swrite = ScopeFor(Kind::StorageWrite);
    CHECK(swrite.Layout == ImageLayout::General);
    CHECK(swrite.Stage == SyncStage::ComputeShader);
    CHECK(swrite.Access == SyncAccess::ShaderWrite);

    // Read-write carries both accesses, so a barrier into it makes earlier writes visible to the
    // pass's loads and atomics, and it counts as a write for the next use's hazard.
    const auto srw = ScopeFor(Kind::StorageReadWrite);
    CHECK(srw.Layout == ImageLayout::General);
    CHECK(srw.Stage == SyncStage::ComputeShader);
    CHECK(srw.Access == (SyncAccess::ShaderRead | SyncAccess::ShaderWrite));
    CHECK(IsWriteAccess(srw.Access));
    CHECK_FALSE(IsSampledAccess(Kind::StorageReadWrite));

    const auto tsrc = ScopeFor(Kind::TransferSrc);
    CHECK(tsrc.Layout == ImageLayout::TransferSrc);
    CHECK(tsrc.Stage == SyncStage::Transfer);
    CHECK(tsrc.Access == SyncAccess::TransferRead);

    const auto tdst = ScopeFor(Kind::TransferDst);
    CHECK(tdst.Layout == ImageLayout::TransferDst);
    CHECK(tdst.Stage == SyncStage::Transfer);
    CHECK(tdst.Access == SyncAccess::TransferWrite);

    // Buffer access kinds carry no layout (a buffer has none): the layout stays
    // Undefined and only stage/access are meaningful.
    const auto indirect = ScopeFor(Kind::IndirectRead);
    CHECK(indirect.Layout == ImageLayout::Undefined);
    CHECK(indirect.Stage == SyncStage::DrawIndirect);
    CHECK(indirect.Access == SyncAccess::IndirectCommandRead);

    const auto sbr = ScopeFor(Kind::StorageBufferRead);
    CHECK(sbr.Layout == ImageLayout::Undefined);
    CHECK(sbr.Stage == SyncStage::ComputeShader);
    CHECK(sbr.Access == SyncAccess::ShaderRead);

    const auto sbrg = ScopeFor(Kind::StorageBufferReadGraphics);
    CHECK(sbrg.Layout == ImageLayout::Undefined);
    CHECK(sbrg.Stage == (SyncStage::VertexShader | SyncStage::FragmentShader));
    CHECK(sbrg.Access == SyncAccess::ShaderRead);

    const auto sbw = ScopeFor(Kind::StorageBufferWrite);
    CHECK(sbw.Layout == ImageLayout::Undefined);
    CHECK(sbw.Stage == SyncStage::ComputeShader);
    CHECK(sbw.Access == SyncAccess::ShaderWrite);
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
    CHECK(write.Stage == SyncStage::ComputeShader);
    CHECK(write.Access == SyncAccess::ShaderWrite);
    CHECK(read.Stage == SyncStage::DrawIndirect);
    CHECK(read.Access == SyncAccess::IndirectCommandRead);
}

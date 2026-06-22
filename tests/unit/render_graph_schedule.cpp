// Render-graph schedule-derivation unit cases. DeriveRenderGraphSchedule is the
// device-free core of RenderGraph::Compile (Compile delegates to it, then allocates
// transient backing), so the graph-level barrier orchestration — multi-pass
// read-after-write ordering, per-mip subresource tracking, buffer hazard detection,
// and attachment collection — is testable without a GPU. The per-edge hazard rule it
// composes is covered by barrier_decision.cpp; these cases cover the cross-pass
// scheduling around it.

#include <doctest/doctest.h>

#include <Veng/Renderer/Backend/RenderGraphSchedule.h>

using namespace Veng;
using namespace Veng::Renderer;
using namespace Veng::Renderer::Backend;

namespace
{
    // An imported image slot — exempt from the transient read-before-write and
    // usage-flag validation, so cases compose passes freely.
    [[nodiscard]] ScheduleResource ImportImage(string name)
    {
        return {.IsImport = true, .IsBuffer = false, .Name = std::move(name)};
    }

    // An imported buffer slot.
    [[nodiscard]] ScheduleResource ImportBuffer(string name)
    {
        return {.IsImport = true, .IsBuffer = true, .Name = std::move(name)};
    }

    // A graph-owned transient image with the usage its accesses require.
    [[nodiscard]] ScheduleResource TransientImage(string name, ImageUsage usage)
    {
        return {.IsImport = false, .IsBuffer = false, .Name = std::move(name), .ImageUsage = usage};
    }

    [[nodiscard]] RenderGraph::Access Access(u32 slot, AccessKind kind)
    {
        return {.Resource = ResourceId{.Index = slot}, .Kind = kind};
    }
}

TEST_CASE("Schedule: a color attachment write derives one transition and one attachment")
{
    const vector<ScheduleResource> resources{ImportImage("Target")};
    const vector<RenderGraph::Access> accesses{{.Resource = ResourceId{.Index = 0},
                                                .Kind = AccessKind::ColorAttachment,
                                                .Load = LoadOp::Clear,
                                                .Store = StoreOp::Store}};
    const vector<SchedulePass> passes{{.Name = "Geometry", .Accesses = accesses}};

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 1);
    REQUIRE(schedule[0].Transitions.size() == 1);
    CHECK(schedule[0].Transitions[0].Slot == 0);
    CHECK(schedule[0].Transitions[0].Dst.Layout == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(schedule[0].BufferBarriers.empty());

    REQUIRE(schedule[0].Attachments.size() == 1);
    CHECK(schedule[0].Attachments[0].Slot == 0);
    CHECK_FALSE(schedule[0].Attachments[0].IsDepth);
    CHECK(schedule[0].Attachments[0].Load == LoadOp::Clear);
    CHECK(schedule[0].Attachments[0].Store == StoreOp::Store);
}

TEST_CASE(
    "Schedule: write-then-sample across passes transitions the producer's slot to shader-read")
{
    // The load-bearing cross-pass hazard: pass 0 renders into a transient, pass 1
    // samples it. The sample pass must transition that slot to ShaderReadOnly.
    const vector<ScheduleResource> resources{
        TransientImage("GBuffer", ImageUsage::ColorAttachment | ImageUsage::Sampled)};

    const vector<RenderGraph::Access> writeAccess{Access(0, AccessKind::ColorAttachment)};
    const vector<RenderGraph::Access> readAccess{Access(0, AccessKind::Sample)};
    const vector<SchedulePass> passes{
        {.Name = "Geometry", .Accesses = writeAccess},
        {.Name = "Lighting", .Accesses = readAccess},
    };

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 2);
    // Producer transitions to color-attachment.
    REQUIRE(schedule[0].Transitions.size() == 1);
    CHECK(schedule[0].Transitions[0].Dst.Layout == vk::ImageLayout::eColorAttachmentOptimal);
    // Consumer transitions the same slot to shader-read.
    REQUIRE(schedule[1].Transitions.size() == 1);
    CHECK(schedule[1].Transitions[0].Slot == 0);
    CHECK(schedule[1].Transitions[0].Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(schedule[1].Transitions[0].Dst.Access == vk::AccessFlagBits::eShaderRead);
}

TEST_CASE("Schedule: a mip chain tracks each level's transition on its own slot")
{
    // ImportImageMips lays a mip chain out as one resource slot per level. A
    // reduction dispatch reads mip k-1 (sampled) and writes mip k (storage); the
    // derived transitions must land on the distinct per-level slots so the per-mip
    // read-after-write barrier is emitted, not a single whole-image hazard.
    const vector<ScheduleResource> resources{
        ImportImage("Pyramid Mip 0"),
        ImportImage("Pyramid Mip 1"),
        ImportImage("Pyramid Mip 2"),
    };

    // Dispatch that writes mip 0 (seed), then a dispatch reading mip 0 and writing mip 1,
    // then one reading mip 1 and writing mip 2.
    const vector<RenderGraph::Access> seed{Access(0, AccessKind::StorageWrite)};
    const vector<RenderGraph::Access> reduce1{Access(0, AccessKind::Sample),
                                              Access(1, AccessKind::StorageWrite)};
    const vector<RenderGraph::Access> reduce2{Access(1, AccessKind::Sample),
                                              Access(2, AccessKind::StorageWrite)};
    const vector<SchedulePass> passes{
        {.Name = "Seed", .Accesses = seed},
        {.Name = "Reduce 0->1", .Accesses = reduce1},
        {.Name = "Reduce 1->2", .Accesses = reduce2},
    };

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 3);
    // Reduce 0->1: sample mip 0 (ShaderReadOnly), storage-write mip 1 (General).
    REQUIRE(schedule[1].Transitions.size() == 2);
    CHECK(schedule[1].Transitions[0].Slot == 0);
    CHECK(schedule[1].Transitions[0].Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(schedule[1].Transitions[1].Slot == 1);
    CHECK(schedule[1].Transitions[1].Dst.Layout == vk::ImageLayout::eGeneral);
    // Reduce 1->2: the read is on slot 1 now, not slot 0 — levels are independent.
    REQUIRE(schedule[2].Transitions.size() == 2);
    CHECK(schedule[2].Transitions[0].Slot == 1);
    CHECK(schedule[2].Transitions[0].Dst.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(schedule[2].Transitions[1].Slot == 2);
}

TEST_CASE("Schedule: compute storage-write to graphics indirect-read derives a buffer barrier")
{
    // The GPU-driven culling handoff: a compute pass writes the indirect-args buffer,
    // a graphics pass reads it as draw-indirect. The barrier bakes both halves.
    const vector<ScheduleResource> resources{ImportBuffer("DrawArgs")};

    const vector<RenderGraph::Access> writeArgs{Access(0, AccessKind::StorageBufferWrite)};
    const vector<RenderGraph::Access> readArgs{Access(0, AccessKind::IndirectRead)};
    const vector<SchedulePass> passes{
        {.Name = "Cull", .Accesses = writeArgs},
        {.Name = "Draw", .Accesses = readArgs},
    };

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 2);
    // The producing pass emits no barrier (no prior access).
    CHECK(schedule[0].BufferBarriers.empty());
    CHECK(schedule[0].Transitions.empty()); // buffers never produce image transitions

    // The consuming pass emits the compute-write -> indirect-read barrier.
    REQUIRE(schedule[1].BufferBarriers.size() == 1);
    const ScheduledBufferBarrier& barrier = schedule[1].BufferBarriers[0];
    CHECK(barrier.Slot == 0);
    CHECK(barrier.SrcStage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(barrier.SrcAccess == vk::AccessFlagBits::eShaderWrite);
    CHECK(barrier.DstStage == vk::PipelineStageFlagBits::eDrawIndirect);
    CHECK(barrier.DstAccess == vk::AccessFlagBits::eIndirectCommandRead);
}

TEST_CASE("Schedule: a read-after-read on a buffer emits no barrier")
{
    const vector<ScheduleResource> resources{ImportBuffer("Params")};

    const vector<RenderGraph::Access> readA{Access(0, AccessKind::StorageBufferRead)};
    const vector<RenderGraph::Access> readB{Access(0, AccessKind::StorageBufferRead)};
    const vector<SchedulePass> passes{
        {.Name = "ReadA", .Accesses = readA},
        {.Name = "ReadB", .Accesses = readB},
    };

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 2);
    CHECK(schedule[0].BufferBarriers.empty());
    CHECK(schedule[1].BufferBarriers.empty()); // read-after-read is not a hazard
}

TEST_CASE("Schedule: a write after two reads waits on both reads' stages")
{
    // The OR-accumulation rule: a later write must wait on every prior read, so the
    // barrier's source stage spans both reads (storage-buffer read in compute, indirect
    // read in draw-indirect) — not just the most recent one.
    const vector<ScheduleResource> resources{ImportBuffer("Shared")};

    const vector<RenderGraph::Access> readStorage{Access(0, AccessKind::StorageBufferRead)};
    const vector<RenderGraph::Access> readIndirect{Access(0, AccessKind::IndirectRead)};
    const vector<RenderGraph::Access> write{Access(0, AccessKind::StorageBufferWrite)};
    const vector<SchedulePass> passes{
        {.Name = "ReadStorage", .Accesses = readStorage},
        {.Name = "ReadIndirect", .Accesses = readIndirect},
        {.Name = "Write", .Accesses = write},
    };

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 3);
    CHECK(schedule[0].BufferBarriers.empty());
    CHECK(schedule[1].BufferBarriers.empty()); // read-after-read
    REQUIRE(schedule[2].BufferBarriers.size() == 1);
    const ScheduledBufferBarrier& barrier = schedule[2].BufferBarriers[0];
    CHECK(barrier.SrcStage ==
          (vk::PipelineStageFlagBits::eComputeShader | vk::PipelineStageFlagBits::eDrawIndirect));
    CHECK(barrier.SrcAccess ==
          (vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eIndirectCommandRead));
    CHECK(barrier.DstStage == vk::PipelineStageFlagBits::eComputeShader);
    CHECK(barrier.DstAccess == vk::AccessFlagBits::eShaderWrite);
}

TEST_CASE("Schedule: a depth attachment is flagged depth and a color attachment is not")
{
    const vector<ScheduleResource> resources{
        TransientImage("Color", ImageUsage::ColorAttachment),
        TransientImage("Depth", ImageUsage::DepthAttachment),
    };
    const vector<RenderGraph::Access> accesses{Access(0, AccessKind::ColorAttachment),
                                               Access(1, AccessKind::DepthAttachment)};
    const vector<SchedulePass> passes{{.Name = "Geometry", .Accesses = accesses}};

    const auto schedule = DeriveRenderGraphSchedule(resources, passes);

    REQUIRE(schedule.size() == 1);
    REQUIRE(schedule[0].Attachments.size() == 2);
    CHECK_FALSE(schedule[0].Attachments[0].IsDepth);
    CHECK(schedule[0].Attachments[0].Slot == 0);
    CHECK(schedule[0].Attachments[1].IsDepth);
    CHECK(schedule[0].Attachments[1].Slot == 1);
    CHECK(schedule[0].Transitions[1].Dst.Layout == vk::ImageLayout::eDepthStencilAttachmentOptimal);
}

// Indirect-draw infrastructure case: drives the full
//   host-fill commands -> compute writes instanceCount -> graphics reads as
//   indirect args -> DrawIndexedIndirect
// chain the GPU cull builds on, in isolation.
//
// A fixed-max indirect buffer holds three DrawIndexedIndirectCommand records at a stride wider than
// a record. A compute pass writes instanceCount = 1 into the middle one (the survivor) and 0 into
// the other two (culled). The graphics pass issues one DrawIndexedIndirect over all three: the
// survivor draws a fullscreen triangle whose red channel is the candidate id it read from an
// instance-rate vertex attribute at the command's firstInstance; the zero-instance commands no-op.
// The downloaded pixel proves three things at once:
//   1. The base instance reached the shader — the survivor's firstInstance selected the right
//      instance-rate element (the candidate-index read mechanism the cull depends on).
//   2. The zero-instance commands issued no work — their (different) candidates never land.
//   3. A multi-draw steps by the stride, so the record after a culled one is the one drawn.
//
// The compute -> indirect handoff is a graph-derived buffer barrier
// (StorageBufferWrite -> IndirectRead), so the validation gate pins it on both backends.
//
// The count-driven cases cull into compacted records and draw as many as the cull counted, on a
// device with DeviceCapabilities::IndirectDrawCount; they skip with a message elsewhere.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

#include <gpu/fixture.h>
#include <support/GpuContext.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // One record per 32 bytes: wider than the 20-byte record, so a multi-draw that ignored the
    // stride would read the next record's fields as this one's.
    constexpr u32 CommandStride = 32;

    struct PaddedCommand
    {
        DrawIndexedIndirectCommand Command;
        std::array<u32, 3> Padding{};
    };
    static_assert(sizeof(PaddedCommand) == CommandStride);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "indirect draw: base instance selects the instance-rate attribute, "
                  "zero-instance command no-ops")
{
    if (!Context.IsGpuDrivenCullingSupported())
    {
        // multiDrawIndirect + drawIndirectFirstInstance absent — the cull path is
        // unavailable on this device; skip rather than fail.
        MESSAGE("GPU-driven culling unsupported on this device; skipping");
        return;
    }

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mounted);

    const auto cullShader = assets.LoadSync<Shader>(AssetId{0x1F48}); // indirect_cull.comp
    REQUIRE(cullShader);
    const auto vertShader = assets.LoadSync<Shader>(AssetId{0x1F46}); // indirect_instance.vert
    REQUIRE(vertShader);
    const auto fragShader = assets.LoadSync<Shader>(AssetId{0x1F47}); // indirect_instance.frag
    REQUIRE(fragShader);

    constexpr u32 size = 8;
    constexpr u32 candidateCount = 8;

    // The candidate ids the instance-rate attribute reads, each the element's index times ten so
    // a wrong base instance reads a recognizably different id.
    constexpr u32 survivorInstance = 5;
    std::array<u32, candidateCount> candidateIds{};
    for (u32 i = 0; i < candidateCount; i++)
    {
        candidateIds[i] = i * 10;
    }
    const std::array<u8, 4> expectedColor{survivorInstance * 10, 0, 0, 255};

    auto instanceBuffer = Buffer::Create(
        Context,
        {.Name = "Candidate Ids", .Size = sizeof(candidateIds), .Usage = BufferUsage::Vertex});
    instanceBuffer->UploadSync(std::span<const u8>(reinterpret_cast<const u8*>(candidateIds.data()),
                                                   sizeof(candidateIds)));

    // The per-vertex attribute the layout carries ahead of the candidate id; the triangle is
    // placed from the vertex index alone.
    const std::array<f32, 3> unused{};
    auto vertexBuffer = Buffer::Create(
        Context, {.Name = "Vertices", .Size = sizeof(unused), .Usage = BufferUsage::Vertex});
    vertexBuffer->UploadSync(
        std::span<const u8>(reinterpret_cast<const u8*>(unused.data()), sizeof(unused)));

    // The fullscreen triangle reads VertexIndex, but DrawIndexed still consumes an
    // index buffer — three indices for the one triangle.
    const std::array<u32, 3> indices{0, 1, 2};
    auto indexBuffer = Buffer::Create(
        Context, {.Name = "Indices", .Size = sizeof(indices), .Usage = BufferUsage::Index});
    indexBuffer->UploadSync(
        std::span<const u8>(reinterpret_cast<const u8*>(indices.data()), sizeof(indices)));

    // Host-fill every command except instanceCount, which the compute pass writes: the middle
    // one survives, the culled ones on either side name other candidates.
    const auto command = [](const u32 firstInstance)
    {
        return PaddedCommand{.Command = {.IndexCount = 3,
                                         .InstanceCount = 99,
                                         .FirstIndex = 0,
                                         .VertexOffset = 0,
                                         .FirstInstance = firstInstance}};
    };
    const std::array<PaddedCommand, 3> commands{command(2), command(survivorInstance), command(7)};
    auto indirectBuffer =
        Buffer::Create(Context, {.Name = "Indirect Commands",
                                 .Size = sizeof(commands),
                                 .Usage = BufferUsage::Indirect | BufferUsage::Storage |
                                          BufferUsage::TransferDst});
    indirectBuffer->UploadSync(
        std::span<const u8>(reinterpret_cast<const u8*>(commands.data()), sizeof(commands)));

    const auto target =
        Image::Create(Context, {.Name = "Indirect Target",
                                .Extent = {size, size, 1},
                                .Format = Format::RGBA8Unorm,
                                .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc});
    auto targetView = ImageView::Create(Context, {.Name = "Indirect Target View", .Image = target});

    // --- Compute pipeline: writes the commands' instanceCount (set 1, binding 0). ---
    auto cullSetLayout =
        DescriptorSetLayout::Create(Context, {.Name = "Cull Set Layout",
                                              .Bindings = {{.Binding = 0,
                                                            .Type = DescriptorType::StorageBuffer,
                                                            .Count = 1,
                                                            .Stages = ShaderStage::Compute}}});
    const auto cullLayout = PipelineLayout::Create(
        Context, {.Name = "Cull Layout", .DescriptorSetLayouts = {cullSetLayout}});
    auto cullPipeline = ComputePipeline::Create(
        Context,
        {.Name = "Cull Pipeline",
         .PipelineLayout = cullLayout,
         .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullShader->Get()->Module}});
    auto cullSet = DescriptorSet::Create(Context, {.Name = "Cull Set", .Layout = cullSetLayout});
    cullSet->Write(0, indirectBuffer);

    // --- Graphics pipeline: an instance-rate candidate id, no descriptors. ---
    const auto drawLayout = PipelineLayout::Create(Context, {.Name = "Indirect Draw Layout"});
    const auto drawPipeline = GraphicsPipeline::Create(
        Context,
        {.Name = "Indirect Draw Pipeline",
         .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
         .VertexBufferLayout = VertexBufferLayout{{Format::R32Sfloat, "a_Unused"}},
         .InstanceCandidateId = true,
         .PipelineLayout = drawLayout,
         .ShaderStages = {{.Stage = ShaderStage::Vertex, .Module = vertShader->Get()->Module},
                          {.Stage = ShaderStage::Fragment, .Module = fragShader->Get()->Module}}});

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);

            const ResourceId targetId = graph.Import("Target");
            const ResourceId indirectId = graph.ImportBuffer("Indirect");

            // Compute writes the commands as a storage buffer; the graphics pass reads
            // them as indirect args — the graph derives the buffer barrier between.
            graph.AddComputePass("Cull")
                .StorageBufferWrite(indirectId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        ctx.Cmd().BindPipeline(cullPipeline);
                        // Set 0 is reserved for the bindless registry in every layout;
                        // the cull's storage buffer is the author-declared set 1.
                        ctx.Cmd().BindDescriptorSets(
                            {.Sets = {cullSet},
                             .FirstSet = 3,
                             .PipelineBindPoint = PipelineBindPoint::Compute});
                        ctx.Cmd().Dispatch(1, 1, 1);
                    });

            graph.AddPass("Indirect Draw")
                .Color({.Resource = targetId, .Load = LoadOp::Clear, .Store = StoreOp::Store})
                .IndirectRead(indirectId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& c = ctx.Cmd();
                        c.BindPipeline(drawPipeline);
                        c.SetViewport({0, 0}, {size, size});
                        c.SetScissor({0, 0}, {size, size});
                        c.BindVertexBuffer(vertexBuffer);
                        c.BindInstanceBuffer(instanceBuffer);
                        c.BindIndexBuffer(indexBuffer, IndexType::U32);
                        c.DrawIndexedIndirect(ctx.ResolvedBuffer(indirectId), 0,
                                              static_cast<u32>(commands.size()), CommandStride);
                    });

            auto compiled = graph.Compile();
            compiled->Execute(
                cmd,
                std::array{RenderGraph::ImportBinding{.Id = targetId, .View = targetView},
                           RenderGraph::ImportBinding{.Id = indirectId, .Buffer = indirectBuffer}});
        });

    const vector<u8> pixels = target->Download();
    CHECK(Test::PixelsMatch(pixels, expectedColor));
}

namespace
{
    // The count-driven cases: a compute pass culls eight candidates (every index not a multiple of
    // three survives) into indirect records, and one indirect draw of a fullscreen triangle per
    // record into a one-pixel target counts the fragments it shades and marks each candidate drawn.
    // A record draws one instance, so the fragment count is the number of draws executed.
    constexpr AssetId CountCullShaderId{0x95B48A78EA5B5E22ULL};
    constexpr AssetId CountFragmentShaderId{0x0F06AC2666DCE248ULL};
    constexpr u32 CountCandidates = 8;
    constexpr u32 SurvivorMask = (1u << 1) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7);
    constexpr u32 SurvivorCount = 5;

    // How the draw reaches the cull's records.
    enum class CountPath : u8
    {
        // Every record, a culled one with no instances.
        FixedMaximum,
        // As many indexed records as the cull counted.
        IndexedCount,
        // As many non-indexed records as the cull counted.
        NonIndexedCount,
    };

    // What the draw shaded: fragments, and a bit per candidate.
    struct Seen
    {
        u32 Fragments = 0;
        u32 Candidates = 0;
    };

    Seen DrawThroughPath(Veng::Test::GpuFixture& fixture, const CountPath route)
    {
        Context& context = fixture.Context;
        AssetManager assets(context, fixture.Tasks, fixture.Types);
        REQUIRE(assets.Mount(path(TEST_SHADER_PACK)));
        const auto cullShader = assets.LoadSync<Shader>(CountCullShaderId);
        const auto vertShader = assets.LoadSync<Shader>(AssetId{0x1F46}); // indirect_instance.vert
        const auto fragShader = assets.LoadSync<Shader>(CountFragmentShaderId);
        REQUIRE(cullShader);
        REQUIRE(vertShader);
        REQUIRE(fragShader);

        const auto upload = [](const Ref<Buffer>& buffer, const auto& values)
        {
            buffer->UploadSync(std::span<const u8>(reinterpret_cast<const u8*>(values.data()),
                                                   values.size() * sizeof(values[0])));
        };

        std::array<u32, CountCandidates> candidateIds{};
        for (u32 i = 0; i < CountCandidates; ++i)
        {
            candidateIds[i] = i;
        }
        auto instanceBuffer = Buffer::Create(
            context,
            {.Name = "Candidate Ids", .Size = sizeof(candidateIds), .Usage = BufferUsage::Vertex});
        upload(instanceBuffer, candidateIds);
        const std::array<f32, 3> unused{};
        auto vertexBuffer = Buffer::Create(
            context, {.Name = "Vertices", .Size = sizeof(unused), .Usage = BufferUsage::Vertex});
        upload(vertexBuffer, unused);
        const std::array<u32, 3> indices{0, 1, 2};
        auto indexBuffer = Buffer::Create(
            context, {.Name = "Indices", .Size = sizeof(indices), .Usage = BufferUsage::Index});
        upload(indexBuffer, indices);

        // Every slot starts as a record drawing candidate 0, which the cull never keeps, so a draw
        // past the count shows up as an extra fragment and a culled candidate.
        std::array<PaddedCommand, CountCandidates> stale{};
        for (PaddedCommand& record : stale)
        {
            record.Command = {.IndexCount = 3, .InstanceCount = 1};
        }
        auto records =
            Buffer::Create(context, {.Name = "Count Records",
                                     .Size = sizeof(stale),
                                     .Usage = BufferUsage::Indirect | BufferUsage::Storage |
                                              BufferUsage::TransferDst});
        upload(records, stale);
        auto count =
            Buffer::Create(context, {.Name = "Count",
                                     .Size = 16,
                                     .Usage = BufferUsage::Indirect | BufferUsage::Storage |
                                              BufferUsage::TransferDst});
        upload(count, std::array<u32, 4>{});
        const auto seen =
            Buffer::Create(context, {.Name = "Seen",
                                     .Size = 16,
                                     .Usage = BufferUsage::Storage | BufferUsage::TransferSrc |
                                              BufferUsage::TransferDst});
        upload(seen, std::array<u32, 4>{});

        const auto target = Image::Create(context, {.Name = "Count Target",
                                                    .Extent = {1, 1, 1},
                                                    .Format = Format::RGBA8Unorm,
                                                    .Usage = ImageUsage::ColorAttachment});
        auto targetView =
            ImageView::Create(context, {.Name = "Count Target View", .Image = target});

        const auto storage = [](const u32 binding, const ShaderStage stages)
        {
            return DescriptorBinding{.Binding = binding,
                                     .Type = DescriptorType::StorageBuffer,
                                     .Count = 1,
                                     .Stages = stages};
        };
        auto cullSetLayout = DescriptorSetLayout::Create(
            context,
            {.Name = "Count Cull Set Layout",
             .Bindings = {storage(0, ShaderStage::Compute), storage(1, ShaderStage::Compute)}});
        const auto cullLayout = PipelineLayout::Create(
            context, {.Name = "Count Cull Layout",
                      .DescriptorSetLayouts = {cullSetLayout},
                      .PushConstantRanges = {PushConstantRange::Of<u32>(ShaderStage::Compute)}});
        auto cullPipeline = ComputePipeline::Create(
            context,
            {.Name = "Count Cull Pipeline",
             .PipelineLayout = cullLayout,
             .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullShader->Get()->Module}});
        auto cullSet =
            DescriptorSet::Create(context, {.Name = "Count Cull Set", .Layout = cullSetLayout});
        cullSet->Write(0, records);
        cullSet->Write(1, count);

        auto seenSetLayout = DescriptorSetLayout::Create(
            context, {.Name = "Seen Set Layout", .Bindings = {storage(0, ShaderStage::Fragment)}});
        const auto drawLayout = PipelineLayout::Create(
            context, {.Name = "Count Draw Layout", .DescriptorSetLayouts = {seenSetLayout}});
        auto seenSet =
            DescriptorSet::Create(context, {.Name = "Seen Set", .Layout = seenSetLayout});
        seenSet->Write(0, seen);
        const auto drawPipeline = GraphicsPipeline::Create(
            context, {.Name = "Count Draw Pipeline",
                      .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                      .VertexBufferLayout = VertexBufferLayout{{Format::R32Sfloat, "a_Unused"}},
                      .InstanceCandidateId = true,
                      .PipelineLayout = drawLayout,
                      .ShaderStages = {
                          {.Stage = ShaderStage::Vertex, .Module = vertShader->Get()->Module},
                          {.Stage = ShaderStage::Fragment, .Module = fragShader->Get()->Module}}});

        const u32 mode = route == CountPath::FixedMaximum   ? 0u
                         : route == CountPath::IndexedCount ? 1u
                                                            : 2u;
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId targetId = graph.Import("Target");
                const ResourceId recordsId = graph.ImportBuffer("Records");
                const ResourceId countId = graph.ImportBuffer("Count");

                graph.AddComputePass("Cull")
                    .StorageBufferWrite(recordsId)
                    .StorageBufferWrite(countId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& c = ctx.Cmd();
                            c.BindPipeline(cullPipeline);
                            c.BindDescriptorSets({.Sets = {cullSet},
                                                  .FirstSet = 3,
                                                  .PipelineBindPoint = PipelineBindPoint::Compute});
                            c.PushConstants(mode);
                            c.Dispatch(1, 1, 1);
                            if (route != CountPath::FixedMaximum)
                            {
                                c.PrepareIndirectCount(records, 0, count, 0, CountCandidates,
                                                       CommandStride);
                            }
                        });

                graph.AddPass("Draw")
                    .Color({.Resource = targetId, .Load = LoadOp::Clear, .Store = StoreOp::Store})
                    .IndirectRead(recordsId)
                    .IndirectRead(countId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& c = ctx.Cmd();
                            c.BindPipeline(drawPipeline);
                            c.BindDescriptorSets({seenSet.get()}, 3);
                            c.SetViewport({0, 0}, {1, 1});
                            c.SetScissor({0, 0}, {1, 1});
                            c.BindVertexBuffer(vertexBuffer);
                            c.BindInstanceBuffer(instanceBuffer);
                            c.BindIndexBuffer(indexBuffer, IndexType::U32);
                            switch (route)
                            {
                            case CountPath::FixedMaximum:
                                c.DrawIndexedIndirect(records, 0, CountCandidates, CommandStride);
                                break;
                            case CountPath::IndexedCount:
                                c.DrawIndexedIndirectCount(records, 0, count, 0, CountCandidates,
                                                           CommandStride);
                                break;
                            case CountPath::NonIndexedCount:
                                c.DrawIndirectCount(records, 0, count, 0, CountCandidates,
                                                    CommandStride);
                                break;
                            }
                        });

                graph.Compile()->Execute(
                    cmd, std::array{RenderGraph::ImportBinding{.Id = targetId, .View = targetView},
                                    RenderGraph::ImportBinding{.Id = recordsId, .Buffer = records},
                                    RenderGraph::ImportBinding{.Id = countId, .Buffer = count}});
            });

        const vector<u8> bytes = seen->Download();
        Seen result;
        std::memcpy(&result.Fragments, bytes.data(), sizeof(u32));
        std::memcpy(&result.Candidates, bytes.data() + sizeof(u32), sizeof(u32));
        return result;
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "indirect draw: a count-driven draw executes exactly the survivors the cull counted")
{
    if (!Context.GetDeviceCapabilities().IndirectDrawCount)
    {
        MESSAGE("count-driven indirect draws unsupported on this device; skipping");
        return;
    }
    for (const CountPath path : {CountPath::IndexedCount, CountPath::NonIndexedCount})
    {
        CAPTURE(static_cast<u32>(path));
        const Seen seen = DrawThroughPath(*this, path);
        CHECK(seen.Fragments == SurvivorCount);
        CHECK(seen.Candidates == SurvivorMask);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "indirect draw: the fixed-maximum and count-driven paths draw the same survivors")
{
    if (!Context.IsGpuDrivenCullingSupported())
    {
        MESSAGE("GPU-driven culling unsupported on this device; skipping");
        return;
    }
    const Seen fixed = DrawThroughPath(*this, CountPath::FixedMaximum);
    CHECK(fixed.Candidates == SurvivorMask);
    if (!Context.GetDeviceCapabilities().IndirectDrawCount)
    {
        MESSAGE(
            "count-driven indirect draws unsupported on this device; the count path is skipped");
        return;
    }
    const Seen counted = DrawThroughPath(*this, CountPath::IndexedCount);
    CHECK(counted.Candidates == fixed.Candidates);
    CHECK(counted.Fragments == fixed.Fragments);
}

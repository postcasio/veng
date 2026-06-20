// Indirect-draw infrastructure case: drives the full
//   host-fill commands -> compute writes instanceCount -> graphics reads as
//   indirect args -> DrawIndexedIndirect
// chain that Plan 05 builds on, in isolation.
//
// A fixed-max indirect buffer holds two VkDrawIndexedIndirectCommand records. A
// compute pass writes instanceCount = 1 into the first (a survivor) and 0 into the
// second (culled). The graphics pass issues one DrawIndexedIndirect over both: the
// survivor draws a fullscreen triangle whose color is read from an instance-rate
// vertex attribute fetched at the command's firstInstance; the zero-instance
// command no-ops. The downloaded pixel proves two things at once:
//   1. The base instance reached the shader — the survivor's firstInstance selected
//      the right instance-rate element (the candidate-index read mechanism Plan 05
//      depends on; verified here before it does).
//   2. The zero-instance command issued no work — its (different) color never lands.
//
// The compute -> indirect handoff is a graph-derived buffer barrier
// (StorageBufferWrite -> IndirectRead), so under VE_DEBUG the validation gate pins
// the eComputeShader/eShaderWrite -> eDrawIndirect/eIndirectCommandRead barrier, the
// INDIRECT_BUFFER_BIT usage, and the 4-byte-aligned offset.
//
// Skips (exit 77) with no Vulkan ICD or when the device lacks the
// multiDrawIndirect / drawIndirectFirstInstance features the cull path needs.

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
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#include <gpu/fixture.h>
#include <support/GpuContext.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A VkDrawIndexedIndirectCommand laid out by hand: matches the Vulkan struct
    // (5 u32s, 20 bytes), the stride DrawIndexedIndirect expects.
    struct IndexedIndirectCommand
    {
        u32 IndexCount;
        u32 InstanceCount;
        u32 FirstIndex;
        i32 VertexOffset;
        u32 FirstInstance;
    };
    static_assert(sizeof(IndexedIndirectCommand) == 20);

    // Builds the raw graphics pipeline with an instance-rate color attribute on
    // binding 1, location 0. The engine VertexBufferLayout drives only a single
    // per-vertex binding, so the indirect/instance-rate input is assembled directly
    // here. Dynamic rendering, one RGBA8 color target, dynamic viewport/scissor.
    vk::Pipeline CreateInstancePipeline(Context& context, vk::PipelineLayout layout,
                                        vk::ShaderModule vertModule, const char* vertEntry,
                                        vk::ShaderModule fragModule, const char* fragEntry,
                                        vk::Format colorFormat)
    {
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                              .module = vertModule,
                                              .pName = vertEntry},
            vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                              .module = fragModule,
                                              .pName = fragEntry},
        };

        const vk::VertexInputBindingDescription binding{
            .binding = 1,
            .stride = sizeof(vec4),
            .inputRate = vk::VertexInputRate::eInstance,
        };
        const vk::VertexInputAttributeDescription attribute{
            .location = 0,
            .binding = 1,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = 0,
        };
        const vk::PipelineVertexInputStateCreateInfo vertexInput{
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &attribute,
        };

        const vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
            .topology = vk::PrimitiveTopology::eTriangleList};

        const vk::PipelineViewportStateCreateInfo viewportState{.viewportCount = 1,
                                                                .scissorCount = 1};
        const vk::PipelineRasterizationStateCreateInfo rasterization{
            .polygonMode = vk::PolygonMode::eFill,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eCounterClockwise,
            .lineWidth = 1.0f};
        const vk::PipelineMultisampleStateCreateInfo multisample{.rasterizationSamples =
                                                                     vk::SampleCountFlagBits::e1};

        const vk::PipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = vk::False,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        const vk::PipelineColorBlendStateCreateInfo colorBlend{.attachmentCount = 1,
                                                               .pAttachments = &blendAttachment};

        const std::array dynamicStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamicState{
            .dynamicStateCount = static_cast<u32>(dynamicStates.size()),
            .pDynamicStates = dynamicStates.data()};

        vk::PipelineRenderingCreateInfo renderingInfo{.colorAttachmentCount = 1,
                                                      .pColorAttachmentFormats = &colorFormat};

        const vk::GraphicsPipelineCreateInfo pipelineInfo{
            .pNext = &renderingInfo,
            .stageCount = static_cast<u32>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = layout,
        };

        return GetVkDevice(context)
            .createGraphicsPipeline(GetVkPipelineCache(context), pipelineInfo)
            .value;
    }
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

    // Per-candidate instance colors. The survivor's firstInstance indexes this; the
    // expected pixel is the color at that index. A wrong base instance reads a
    // different color, the test fails — proving the base instance reached the shader.
    constexpr u32 survivorInstance = 5;
    constexpr u32 culledInstance = 2;
    const std::array<u8, 4> expectedColor{255, 0, 0, 255}; // red, at index 5

    std::array<vec4, candidateCount> instanceColors{};
    for (u32 i = 0; i < candidateCount; i++)
    {
        instanceColors[i] = vec4(0.0f, 1.0f, 0.0f, 1.0f); // green decoy
    }
    instanceColors[survivorInstance] = vec4(1.0f, 0.0f, 0.0f, 1.0f); // red — the survivor
    instanceColors[culledInstance] = vec4(0.0f, 0.0f, 1.0f, 1.0f);   // blue — must never land

    auto instanceBuffer = Buffer::Create(Context, {.Name = "Instance Colors",
                                                   .Size = sizeof(vec4) * candidateCount,
                                                   .Usage = BufferUsage::Vertex});
    instanceBuffer->UploadSync(std::span<const u8>(
        reinterpret_cast<const u8*>(instanceColors.data()), sizeof(vec4) * candidateCount));

    // The fullscreen triangle reads VertexIndex, but DrawIndexed still consumes an
    // index buffer — three indices for the one triangle.
    const std::array<u32, 3> indices{0, 1, 2};
    auto indexBuffer = Buffer::Create(
        Context, {.Name = "Indices", .Size = sizeof(indices), .Usage = BufferUsage::Index});
    indexBuffer->UploadSync(
        std::span<const u8>(reinterpret_cast<const u8*>(indices.data()), sizeof(indices)));

    // Host-fill both commands except instanceCount, which the compute pass writes.
    const std::array<IndexedIndirectCommand, 2> commands{
        IndexedIndirectCommand{.IndexCount = 3,
                               .InstanceCount = 99, // overwritten to 1 by compute
                               .FirstIndex = 0,
                               .VertexOffset = 0,
                               .FirstInstance = survivorInstance},
        IndexedIndirectCommand{.IndexCount = 3,
                               .InstanceCount = 99, // overwritten to 0 by compute
                               .FirstIndex = 0,
                               .VertexOffset = 0,
                               .FirstInstance = culledInstance},
    };
    auto indirectBuffer =
        Buffer::Create(Context, {.Name = "Indirect Commands",
                                 .Size = sizeof(commands),
                                 .Usage = BufferUsage::Indirect | BufferUsage::Storage |
                                          BufferUsage::TransferDst});
    indirectBuffer->UploadSync(
        std::span<const u8>(reinterpret_cast<const u8*>(commands.data()), sizeof(commands)));

    auto target =
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
    auto cullLayout = PipelineLayout::Create(
        Context, {.Name = "Cull Layout", .DescriptorSetLayouts = {cullSetLayout}});
    auto cullPipeline = ComputePipeline::Create(
        Context,
        {.Name = "Cull Pipeline",
         .PipelineLayout = cullLayout,
         .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullShader->Get()->Module}});
    auto cullSet = DescriptorSet::Create(Context, {.Name = "Cull Set", .Layout = cullSetLayout});
    cullSet->Write(0, indirectBuffer);

    // --- Graphics pipeline: instance-rate color, no descriptors. ---
    auto drawLayout = PipelineLayout::Create(Context, {.Name = "Indirect Draw Layout"});
    const vk::Pipeline drawPipeline = CreateInstancePipeline(
        Context, GetVkPipelineLayout(*drawLayout), GetVkShaderModule(*vertShader->Get()->Module),
        vertShader->Get()->Module->GetEntryPoint().c_str(),
        GetVkShaderModule(*fragShader->Get()->Module),
        fragShader->Get()->Module->GetEntryPoint().c_str(), vk::Format::eR8G8B8A8Unorm);

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
                             .FirstSet = 1,
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
                        c.GetNative().CommandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
                                                                 drawPipeline);
                        c.SetViewport({0, 0}, {size, size});
                        c.SetScissor({0, 0}, {size, size});
                        c.GetNative().CommandBuffer.bindVertexBuffers(
                            1, GetVkBuffer(*instanceBuffer), {0});
                        c.BindIndexBuffer(indexBuffer, IndexType::U32);
                        c.DrawIndexedIndirect(ctx.ResolvedBuffer(indirectId), 0, 2,
                                              sizeof(IndexedIndirectCommand));
                    });

            auto compiled = graph.Compile();
            compiled->Execute(
                cmd,
                std::array{RenderGraph::ImportBinding{.Id = targetId, .View = targetView},
                           RenderGraph::ImportBinding{.Id = indirectId, .Buffer = indirectBuffer}});
        });

    // The raw pipeline outlives the frame's GPU work; wait before dropping it.
    Context.WaitIdle();
    GetVkDevice(Context).destroyPipeline(drawPipeline);

    const vector<u8> pixels = target->Download();
    CHECK(Test::PixelsMatch(pixels, expectedColor));
}

// BindlessRegistry cases that reach the Vulkan backend: register an image view and a sampler, bind
// set 0, and sample the registered texture in a draw, proving the registry's descriptor writes and
// Bind() produce a working binding; and an immediate submit keeping released slots while a frame
// slot is in flight, staged by unsignalling that slot's fence directly. The backend-neutral slot
// lifecycle cases are bindless_lifecycle.cpp.

#include <array>
#include <string>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>
#include <Renderer/SynchronizationFrame.h>
#include <Renderer/Backend/Vulkan/TypeMapping.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Renderer/Backend/Vulkan/Natives.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    // One slot in each slot-keyed array a release defers, plus a material range, all released at
    // once — the drops a renderer resize or teardown makes between frames.
    struct SlotSet
    {
        TextureHandle Texture;
        SamplerHandle Sampler;
        StorageBufferHandle StorageBuffer;
        MaterialHandle Material;
    };

    SlotSet RegisterSlotSet(Context& context)
    {
        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const Ref<Image> image = Image::Create(context, {
                                                            .Name = "Reclaim Texture",
                                                            .Extent = {Size, Size, 1},
                                                            .Format = Format::RGBA8Unorm,
                                                            .Usage = ImageUsage::Sampled,
                                                        });
        const Ref<Sampler> sampler = Sampler::Create(context, {.Name = "Reclaim Sampler"});
        const Ref<Buffer> buffer = Buffer::Create(context, {
                                                               .Name = "Reclaim Buffer",
                                                               .Size = 256,
                                                               .Usage = BufferUsage::Storage,
                                                           });
        constexpr std::array<std::byte, 16> block{};
        return SlotSet{
            .Texture = bindless.Register(
                ImageView::Create(context, {.Name = "Reclaim Texture View", .Image = image})),
            .Sampler = bindless.Register(sampler),
            .StorageBuffer = bindless.Register(buffer),
            .Material = bindless.RegisterMaterial(block),
        };
    }

    void ReleaseSlotSet(const Context& context, const SlotSet& set)
    {
        BindlessRegistry& bindless = context.GetBindlessRegistry();
        bindless.Release(set.Texture);
        bindless.Release(set.Sampler);
        bindless.Release(set.StorageBuffer);
        bindless.Release(set.Material);
    }

    bool SameFreeSlots(const BindlessCapacity& a, const BindlessCapacity& b)
    {
        return a.Textures == b.Textures && a.Samplers == b.Samplers &&
               a.StorageBuffers == b.StorageBuffers && a.Materials == b.Materials &&
               a.MaterialBlocks == b.MaterialBlocks;
    }

    // Builds a pipeline that draws a fullscreen triangle sampling a single
    // bindless-registered texture/sampler pair selected by push constants.
    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                               const Ref<ShaderModule>& vertexModule,
                                               const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(
            context, {
                         .Name = "Bindless Sample Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
                             },
                     });

        return GraphicsPipeline::Create(
            context, {
                         .Name = "Bindless Sample Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                         .PipelineLayout = outLayout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                                 {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
                             },
                     });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: register, bind, and sample a registered texture")
{
    // Exactly representable in RGBA8Unorm: blue, fully opaque.
    constexpr std::array<u8, 4> expected = {0, 0, 255, 255};

    const auto sourceImage =
        Image::Create(Context, {
                                   .Name = "Bindless Source",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
                               });
    auto sourceView =
        ImageView::Create(Context, {.Name = "Bindless Source View", .Image = sourceImage});

    const auto outputImage =
        Image::Create(Context, {
                                   .Name = "Bindless Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Bindless Output View", .Image = outputImage});

    const auto sampler = Sampler::Create(Context, {
                                                      .Name = "Bindless Test Sampler",
                                                      .AddressModeU = AddressMode::ClampToEdge,
                                                      .AddressModeV = AddressMode::ClampToEdge,
                                                      .AddressModeW = AddressMode::ClampToEdge,
                                                  });

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F44});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module,
                                         fragmentAsset->Get()->Module);

    auto& bindless = Context.GetBindlessRegistry();
    const TextureHandle textureHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);

    CHECK(textureHandle.IsValid());
    CHECK(samplerHandle.IsValid());

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId sourceId = graph.Import("Source");
            const ResourceId outputId = graph.Import("Output");

            graph.AddPass("Clear Source")
                .Color({
                    .Resource = sourceId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 1.0f, .A = 1.0f},
                })
                .Execute([](PassContext&) {});

            graph.AddPass("Sample Bindless")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Sample(sourceId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& cmd = ctx.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.SetViewport({0, 0}, {Size, Size});
                        cmd.SetScissor({0, 0}, {Size, Size});
                        bindless.Bind(cmd);
                        cmd.PushConstants(SamplePushConstants{
                            .TextureIndex = textureHandle.Index,
                            .SamplerIndex = samplerHandle.Index,
                        });
                        cmd.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding bindings[] = {
                {.Id = sourceId, .View = sourceView},
                {.Id = outputId, .View = outputView},
            };
            graph.Compile()->Execute(cmd, bindings);
        });

    const vector<u8> pixels = outputImage->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(textureHandle);
    bindless.Release(samplerHandle);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: an immediate submit keeps released slots while a frame "
                  "slot is in flight")
{
    // Unsignaling a slot other than the current one stands in for a submitted frame still
    // executing, without stalling the immediate submit, which waits only the current slot.
    Context::Native& native = Context.GetNative();
    REQUIRE(native.MaxFramesInFlight >= 2);
    const u32 inFlightSlot = (native.CurrentFrameInFlight + 1) % native.MaxFramesInFlight;
    const vk::Fence fence =
        GetVkFence(native.SynchronizationFrames[inFlightSlot].GetInFlightFence());
    VK_ASSERT(GetVkDevice(Context).resetFences(fence), "failed to reset fence");

    const BindlessRegistry& bindless = Context.GetBindlessRegistry();
    const BindlessCapacity before = bindless.GetFreeSlots();
    ReleaseSlotSet(Context, RegisterSlotSet(Context));
    const BindlessCapacity released = bindless.GetFreeSlots();

    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(SameFreeSlots(bindless.GetFreeSlots(), released));

    native.LockedSubmit(native.GraphicsQueue, vk::SubmitInfo{}, fence);
    native.SynchronizationFrames[inFlightSlot].GetInFlightFence().Wait();
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(SameFreeSlots(bindless.GetFreeSlots(), before));
}

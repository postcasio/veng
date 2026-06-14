// BindlessRegistry cases: exercises the global bindless
// descriptor subsystem (set 0) end to end.
//
//   1. Register an image view and a sampler, bind set 0, and sample the
//      registered texture in a draw — proves the registry's descriptor
//      writes and Bind() produce a working binding through the
//      texture2D[]/sampler[] arrays in bindless_sample.frag.
//   2. Register, release, and re-register through a sequence of
//      BeginFrame/EndFrame cycles — proves a released slot is not reused
//      while still possibly in flight, and is reclaimed once
//      Context::AcquireNextFrame() has cycled back to the frame-in-flight
//      index the release happened on (SlotArray::PendingRelease, mirroring
//      the per-frame retire bins).

#include <array>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/ShaderAsset.h>
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

    // Builds a pipeline that draws a fullscreen triangle sampling a single
    // bindless-registered texture/sampler pair selected by push constants.
    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                               const Ref<Shader>& vertexModule, const Ref<Shader>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(context, {
            .Name = "Bindless Sample Layout",
            .PushConstantRanges = {
                PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
            },
        });

        return GraphicsPipeline::Create(context, {
            .Name = "Bindless Sample Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = outLayout,
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
            },
        });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "bindless registry: register, bind, and sample a registered texture")
{
    // Exactly representable in RGBA8Unorm: blue, fully opaque.
    constexpr std::array<u8, 4> expected = {0, 0, 255, 255};

    auto sourceImage = Image::Create(Context, {
        .Name = "Bindless Source",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
    });
    auto sourceView = ImageView::Create(Context, {.Name = "Bindless Source View", .Image = sourceImage});

    auto outputImage = Image::Create(Context, {
        .Name = "Bindless Output",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
    });
    auto outputView = ImageView::Create(Context, {.Name = "Bindless Output View", .Image = outputImage});

    auto sampler = Sampler::Create(Context, {
        .Name = "Bindless Test Sampler",
        .AddressModeU = AddressMode::ClampToEdge,
        .AddressModeV = AddressMode::ClampToEdge,
        .AddressModeW = AddressMode::ClampToEdge,
    });

    AssetManager assets(Context);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<ShaderAsset>> vertexAsset = assets.LoadSync<ShaderAsset>(AssetId{8002});
    const AssetResult<AssetHandle<ShaderAsset>> fragmentAsset = assets.LoadSync<ShaderAsset>(AssetId{8004});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module, fragmentAsset->Get()->Module);

    auto& bindless = Context.GetBindlessRegistry();
    const TextureHandle textureHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);

    CHECK(textureHandle.IsValid());
    CHECK(samplerHandle.IsValid());

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        RenderGraph graph;

        graph.AddPass("Clear Source")
            .Color({
                .View = sourceView,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 1.0f, 1.0f},
            })
            .Execute([](CommandBuffer&) {});

        graph.AddPass("Sample Bindless")
            .Color({
                .View = outputView,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Sample(sourceView)
            .Execute([&](CommandBuffer& cmd)
            {
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

        graph.Execute(cmd);
    });

    const vector<u8> pixels = outputImage->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(textureHandle);
    bindless.Release(samplerHandle);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "bindless registry: released slots are not reused until they cycle through every frame in flight")
{
    auto& bindless = Context.GetBindlessRegistry();

    auto MakeView = [&](string_view name)
    {
        auto image = Image::Create(Context, {
            .Name = string(name),
            .Extent = {Size, Size, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::Sampled,
        });

        return ImageView::Create(Context, {.Name = string(name) + " View", .Image = image});
    };

    auto viewA = MakeView("Slot A");
    auto viewB = MakeView("Slot B");
    auto viewC = MakeView("Slot C");
    auto viewD = MakeView("Slot D");

    const TextureHandle handleA = bindless.Register(viewA);
    const TextureHandle handleB = bindless.Register(viewB);

    bindless.Release(handleA);

    // The slot freed by Release(handleA) must not be handed out immediately —
    // it's still potentially sampled by in-flight frames.
    const TextureHandle handleC = bindless.Register(viewC);
    CHECK(handleC.Index != handleA.Index);

    // Cycle through every frame-in-flight index so AcquireNextFrame() (called
    // from BeginFrame/EndFrame) processes handleA's deferred release.
    const u32 framesInFlight = Context.GetMaxFramesInFlight();
    for (u32 i = 0; i < framesInFlight + 1; i++)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }

    const TextureHandle handleD = bindless.Register(viewD);
    CHECK(handleD.Index == handleA.Index);

    bindless.Release(handleB);
    bindless.Release(handleC);
    bindless.Release(handleD);
}

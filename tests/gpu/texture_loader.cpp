// Texture load + bindless sample test: cooks the texture
// fixture pack in-process, mounts it, LoadSync<Texture>s it through
// AssetManager, and samples the result via the bindless registry — the
// end-to-end proof for the first full asset-type vertical slice.

#include <array>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
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

    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                               const Ref<ShaderModule>& vertexModule,
                                               const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(
            context, {
                         .Name = "Texture Loader Sample Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
                             },
                     });

        return GraphicsPipeline::Create(
            context, {
                         .Name = "Texture Loader Sample Pipeline",
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
                  "texture loader: cook, mount, LoadSync, and sample bindlessly")
{
    // Exactly representable in RGBA8Unorm: the fixture's solid 4x4 color.
    constexpr std::array<u8, 4> expected = {200, 80, 40, 255};

    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_texture.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Texture>> handle = assets.LoadSync<Texture>(AssetId{0x7D1});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const AssetHandle<Texture>& textureHandle = *handle;
    const Texture& texture = *textureHandle.Get();
    CHECK(texture.GetFormat() == Format::RGBA8Unorm);
    CHECK(texture.GetExtent() == uvec2{Size, Size});
    CHECK(texture.GetHandle().IsValid());
    CHECK(texture.GetSamplerHandle().IsValid());

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Texture Loader Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Texture Loader Output View", .Image = outputImage});

    AssetManager shaderAssets(Context, Tasks, Types);
    const VoidResult shaderMountResult = shaderAssets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(shaderMountResult.has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset =
        shaderAssets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset =
        shaderAssets.LoadSync<Shader>(AssetId{0x1F44});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module,
                                         fragmentAsset->Get()->Module);

    auto& bindless = Context.GetBindlessRegistry();

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId outputId = graph.Import("Output");

            graph.AddPass("Sample Loaded Texture")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& cmd = ctx.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.SetViewport({0, 0}, {Size, Size});
                        cmd.SetScissor({0, 0}, {Size, Size});
                        bindless.Bind(cmd);
                        cmd.PushConstants(SamplePushConstants{
                            .TextureIndex = texture.GetHandle().Index,
                            .SamplerIndex = texture.GetSamplerHandle().Index,
                        });
                        cmd.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding binding{.Id = outputId, .View = outputView};
            graph.Compile()->Execute(cmd, {&binding, 1});
        });

    const vector<u8> pixels = outputImage->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "texture loader: async Load returns pending, becomes resident after a pump")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_pack.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_gpu_texture_async.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

    // The async upload path records onto the per-worker transfer pools.
    Context.InitializeTransferPools(Tasks);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // Load returns immediately with a not-yet-resident handle.
    const AssetHandle<Texture> handle = assets.Load<Texture>(AssetId{0x7D1});
    CHECK_FALSE(handle.IsLoaded());

    // The decode + upload run on the task system; the finalize lands on the main
    // thread. Drain both until the handle is resident.
    for (int i = 0; i < 100 && !handle.IsLoaded(); ++i)
    {
        Tasks.WaitForAll();
        Tasks.PumpMainThread();
        assets.PumpFinalizes();
    }

    REQUIRE(handle.IsLoaded());
    CHECK(handle.Get()->GetFormat() == Format::RGBA8Unorm);
    CHECK(handle.Get()->GetExtent() == uvec2{Size, Size});
    CHECK(handle.Get()->GetHandle().IsValid());
    CHECK(handle.Get()->GetSamplerHandle().IsValid());

    std::filesystem::remove(outArchive);
}

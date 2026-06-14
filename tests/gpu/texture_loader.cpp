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
#include <Veng/Renderer/ShaderAsset.h>
#include <Veng/Renderer/Texture.h>
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
                                               const Ref<Shader>& vertexModule, const Ref<Shader>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(context, {
            .Name = "Texture Loader Sample Layout",
            .PushConstantRanges = {
                PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
            },
        });

        return GraphicsPipeline::Create(context, {
            .Name = "Texture Loader Sample Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = outLayout,
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
            },
        });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "texture loader: cook, mount, LoadSync, and sample bindlessly")
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

    AssetManager assets(Context);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Texture>> handle = assets.LoadSync<Texture>(AssetId{2001});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const AssetHandle<Texture>& textureHandle = *handle;
    const Texture& texture = *textureHandle.Get();
    CHECK(texture.GetFormat() == Format::RGBA8Unorm);
    CHECK(texture.GetExtent() == uvec2{Size, Size});
    CHECK(texture.GetHandle().IsValid());
    CHECK(texture.GetSamplerHandle().IsValid());

    auto outputImage = Image::Create(Context, {
        .Name = "Texture Loader Output",
        .Extent = {Size, Size, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
    });
    auto outputView = ImageView::Create(Context, {.Name = "Texture Loader Output View", .Image = outputImage});

    AssetManager shaderAssets(Context);
    const VoidResult shaderMountResult = shaderAssets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(shaderMountResult.has_value());

    const AssetResult<AssetHandle<ShaderAsset>> vertexAsset = shaderAssets.LoadSync<ShaderAsset>(AssetId{8002});
    const AssetResult<AssetHandle<ShaderAsset>> fragmentAsset = shaderAssets.LoadSync<ShaderAsset>(AssetId{8004});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout, vertexAsset->Get()->Module, fragmentAsset->Get()->Module);

    auto& bindless = Context.GetBindlessRegistry();

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        RenderGraph graph;

        graph.AddPass("Sample Loaded Texture")
            .Color({
                .View = outputView,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Execute([&](CommandBuffer& cmd)
            {
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

        graph.Execute(cmd);
    });

    const vector<u8> pixels = outputImage->Download();

    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    std::filesystem::remove(outArchive);
}

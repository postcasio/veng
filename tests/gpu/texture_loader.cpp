// Texture load + bindless sample test (planset-5 plan 06): cooks the texture
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
#include <Veng/Renderer/Shader.h>
#include <Veng/Renderer/Texture.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 kSize = 4;

    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    Ref<GraphicsPipeline> CreateSamplePipeline(Context& context, Ref<PipelineLayout>& outLayout)
    {
        const auto vertexShader = Shader::Create(context, {
            .Name = "fullscreen.vert",
            .Path = path(GPU_SHADER_DIR) / "fullscreen.vert.spv",
        });
        VE_ASSERT(vertexShader, "{}", vertexShader.error());

        const auto fragmentShader = Shader::Create(context, {
            .Name = "bindless_sample.frag",
            .Path = path(GPU_SHADER_DIR) / "bindless_sample.frag.spv",
        });
        VE_ASSERT(fragmentShader, "{}", fragmentShader.error());

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
                {.Stage = ShaderStage::Vertex, .Module = vertexShader.value()},
                {.Stage = ShaderStage::Fragment, .Module = fragmentShader.value()},
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
    CHECK(texture.GetExtent() == uvec2{kSize, kSize});
    CHECK(texture.GetHandle().IsValid());
    CHECK(texture.GetSamplerHandle().IsValid());

    auto outputImage = Image::Create(Context, {
        .Name = "Texture Loader Output",
        .Extent = {kSize, kSize, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
    });
    auto outputView = ImageView::Create(Context, {.Name = "Texture Loader Output View", .Image = outputImage});

    Ref<PipelineLayout> layout;
    auto pipeline = CreateSamplePipeline(Context, layout);

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
                cmd.SetViewport({0, 0}, {kSize, kSize});
                cmd.SetScissor({0, 0}, {kSize, kSize});
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

    REQUIRE(pixels.size() == static_cast<size_t>(kSize) * kSize * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    std::filesystem::remove(outArchive);
}

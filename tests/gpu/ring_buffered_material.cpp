// Ring-buffered material parameter cases: prove the N-buffered, host-mapped
// per-material block buffer (set 0, binding 4) is written safely per frame and
// read correctly through the per-frame dynamic offset.
//
//   1. Per-frame mutation: a material's param changes every frame; the draw
//      that frame must read that frame's value (no tearing, no stale read from
//      a prior in-flight region).
//   2. Write-once stability: a material registered once and never updated reads
//      the same value on every frame across the in-flight window (the value
//      flushed to every region).
//
// Each frame draws a fullscreen triangle whose fragment shader loads the
// material block at materialIndex * stride from the dynamic binding and outputs
// the param as color; the value is read back from the off-screen target.

#include <array>
#include <cstdlib>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    struct MaterialPush
    {
        u32 MaterialIndex;
    };

    Ref<GraphicsPipeline> CreateMaterialPipeline(Context& context, Ref<PipelineLayout>& outLayout,
                                                 const Ref<ShaderModule>& vertexModule,
                                                 const Ref<ShaderModule>& fragmentModule)
    {
        outLayout = PipelineLayout::Create(
            context, {
                         .Name = "Material Param Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<MaterialPush>(ShaderStage::Fragment),
                             },
                     });

        return GraphicsPipeline::Create(
            context, {
                         .Name = "Material Param Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                         .PipelineLayout = outLayout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vertexModule},
                                 {.Stage = ShaderStage::Fragment, .Module = fragmentModule},
                             },
                     });
    }

    // A block whose first 16 bytes hold one float4 the test shader reads.
    std::array<std::byte, 16> MakeBlock(const vec4& value)
    {
        std::array<std::byte, 16> block{};
        std::memcpy(block.data(), &value, sizeof(value));
        return block;
    }

    // Renders one frame sampling the material block and returns the center pixel.
    std::array<u8, 4> RenderFrame(Context& context, BindlessRegistry& bindless,
                                  const Ref<GraphicsPipeline>& pipeline,
                                  const Ref<Image>& outputImage, const Ref<ImageView>& outputView,
                                  MaterialHandle material)
    {
        CommandBuffer& cmd = context.BeginFrame();

        RenderGraph graph(context);
        const ResourceId outputId = graph.Import("Output");

        graph.AddPass("Draw Material Param")
            .Color({
                .Resource = outputId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
            })
            .Execute(
                [&](PassContext& ctx)
                {
                    CommandBuffer& passCmd = ctx.Cmd();
                    passCmd.BindPipeline(pipeline);
                    passCmd.SetViewport({0, 0}, {Size, Size});
                    passCmd.SetScissor({0, 0}, {Size, Size});
                    bindless.Bind(passCmd);
                    // Fold the current frame's region base into the selector so the
                    // shader's index * stride load reads this frame's region.
                    passCmd.PushConstants(MaterialPush{
                        .MaterialIndex = bindless.GetCurrentFrameBase() + material.Index});
                    passCmd.DrawFullscreenTriangle();
                });

        const RenderGraph::ImportBinding bindings[] = {{outputId, outputView}};
        graph.Compile()->Execute(cmd, bindings);

        context.EndFrame();
        context.WaitIdle();

        const vector<u8> pixels = outputImage->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
        return {pixels[0], pixels[1], pixels[2], pixels[3]};
    }

    // Renders to RGBA8Unorm, so a [0,1] float channel quantizes to ~round(c*255);
    // allow a 1-LSB tolerance for the device's rounding of the sRGB-free unorm
    // store.
    bool ChannelNear(u8 actual, f32 expected)
    {
        const i32 want = static_cast<i32>(expected * 255.0f + 0.5f);
        return std::abs(static_cast<i32>(actual) - want) <= 1;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ring-buffered material: a per-frame param change reads its own value each frame")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ring Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ring Output View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    // A distinct value per frame, spanning more than framesInFlight frames so a
    // stale region from a prior in-flight frame would be caught as a wrong pixel.
    const std::array<vec4, 6> perFrame = {
        vec4{0.2f, 0.0f, 0.0f, 1.0f}, vec4{0.4f, 0.0f, 0.0f, 1.0f}, vec4{0.6f, 0.0f, 0.0f, 1.0f},
        vec4{0.8f, 0.0f, 0.0f, 1.0f}, vec4{0.1f, 0.0f, 0.0f, 1.0f}, vec4{0.9f, 0.0f, 0.0f, 1.0f},
    };

    const auto initial = MakeBlock(perFrame[0]);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(initial));
    CHECK(material.IsValid());

    for (usize i = 0; i < perFrame.size(); i++)
    {
        // Update before recording this frame; the draw must read this value, not
        // a value left in the region by an earlier in-flight frame.
        const auto block = MakeBlock(perFrame[i]);
        bindless.UpdateMaterial(material, std::span<const std::byte>(block));

        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material);

        CHECK(ChannelNear(pixel[0], perFrame[i].x));
    }

    bindless.Release(material);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "ring-buffered material: a write-once material is stable across the in-flight window")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    Ref<PipelineLayout> layout;
    auto pipeline = CreateMaterialPipeline(Context, layout, vertexAsset->Get()->Module,
                                           fragmentAsset->Get()->Module);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Ring Output Stable",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Ring Output Stable View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    const vec4 value{0.0f, 0.5f, 0.0f, 1.0f};
    const auto block = MakeBlock(value);
    const MaterialHandle material = bindless.RegisterMaterial(std::span<const std::byte>(block));
    CHECK(material.IsValid());

    // Never updated again: the registered value must have flushed to every region
    // over the first framesInFlight frames, so every later frame reads it.
    const u32 frames = Context.GetMaxFramesInFlight() + 3;
    for (u32 i = 0; i < frames; i++)
    {
        const std::array<u8, 4> pixel =
            RenderFrame(Context, bindless, pipeline, outputImage, outputView, material);

        CHECK(ChannelNear(pixel[1], value.y));
    }

    bindless.Release(material);
}

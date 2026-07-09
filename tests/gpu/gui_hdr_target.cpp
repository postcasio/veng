// Gui HDR render-target round-trip: builds an imperative Gui::Document whose Panel root carries a
// background color above 1.0 (glowing cyan, set imperatively), drives it through the GuiScenePass
// render-to-texture sink into a persistent RGBA16Sfloat Gui::RenderTarget, then samples that target
// through its bindless GetOutputHandle() in a downstream fullscreen pass — a cross-graph handoff, no
// graph-inserted barrier. The producer's own PrepareForAccess is the only thing that leaves the
// target shader-readable before the sampler runs, so the downstream output reading back the emissive
// color proves (a) the ordering left the target shader-read before the sample and (b) the >1.0 texel
// round-tripped through the half-float target and the handle unclamped. Runs under the validation
// gate, so a missing or wrong layout transition across the handoff fails it.

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Style.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <Renderer/Passes/GuiScenePass.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};

    // The test shader pack's bindless-sample pipeline: fullscreen.vert + bindless_sample.frag,
    // selecting a registered texture/sampler pair by push constants and returning the sampled
    // texel unclamped (float4 out) — so a >1.0 sampled component survives into the half-float
    // output attachment.
    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    // Decodes one RGBA16Sfloat texel to a linear vec4.
    vec4 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec4(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]), glm::unpackHalf1x16(halves[base + 3]));
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr target: an emissive document renders to a persistent RGBA16Sfloat target "
    "a downstream pass samples above 1.0")
{
    AssetManager assets(Context, Tasks, Types);
    // The GuiScenePass gui shaders come from the auto-mounted core pack; the downstream sample
    // pipeline uses the test shader pack's fullscreen.vert + bindless_sample.frag.
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F44});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    // An imperative document: a single Panel root filling the extent, its background an HDR cyan
    // whose green/blue components are 4.0 — well above the 1.0 an 8-bit target would clamp to.
    constexpr vec4 emissive{0.0f, 4.0f, 4.0f, 1.0f};
    Gui::Document document;
    Gui::Style rootStyle;
    rootStyle.Width = Gui::Length::Points(static_cast<f32>(Extent.x));
    rootStyle.Height = Gui::Length::Points(static_cast<f32>(Extent.y));
    rootStyle.Background = emissive;
    document.SetStyle(document.Root(), rootStyle);
    document.Solve(vec2(static_cast<f32>(Extent.x), static_cast<f32>(Extent.y)));

    Gui::DrawList list;
    document.Build(list);

    // The persistent HDR target the document renders into and the downstream pass samples.
    const Unique<Gui::RenderTarget> target = Gui::RenderTarget::Create({
        .Context = Context,
        .Extent = Extent,
        .Name = "Gui HDR Target",
    });
    CHECK(target->GetFormat() == Format::RGBA16Sfloat);
    CHECK(target->GetOutputHandle().IsValid());

    const Unique<GuiScenePass> pass = GuiScenePass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = Extent,
        .OutputFormat = Format::RGBA16Sfloat,
    });
    pass->SetDrawList(list);

    // The downstream fullscreen pass samples the target's bindless handle into its own output.
    const Ref<Image> outputImage =
        Image::Create(Context, {
                                   .Name = "Gui HDR Sample Output",
                                   .Extent = {Extent.x, Extent.y, 1},
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    const Ref<ImageView> outputView =
        ImageView::Create(Context, {.Name = "Gui HDR Sample Output View", .Image = outputImage});

    const Ref<Sampler> sampler =
        Sampler::Create(Context, {
                                     .Name = "Gui HDR Sampler",
                                     .MagFilter = Filter::Nearest,
                                     .MinFilter = Filter::Nearest,
                                     .AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge,
                                 });
    const SamplerHandle samplerHandle = Context.GetBindlessRegistry().Register(sampler);

    const Ref<PipelineLayout> sampleLayout = PipelineLayout::Create(
        Context, {
                     .Name = "Gui HDR Sample Layout",
                     .PushConstantRanges = {PushConstantRange::Of<SamplePushConstants>(
                         ShaderStage::Fragment)},
                 });
    const Ref<GraphicsPipeline> samplePipeline = GraphicsPipeline::Create(
        Context,
        {
            .Name = "Gui HDR Sample Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA16Sfloat}},
            .PipelineLayout = sampleLayout,
            .ShaderStages =
                {
                    {.Stage = ShaderStage::Vertex, .Module = vertexAsset->Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = fragmentAsset->Get()->Module},
                },
        });

    // Producer then consumer in one command buffer: RenderToTarget records the document into the
    // HDR target and transitions it to a sampleable layout; the downstream graph then samples the
    // target's handle — the same cross-graph, single-queue, no-semaphore handoff a material read
    // performs. Only the producer's own barrier makes the sample legal.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            pass->RenderToTarget(cmd, *target);

            RenderGraph graph(Context);
            const ResourceId outputId = graph.Import("Gui HDR Sample");
            graph.AddPass("Sample Gui HDR")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& passCmd = ctx.Cmd();
                        passCmd.BindPipeline(samplePipeline);
                        passCmd.SetViewport({0, 0}, Extent);
                        passCmd.SetScissor({0, 0}, Extent);
                        Context.GetBindlessRegistry().Bind(passCmd);
                        passCmd.PushConstants(SamplePushConstants{
                            .TextureIndex = target->GetOutputHandle().Index,
                            .SamplerIndex = samplerHandle.Index,
                        });
                        passCmd.DrawFullscreenTriangle();
                    });
            const RenderGraph::ImportBinding binding{.Id = outputId, .View = outputView};
            graph.Compile()->Execute(cmd, {&binding, 1});
        });

    // The target holds the emissive color: the document's background survived into the half-float
    // producer target unclamped.
    const vector<u8> targetPixels = target->GetOutput()->GetImage()->Download();
    REQUIRE(targetPixels.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vec4 targetCenter = DecodeTexel(targetPixels, Extent.x, Extent.x / 2, Extent.y / 2);
    CHECK(targetCenter.g > 1.0f);
    CHECK(targetCenter.b > 1.0f);

    // The downstream sample reads that same emissive color back — the target was rendered and left
    // shader-readable when the sampler ran, and the >1.0 value round-tripped through the handle.
    const vector<u8> outputPixels = outputImage->Download();
    REQUIRE(outputPixels.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
    const vec4 sampled = DecodeTexel(outputPixels, Extent.x, Extent.x / 2, Extent.y / 2);
    CHECK(sampled.g > 1.0f);
    CHECK(sampled.b > 1.0f);
    CHECK(sampled.r < 0.5f);
    CHECK(sampled.g == doctest::Approx(emissive.g).epsilon(0.05));
    CHECK(sampled.b == doctest::Approx(emissive.b).epsilon(0.05));

    Context.GetBindlessRegistry().Release(samplerHandle);
}

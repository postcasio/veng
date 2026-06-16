// SceneRenderer cases: prove the renderer shell produces a valid, sampleable
// output of the requested extent/format from a Scene + Camera, and that Resize
// recreates that output at a new extent while keeping it sampleable.
//
// The scene is a primitive cube with a MeshRenderer (no material, so the forward
// pass clears and draws nothing) — enough to exercise Execute end to end. The
// proof of sampleability is a fullscreen pass that reads GetOutput() through the
// bindless set and writes an RGBA8 target the test downloads: the renderer's
// cleared output (the forward clear color) shows through.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    // Builds a cube scene with a MeshRenderer and returns the owning mesh so the
    // caller keeps it resident for the renderer's lifetime.
    Ref<Mesh> PopulateCubeScene(Context& context, AssetManager& assets, Scene& scene)
    {
        const Ref<Mesh> cube = Mesh::Create(context, Primitives::Cube(1.0f), "Test Cube");

        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

        return cube;
    }

    // Samples `output` through the bindless set into a fresh RGBA8 target and
    // returns the downloaded pixels — the proof the output is a sampleable view.
    vector<u8> SampleThroughBindless(Context& context, AssetManager& assets,
                                     const Ref<ImageView>& output, uvec2 extent)
    {
        const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
        const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F44});
        REQUIRE(vertexAsset.has_value());
        REQUIRE(fragmentAsset.has_value());

        const Ref<PipelineLayout> layout = PipelineLayout::Create(context, {
            .Name = "Scene Sample Layout",
            .PushConstantRanges = {
                PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
            },
        });

        const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(context, {
            .Name = "Scene Sample Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = layout,
            .ShaderStages = {
                {.Stage = ShaderStage::Vertex, .Module = vertexAsset->Get()->Module},
                {.Stage = ShaderStage::Fragment, .Module = fragmentAsset->Get()->Module},
            },
        });

        const Ref<Sampler> sampler = Sampler::Create(context, {
            .Name = "Scene Sample Sampler",
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
        });

        const Ref<Image> target = Image::Create(context, {
            .Name = "Scene Sample Output",
            .Extent = {extent.x, extent.y, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
        });
        const Ref<ImageView> targetView = ImageView::Create(context, {.Name = "Scene Sample Output View", .Image = target});

        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const TextureHandle textureHandle = bindless.Register(output);
        const SamplerHandle samplerHandle = bindless.Register(sampler);

        context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            RenderGraph graph(context);
            const ResourceId sourceId = graph.Import("Scene Output");
            const ResourceId outputId = graph.Import("Sample Output");

            graph.AddPass("Sample Scene")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                })
                .Sample(sourceId)
                .Execute([&](PassContext& ctx)
                {
                    CommandBuffer& passCmd = ctx.Cmd();
                    passCmd.BindPipeline(pipeline);
                    passCmd.SetViewport({0, 0}, extent);
                    passCmd.SetScissor({0, 0}, extent);
                    bindless.Bind(passCmd);
                    passCmd.PushConstants(SamplePushConstants{
                        .TextureIndex = textureHandle.Index,
                        .SamplerIndex = samplerHandle.Index,
                    });
                    passCmd.DrawFullscreenTriangle();
                });

            const RenderGraph::ImportBinding bindings[] = {
                {sourceId, output},
                {outputId, targetView},
            };
            graph.Compile()->Execute(cmd, bindings);
        });

        vector<u8> pixels = target->Download();

        bindless.Release(textureHandle);
        bindless.Release(samplerHandle);

        return pixels;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: Execute produces a sampleable output; Resize updates it")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    constexpr uvec2 initialExtent{64, 48};

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f),
                          static_cast<f32>(initialExtent.x) / static_cast<f32>(initialExtent.y), 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = initialExtent,
        .Settings = {},
    });

    // The output is a valid view of the requested extent/format.
    const Ref<ImageView> output = renderer->GetOutput();
    REQUIRE(output != nullptr);
    CHECK(output->GetImage()->GetWidth() == initialExtent.x);
    CHECK(output->GetImage()->GetHeight() == initialExtent.y);
    CHECK(output->GetImage()->GetFormat() == Context.GetOutputFormat());

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        renderer->Execute(cmd, Renderer::SceneView{*scene, camera, 0.0f});
    });

    // It is sampleable: a fullscreen pass reads it through bindless and produces a
    // non-degenerate frame (the forward clear color, opaque).
    const vector<u8> sampled = SampleThroughBindless(Context, assets, renderer->GetOutput(), initialExtent);
    REQUIRE(sampled.size() == static_cast<size_t>(initialExtent.x) * initialExtent.y * 4);
    CHECK(sampled[3] == 255); // alpha: the scene cleared to an opaque color

    // Resize recreates the output at a new extent; the prior Ref is stale, so
    // re-fetch. The new output samples cleanly at its own size.
    constexpr uvec2 resizedExtent{96, 32};
    renderer->Resize(resizedExtent);

    const Ref<ImageView> resized = renderer->GetOutput();
    REQUIRE(resized != nullptr);
    CHECK(resized.get() != output.get());
    CHECK(resized->GetImage()->GetWidth() == resizedExtent.x);
    CHECK(resized->GetImage()->GetHeight() == resizedExtent.y);

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        renderer->Execute(cmd, Renderer::SceneView{*scene, camera, 0.0f});
    });

    const vector<u8> resampled = SampleThroughBindless(Context, assets, renderer->GetOutput(), resizedExtent);
    REQUIRE(resampled.size() == static_cast<size_t>(resizedExtent.x) * resizedExtent.y * 4);
    CHECK(resampled[3] == 255);
}

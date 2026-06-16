// SceneRenderer cases. The first proves the renderer shell produces a valid,
// sampleable output of the requested extent/format from a Scene + Camera, that
// the deferred g-buffer images allocate with the contracted formats/usage, and
// that Resize recreates the output + g-buffer at a new extent. Its scene is a
// primitive cube with a materialless MeshRenderer (the geometry pass clears the
// g-buffer and draws nothing), and the proof of sampleability is a fullscreen
// pass reading GetOutput() through bindless into an RGBA8 target.
//
// The second (cooker-gated) is the deferred albedo oracle: it cooks a brick
// material on the core canonical layout, draws a cube through the SceneRenderer,
// downloads the albedo-blit output, and asserts spread sample points (a lit cube
// texel, a background texel) plus a whole-frame mean-luminance invariant — the
// automated correctness gate for the deferred plan.

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GBuffer.h>
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
        .Assets = assets,
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

    // The g-buffer images allocate at the renderer's extent with the fixed
    // contracted formats and usages.
    auto CheckGBuffer = [](const Ref<ImageView>& view, uvec2 extent, Format format, ImageUsage usage)
    {
        REQUIRE(view != nullptr);
        const Ref<Image>& image = view->GetImage();
        CHECK(image->GetWidth() == extent.x);
        CHECK(image->GetHeight() == extent.y);
        CHECK(image->GetFormat() == format);
        CHECK(HasFlag(image->GetUsage(), usage));
    };
    CheckGBuffer(renderer->GetAlbedoView(), initialExtent, GBuffer::AlbedoFormat, GBuffer::ColorUsage);
    CheckGBuffer(renderer->GetNormalView(), initialExtent, GBuffer::NormalFormat, GBuffer::ColorUsage);
    CheckGBuffer(renderer->GetDepthView(), initialExtent, GBuffer::DepthFormat, GBuffer::DepthUsage);
    const Ref<ImageView> albedoView = renderer->GetAlbedoView();

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

    // Resize also recreates every g-buffer image at the new extent (re-registering
    // their bindless handles); the prior views are stale.
    CHECK(renderer->GetAlbedoView().get() != albedoView.get());
    CheckGBuffer(renderer->GetAlbedoView(), resizedExtent, GBuffer::AlbedoFormat, GBuffer::ColorUsage);
    CheckGBuffer(renderer->GetNormalView(), resizedExtent, GBuffer::NormalFormat, GBuffer::ColorUsage);
    CheckGBuffer(renderer->GetDepthView(), resizedExtent, GBuffer::DepthFormat, GBuffer::DepthUsage);

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        renderer->Execute(cmd, Renderer::SceneView{*scene, camera, 0.0f});
    });

    const vector<u8> resampled = SampleThroughBindless(Context, assets, renderer->GetOutput(), resizedExtent);
    REQUIRE(resampled.size() == static_cast<size_t>(resizedExtent.x) * resizedExtent.y * 4);
    CHECK(resampled[3] == 255);
}

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <filesystem>

#include <glm/gtc/packing.hpp>

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Asset/Material.h>

namespace
{
    // One RGBA16F output texel decoded to a linear vec3 (the SceneRenderer output
    // is RGBA16Sfloat; the smoke path decodes it the same way).
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]),
                    glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: deferred albedo oracle — a brick cube blits its albedo, "
                  "the background shows the g-buffer clear, mean luminance is in range")
{
    RegisterBuiltinTypes(Types);

    // Cook the g-buffer fixture pack (brick material on the core canonical layout)
    // in-process and mount it; the core pack (auto-mounted) supplies the canonical
    // layout the brick vertex shader references and the blit shaders the renderer
    // loads.
    const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_gbuffer.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    const VoidResult cookResult = cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B}); // 9003
    REQUIRE(material.has_value());
    REQUIRE(material->IsLoaded());

    constexpr uvec2 extent{128, 128};

    // A cube filling the view, the brick material on its single submesh.
    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Oracle Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {},
    });

    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        renderer->Execute(cmd, Renderer::SceneView{*scene, camera, 0.0f});
    });

    const Ref<Image> output = renderer->GetOutput()->GetImage();
    const vector<u8> pixels = output->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8); // RGBA16F = 8 bytes/texel

    // The 1x1-color brick texture (linear (200, 80, 40)/255) sampled and written
    // through the sRGB G0 round-trip, blit back to the linear output. The center
    // covers a cube face; a corner is background (the G0 clear color).
    const vec3 center = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    const vec3 corner = DecodeTexel(pixels, extent.x, 2, 2);

    // Center is the brick albedo: red-dominant, distinctly not the clear color.
    CHECK(center.r > 0.5f);
    CHECK(center.r > center.g);
    CHECK(center.g > center.b);
    CHECK(center.r == doctest::Approx(200.0f / 255.0f).epsilon(0.05f));
    CHECK(center.g == doctest::Approx(80.0f / 255.0f).epsilon(0.08f));
    CHECK(center.b == doctest::Approx(40.0f / 255.0f).epsilon(0.10f));

    // Corner is the g-buffer albedo clear (0.05, 0.05, 0.08), round-tripped — dark
    // and clearly not the brick color.
    CHECK(corner.r < 0.2f);
    CHECK(corner.b >= corner.r); // the clear's blue tint survives

    // Whole-frame mean luminance: bounded between the dark background and the
    // brick albedo, catching a global error (a black g-buffer, a wiped albedo, a
    // gamma slip). The cube covers a large central fraction of the frame.
    f64 luminanceSum = 0.0;
    for (u32 y = 0; y < extent.y; ++y)
        for (u32 x = 0; x < extent.x; ++x)
        {
            const vec3 c = DecodeTexel(pixels, extent.x, x, y);
            luminanceSum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        }
    const f64 meanLuminance = luminanceSum / (static_cast<f64>(extent.x) * extent.y);
    CHECK(meanLuminance > 0.05);
    CHECK(meanLuminance < 0.45);

    std::filesystem::remove(outArchive);
}

#endif // GPU_GBUFFER_FIXTURE_DIR

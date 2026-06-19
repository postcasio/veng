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
#include <Veng/Renderer/ScenePass.h>
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
        renderer->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
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
        renderer->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
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

// The deferred-lighting oracle. A brick cube fills the view, its front face
// (world normal +Z) squarely toward the camera. The fixture material is a fully
// rough dielectric (roughness 1, metallic 0), so the Cook-Torrance result is
// diffuse-dominant. Three lighting setups render the same scene and assert
// qualitative properties of the deferred BRDF at known sample points:
//
//  - Light traveling along -Z (toward the front face): the front face is fully
//    lit (N·L = 1), so the center is bright, red-dominant brick — well above the
//    ambient-only term.
//  - Light traveling along -X (across the front face): N·L = 0 on the front face,
//    so the center reads the hemispheric ambient × albedo only — distinctly
//    darker than the lit case.
//  - No Light in the scene: the renderer's zero-intensity default, so the center
//    is the same flat-ambient term, never pure black.
//
// The brick albedo is linear (200, 80, 40)/255, sampled through the sRGB G0
// round-trip. The lighting pass's ambient is the hemispheric AmbientColor in
// deferred_lighting.frag gated by ORM.r (occlusion 1 here).
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: deferred lighting oracle — a lit face is bright, "
                  "an unlit face and the no-light default fall to the ambient term")
{
    constexpr vec3 BrickAlbedo{200.0f / 255.0f, 80.0f / 255.0f, 40.0f / 255.0f};

    RegisterBuiltinTypes(Types);

    // Cook the g-buffer fixture pack (brick material on the core canonical layout)
    // in-process and mount it; the core pack (auto-mounted) supplies the canonical
    // layout the brick vertex shader references and the lighting/blit shaders the
    // renderer loads.
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

    // A cube filling the view, the brick material on its single submesh, its front
    // face squarely toward the camera (world normal +Z).
    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Oracle Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // The batteries are off: this oracle pins the deferred lighting math (the
    // diffuse term, the hemispheric ambient fallback) in isolation. Shadows, SSAO,
    // and bloom modulate those exact values and carry their own dedicated property
    // tests; enabling them here would couple this oracle to the screen-space AO
    // term, which is not bit-stable on MoltenVK.
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    // Render the scene once and return the downloaded RGBA16F output.
    auto Render = [&]() -> vector<u8>
    {
        Context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            renderer->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
        });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    auto MeanLuminance = [&](const vector<u8>& pixels) -> f64
    {
        f64 sum = 0.0;
        for (u32 y = 0; y < extent.y; ++y)
            for (u32 x = 0; x < extent.x; ++x)
            {
                const vec3 c = DecodeTexel(pixels, extent.x, x, y);
                sum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
            }
        return sum / (static_cast<f64>(extent.x) * extent.y);
    };

    // Case 1 — light straight at the front face (N·L = 1): the center is fully lit.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f), // travels toward the front face
        .Color = vec3(1.0f, 1.0f, 1.0f),
        .Intensity = 1.0f,
    };

    f32 litCenterR = 0.0f;
    {
        const vector<u8> pixels = Render();
        const vec3 center = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
        const vec3 corner = DecodeTexel(pixels, extent.x, 2, 2);
        litCenterR = center.r;

        // Fully lit: the diffuse term dominates, so the center is bright and
        // red-dominant brick, well above the ambient-only term.
        CHECK(center.r > 0.4f);
        CHECK(center.r > center.g);
        CHECK(center.g > center.b);

        // The background (g-buffer clear, no geometry) lights from a zero normal,
        // so only the ambient survives — dark, far below the lit cube.
        CHECK(corner.r < 0.2f);

        const f64 lit = MeanLuminance(pixels);
        CHECK(lit > 0.05);
        CHECK(lit < 0.7);
    }

    // Case 2 — light across the front face (N·L = 0): the center falls to the
    // hemispheric ambient × albedo only.
    scene->Get<Light>(lightEntity).Direction = vec3(-1.0f, 0.0f, 0.0f);

    f32 ambientCenterR = 0.0f;
    {
        const vector<u8> pixels = Render();
        const vec3 center = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
        ambientCenterR = center.r;

        // The ambient term is AmbientColor.r (0.12) × albedo.r (≈0.78) ≈ 0.094.
        CHECK(center.r == doctest::Approx(0.12f * BrickAlbedo.r).epsilon(0.4f));
        // Distinctly darker than the lit case — the diffuse term contributed nothing.
        CHECK(center.r < litCenterR * 0.5f);
        CHECK(center.r > 0.0f);
    }

    // Case 3 — no Light in the scene: the renderer's zero-intensity default, so the
    // center is the same flat-ambient term, never pure black.
    scene->DestroyEntity(lightEntity);

    {
        const vector<u8> pixels = Render();
        const vec3 center = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);

        // Identical to the across-face case: only the ambient term survives.
        CHECK(center.r == doctest::Approx(ambientCenterR).epsilon(0.05f));
        // Flat-ambient, but not pure black.
        CHECK(center.r > 0.0f);
    }

    std::filesystem::remove(outArchive);
}

namespace
{
    // Cooks the brick g-buffer fixture pack in-process and mounts it, returning the
    // archive path the caller removes when done. The core pack (auto-mounted)
    // supplies the canonical layout + the renderer's fullscreen shaders.
    path CookAndMountBrick(AssetManager& assets, const char* archiveName)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = std::filesystem::temp_directory_path() / archiveName;

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult = cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive);
        REQUIRE(cookResult.has_value());

        const VoidResult mountResult = assets.Mount(outArchive);
        REQUIRE(mountResult.has_value());
        return outArchive;
    }

    // Renders a renderer once over the scene/camera and downloads its RGBA16F output.
    vector<u8> RenderOutput(Context& context, SceneRenderer& renderer, const Scene& scene, const Camera& camera)
    {
        context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            renderer.Execute(cmd, Renderer::SceneView{.World = scene, .Camera = camera, .Delta = 0.0f});
        });
        return renderer.GetOutput()->GetImage()->Download();
    }
}

// The design-for-N proof: two SceneRenderers, different extents and different Modes
// (one Final, one Albedo), over ONE Scene with two cameras, Executed INTERLEAVED in
// one command stream (A, B, A) to exercise each renderer's independent barrier
// domain. Asserts the outputs are independent — each sized to its own extent, and
// the same scene texel reads differently between the two modes (Albedo is the raw
// g-buffer color; Final is the lit + tonemapped result), so the two renderers do
// not share state.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: two renderers (different extents + Modes) interleaved over one scene are independent")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_two_renderer.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "N-Test Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    // One directional light straight at the front face, so the lit (Final) result is
    // clearly distinct from the raw albedo.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f),
        .Color = vec3(1.0f, 1.0f, 1.0f),
        .Intensity = 1.0f,
    };

    constexpr uvec2 extentA{96, 96};
    constexpr uvec2 extentB{64, 48};

    Camera cameraA;
    cameraA.SetPerspective(glm::radians(45.0f), static_cast<f32>(extentA.x) / extentA.y, 0.1f, 100.0f);
    cameraA.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    Camera cameraB;
    cameraB.SetPerspective(glm::radians(45.0f), static_cast<f32>(extentB.x) / extentB.y, 0.1f, 100.0f);
    cameraB.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> rendererA = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extentA,
        .Settings = {.Mode = DebugView::Final, .Bloom = false},
    });
    const Unique<SceneRenderer> rendererB = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extentB,
        .Settings = {.Mode = DebugView::Albedo},
    });

    // Each owns its target at its own extent — the crisp independence proof.
    CHECK(rendererA->GetOutput()->GetImage()->GetWidth() == extentA.x);
    CHECK(rendererA->GetOutput()->GetImage()->GetHeight() == extentA.y);
    CHECK(rendererB->GetOutput()->GetImage()->GetWidth() == extentB.x);
    CHECK(rendererB->GetOutput()->GetImage()->GetHeight() == extentB.y);

    // Interleave A, B, A in one command stream so each renderer's barrier domain is
    // exercised against the other's recording, not in isolation.
    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        rendererA->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = cameraA, .Delta = 0.0f});
        rendererB->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = cameraB, .Delta = 0.0f});
        rendererA->Execute(cmd, Renderer::SceneView{.World = *scene, .Camera = cameraA, .Delta = 0.0f});
    });

    const vector<u8> pixelsA = rendererA->GetOutput()->GetImage()->Download();
    const vector<u8> pixelsB = rendererB->GetOutput()->GetImage()->Download();
    REQUIRE(pixelsA.size() == static_cast<size_t>(extentA.x) * extentA.y * 8);
    REQUIRE(pixelsB.size() == static_cast<size_t>(extentB.x) * extentB.y * 8);

    // The cube's lit front face fills both centers. A's Final result is the lit +
    // tonemapped color; B's Albedo result is the raw (linear) brick albedo. They are
    // distinguishable — independent renderers producing mode-consistent content.
    const vec3 centerA = DecodeTexel(pixelsA, extentA.x, extentA.x / 2, extentA.y / 2);
    const vec3 centerB = DecodeTexel(pixelsB, extentB.x, extentB.x / 2, extentB.y / 2);

    // Both render the cube (red-dominant brick), not the dark background.
    CHECK(centerA.r > 0.1f);
    CHECK(centerB.r > 0.1f);
    // The two modes differ measurably at the same world texel.
    const f32 redDelta = std::fabs(centerA.r - centerB.r);
    const f32 greenDelta = std::fabs(centerA.g - centerB.g);
    CHECK((redDelta > 0.02f || greenDelta > 0.02f));

    std::filesystem::remove(outArchive);
}

// Configure/Mode recompile proof. The renderer renders the same scene under each
// DebugView; the proof the pass set re-wired is the observable difference in the
// output pixels — there is no pass-count introspection. Final (lit + tonemap),
// Albedo (raw g-buffer color), Normal (decoded world normal), and Depth (greyscale)
// each produce a distinct center texel for the same cube.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: Configure(Mode) re-wires the pass set — output pixels change per mode")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_mode_recompile.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Mode Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f), .Color = vec3(1.0f), .Intensity = 1.0f,
    };

    constexpr uvec2 extent{128, 128};

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false},
    });

    auto Center = [&]() -> vec3
    {
        const vector<u8> pixels = RenderOutput(Context, *renderer, *scene, camera);
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    const vec3 finalCenter = Center();

    renderer->Configure({.Mode = DebugView::Albedo});
    const vec3 albedoCenter = Center();

    renderer->Configure({.Mode = DebugView::Normal});
    const vec3 normalCenter = Center();

    renderer->Configure({.Mode = DebugView::Depth});
    const vec3 depthCenter = Center();

    // The front face's world normal is +Z, decoded as (0.5, 0.5, 1.0) — blue-biased,
    // unlike the red-biased brick albedo. Depth is a grey shade (r == g == b).
    CHECK(normalCenter.b > normalCenter.r);
    CHECK(depthCenter.r == doctest::Approx(depthCenter.g).epsilon(0.02f));
    CHECK(depthCenter.g == doctest::Approx(depthCenter.b).epsilon(0.02f));

    // Each mode produces a distinct center — the recompile re-wired the pass set.
    auto Differs = [](const vec3 a, const vec3 b) -> bool
    {
        return std::fabs(a.r - b.r) > 0.02f || std::fabs(a.g - b.g) > 0.02f || std::fabs(a.b - b.b) > 0.02f;
    };
    CHECK(Differs(finalCenter, albedoCenter));
    CHECK(Differs(finalCenter, normalCenter));
    CHECK(Differs(albedoCenter, normalCenter));
    CHECK(Differs(normalCenter, depthCenter));

    // The packed-ORM channel arms (Roughness/Metallic/Occlusion) each blit one G2
    // channel as greyscale — a valid recompile to a single-channel debug view.
    renderer->Configure({.Mode = DebugView::Roughness});
    const vec3 roughnessCenter = Center();
    CHECK(roughnessCenter.r == doctest::Approx(roughnessCenter.g).epsilon(0.02f));
    CHECK(roughnessCenter.g == doctest::Approx(roughnessCenter.b).epsilon(0.02f));

    renderer->Configure({.Mode = DebugView::Metallic});
    const vec3 metallicCenter = Center();
    CHECK(metallicCenter.r == doctest::Approx(metallicCenter.g).epsilon(0.02f));

    renderer->Configure({.Mode = DebugView::Occlusion});
    const vec3 occlusionCenter = Center();
    CHECK(occlusionCenter.r == doctest::Approx(occlusionCenter.g).epsilon(0.02f));

    // The AO and Shadows arms force-wire their producing battery pass (regardless of
    // the AO/Shadows settings, both off here) and blit its target as greyscale —
    // exercising the graceful-degradation path. They recompile and render without
    // error to a valid grey debug view.
    renderer->Configure({.Mode = DebugView::AO, .Bloom = false, .Shadows = false, .AO = false});
    const vec3 aoCenter = Center();
    CHECK(aoCenter.r == doctest::Approx(aoCenter.g).epsilon(0.02f));
    CHECK(aoCenter.g == doctest::Approx(aoCenter.b).epsilon(0.02f));

    renderer->Configure({.Mode = DebugView::Shadows, .Bloom = false, .Shadows = false, .AO = false});
    const vec3 shadowCenter = Center();
    CHECK(shadowCenter.r == doctest::Approx(shadowCenter.g).epsilon(0.02f));
    CHECK(shadowCenter.g == doctest::Approx(shadowCenter.b).epsilon(0.02f));

    // Configure back to Final restores the lit result.
    renderer->Configure({.Mode = DebugView::Final, .Bloom = false});
    const vec3 restored = Center();
    CHECK(restored.r == doctest::Approx(finalCenter.r).epsilon(0.02f));

    std::filesystem::remove(outArchive);
}

// Exposure proof: a higher exposure brightens the tonemapped Final result. The same
// scene rendered at exposure 0.25 vs 4.0 produces a measurably darker vs brighter
// center texel — the Exposure setting flows into the tonemap pass.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: Exposure setting changes the tonemapped result")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_exposure.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Exposure Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f), .Color = vec3(1.0f), .Intensity = 1.0f,
    };

    constexpr uvec2 extent{128, 128};

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Exposure = 0.25f},
    });

    auto CenterLuma = [&]() -> f32
    {
        const vector<u8> pixels = RenderOutput(Context, *renderer, *scene, camera);
        const vec3 c = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };

    const f32 dim = CenterLuma();
    renderer->Configure({.Mode = DebugView::Final, .Exposure = 4.0f});
    const f32 bright = CenterLuma();

    CHECK(bright > dim + 0.05f);

    std::filesystem::remove(outArchive);
}

// The BRDF property assertion (this plan's riskiest single golden move, pinned by a
// property beyond the re-blessed image). A metallic, smooth surface shows a specular
// response that a rough, dielectric surface does not: with the light, the view, and
// the surface normal aligned (a face-on cube lit straight on), the half-vector
// equals the normal and the GGX specular peaks for a smooth surface. The same scene
// is rendered twice through one renderer; only the material's authored
// roughness/metallic differ between renders, isolating the specular term.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: a metallic/smooth surface shows a specular response a "
                  "rough/dielectric one does not")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_brdf_property.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "BRDF Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    // A white light straight at the front face: N, L, and V all align, the
    // half-vector equals the normal, and the specular lobe peaks.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f), .Color = vec3(1.0f), .Intensity = 1.0f,
    };

    constexpr uvec2 extent{128, 128};

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false},
    });

    auto CenterLuma = [&]() -> f32
    {
        const vector<u8> pixels = RenderOutput(Context, *renderer, *scene, camera);
        const vec3 c = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };

    Material& mat = const_cast<Material&>(*material->Get());

    // Rough dielectric: roughness 1, metallic 0 — broad, weak specular.
    mat.SetParam("RoughnessFactor", 1.0f);
    mat.SetParam("MetallicFactor", 0.0f);
    const f32 roughLuma = CenterLuma();

    // Smooth metal: roughness ~0.1, metallic 1 — a tight, strong specular peak at
    // the aligned N=L=V geometry, brightening the center beyond the dielectric.
    mat.SetParam("RoughnessFactor", 0.1f);
    mat.SetParam("MetallicFactor", 1.0f);
    const f32 smoothLuma = CenterLuma();

    // The specular response is the discriminating property: the smooth metal reads
    // measurably brighter at the aligned highlight than the rough dielectric.
    CHECK(smoothLuma > roughLuma + 0.05f);

    std::filesystem::remove(outArchive);
}

// Multiple-light accumulation. The lighting pass loops over the scene's lights and
// sums their contributions. A face-on brick cube is rendered three times through
// one renderer: lit by one directional light, then by a second point light alone,
// then by both. The both-lights center is brighter than either single light — the
// loop accumulates rather than overwriting — and the point light's position (read
// from its Transform) reaches the lighting pass, since a point light with no
// Transform-derived position would not illuminate the front face.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: multiple lights accumulate in the lighting loop")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_multi_light.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Multi-Light Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    constexpr uvec2 extent{128, 128};

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final},
    });

    auto CenterLuma = [&]() -> f32
    {
        const vector<u8> pixels = RenderOutput(Context, *renderer, *scene, camera);
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        const vec3 c = DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };

    // A directional light straight at the front face (N·L = 1).
    const Entity dirEntity = scene->CreateEntity();
    scene->Add<Light>(dirEntity) = Light{
        .Type = LightType::Directional,
        .Direction = vec3(0.0f, 0.0f, -1.0f),
        .Color = vec3(1.0f), .Intensity = 1.0f,
    };
    const f32 directionalOnly = CenterLuma();
    CHECK(directionalOnly > 0.0f);

    // Swap to a single point light in front of the cube (placed by its Transform),
    // illuminating the same front face through the distance-attenuated path.
    scene->DestroyEntity(dirEntity);
    const Entity pointEntity = scene->CreateEntity();
    scene->Add<Transform>(pointEntity).Position = vec3(0.0f, 0.0f, 2.0f);
    scene->Add<Light>(pointEntity) = Light{
        .Type = LightType::Point,
        .Color = vec3(1.0f), .Intensity = 8.0f, .Range = 8.0f,
    };
    const f32 pointOnly = CenterLuma();
    // The point light reaches the front face via its Transform-derived position.
    CHECK(pointOnly > 0.0f);

    // Both lights together: the loop sums their contributions, so the center is
    // brighter than either alone.
    const Entity dir2Entity = scene->CreateEntity();
    scene->Add<Light>(dir2Entity) = Light{
        .Type = LightType::Directional,
        .Direction = vec3(0.0f, 0.0f, -1.0f),
        .Color = vec3(1.0f), .Intensity = 1.0f,
    };
    const f32 bothLights = CenterLuma();

    CHECK(bothLights > pointOnly + 0.02f);
    CHECK(bothLights > directionalOnly + 0.02f);

    std::filesystem::remove(outArchive);
}

// The bloom property assertion (this plan's dedicated property pin beyond the
// re-blessed golden). A smooth, metallic brick cube under a strong white light
// produces a tight, saturating specular highlight (HDR well above 1.0) at the
// face-on center. Bloom's bright-pass + separable blur spreads that energy into the
// surrounding texels, so with Bloom ON a ring of texels around the highlight reads
// brighter than with Bloom OFF, where the highlight stays sharp. The two renders
// differ only in the Bloom topology toggle. A third pair then changes the per-frame
// BloomThreshold/BloomIntensity on the SceneView (no Configure) and asserts the
// result moves — proving the params tune without a recompile.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: bloom spreads a bright highlight, and its params tune "
                  "without a recompile")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_bloom.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    // A smooth metal: a tight, strong specular peak at the aligned N=L=V geometry,
    // driving the center HDR luminance well past the bloom threshold.
    Material& mat = const_cast<Material&>(*material->Get());
    mat.SetParam("RoughnessFactor", 0.08f);
    mat.SetParam("MetallicFactor", 1.0f);

    const Ref<Mesh> cube = Mesh::Create(Context, Primitives::Cube(1.4f, *material), "Bloom Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    // A strong white light straight at the front face: N, L, V align and the
    // specular lobe peaks far above 1.0 in HDR — the bright region bloom acts on.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f), .Color = vec3(1.0f), .Intensity = 8.0f,
    };

    constexpr uvec2 extent{128, 128};

    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false},
    });

    // Mean luminance over a window centered on, but excluding, the brightest texel —
    // the "halo" region bloom lifts. The highlight sits at the cube's face-on center.
    auto HaloLuma = [&](const vector<u8>& pixels) -> f64
    {
        const u32 cx = extent.x / 2;
        const u32 cy = extent.y / 2;
        f64 sum = 0.0;
        u32 count = 0;
        for (i32 dy = -24; dy <= 24; ++dy)
            for (i32 dx = -24; dx <= 24; ++dx)
            {
                if (std::abs(dx) < 8 && std::abs(dy) < 8)
                    continue; // skip the saturated core; measure the surrounding halo
                const vec3 c = DecodeTexel(pixels, extent.x,
                    static_cast<u32>(static_cast<i32>(cx) + dx),
                    static_cast<u32>(static_cast<i32>(cy) + dy));
                sum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
                ++count;
            }
        return sum / count;
    };

    auto Render = [&](f32 threshold, f32 intensity) -> vector<u8>
    {
        Context.ImmediateCommands([&](CommandBuffer& cmd)
        {
            renderer->Execute(cmd, Renderer::SceneView{
                .World = *scene, .Camera = camera, .Delta = 0.0f,
                .BloomThreshold = threshold, .BloomIntensity = intensity});
        });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 8);
        return pixels;
    };

    // Bloom OFF: the halo stays at the surface's own (dim) shading.
    const f64 haloNoBloom = HaloLuma(Render(1.0f, 1.0f));

    // Bloom ON (Configure recompiles the topology to insert the chain). The same
    // halo region lifts as the blurred bright residual bleeds outward.
    renderer->Configure({.Mode = DebugView::Final, .Bloom = true});
    const f64 haloBloom = HaloLuma(Render(1.0f, 1.0f));

    CHECK(haloBloom > haloNoBloom + 0.01);

    // Per-frame param tuning without a recompile: a far higher threshold rejects
    // most of the highlight, so the halo falls back toward the no-bloom level — and
    // NO Configure is called between these two renders. The change is purely the
    // ring-buffered material write the bloom stages read this frame.
    const f64 haloLowMix = HaloLuma(Render(1.0f, 0.0f));   // intensity 0: bloom adds nothing
    const f64 haloHighMix = HaloLuma(Render(1.0f, 2.0f));  // intensity 2: bloom adds more

    // Intensity scales the added bloom; 0 → no lift over the surface, 2 → more than 1.
    CHECK(haloHighMix > haloLowMix + 0.01);
    // Intensity 0 collapses the halo back to (about) the no-bloom level.
    CHECK(haloLowMix == doctest::Approx(haloNoBloom).epsilon(0.2));

    std::filesystem::remove(outArchive);
}

// The directional-shadow property assertion (this plan's property pin beyond the
// re-blessed golden). A caster cube floats above a large receiver plane, lit by a
// directional light traveling straight down. With Shadows ON the plane texel
// directly beneath the caster is occluded — distinctly darker than the same texel
// with Shadows OFF, where the light reaches the plane unobstructed. The two renders
// differ only in the Shadows topology toggle, so the recompile inserting/removing
// the shadow pass is the discriminating change. A final Configure({Shadows=false})
// then renders again to prove the stale shadow slot is not sampled after the
// recompile (the validation gate, run under VE_DEBUG, fails on any error this would
// raise).
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: a directional shadow caster darkens a receiver plane, "
                  "and toggling Shadows off recompiles cleanly")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_shadow.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    // A large receiver plane in the XZ plane (world normal +Y) at the origin, and a
    // caster cube floating above its center. Both share the brick material.
    const Ref<Mesh> plane = Mesh::Create(Context, Primitives::Plane(vec2(8.0f), uvec2(1), *material), "Shadow Plane");
    const Ref<Mesh> caster = Mesh::Create(Context, Primitives::Cube(1.2f, *material), "Shadow Caster");

    const Unique<Scene> scene = Scene::Create(Types);

    const Entity planeEntity = scene->CreateEntity();
    scene->Add<Transform>(planeEntity);
    scene->Add<MeshRenderer>(planeEntity).Mesh = assets.Adopt(plane);

    const Entity casterEntity = scene->CreateEntity();
    scene->Add<Transform>(casterEntity).Position = vec3(0.0f, 1.6f, 0.0f);
    scene->Add<MeshRenderer>(casterEntity).Mesh = assets.Adopt(caster);

    // A directional light traveling straight down: the caster occludes the plane
    // texel directly below it.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Type = LightType::Directional,
        .Direction = vec3(0.0f, -1.0f, 0.0f),
        .Color = vec3(1.0f), .Intensity = 3.0f,
    };

    constexpr uvec2 extent{128, 128};

    // Look down at the plane from above-front, so the plane fills the frame and the
    // shadow falls near the image center.
    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 5.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = true},
    });

    auto Luma = [&](const vector<u8>& pixels, u32 x, u32 y) -> f32
    {
        const vec3 c = DecodeTexel(pixels, extent.x, x, y);
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };

    const vector<u8> shadowedPixels = RenderOutput(Context, *renderer, *scene, camera);

    // The cast shadow falls on the plane directly beneath the caster (the light
    // travels straight down). Locate it: scan the center column across the lower
    // half of the frame — below the cube's silhouette, on the plane — for the
    // darkest receiver texel. That is the shadow.
    const u32 column = extent.x / 2;
    u32 shadowRow = extent.y * 5 / 8;
    f32 shadowedLuma = Luma(shadowedPixels, column, shadowRow);
    for (u32 y = extent.y / 2 + 4; y < extent.y - 4; ++y)
    {
        const f32 l = Luma(shadowedPixels, column, y);
        if (l < shadowedLuma)
        {
            shadowedLuma = l;
            shadowRow = y;
        }
    }

    // Shadows off recompiles the topology (removing the shadow pass); the same plane
    // texel now receives the light unobstructed.
    renderer->Configure({.Mode = DebugView::Final, .Bloom = false, .Shadows = false});
    const f32 unshadowedLuma = Luma(RenderOutput(Context, *renderer, *scene, camera), column, shadowRow);

    // The shadow darkens the receiver: the same texel is measurably dimmer with the
    // caster's shadow than without it.
    CHECK(unshadowedLuma > shadowedLuma + 0.03f);

    // Render once more with Shadows off to confirm no stale shadow-slot sample after
    // the recompile (the validation gate catches any error this would raise).
    const vector<u8> afterToggle = RenderOutput(Context, *renderer, *scene, camera);
    REQUIRE(afterToggle.size() == static_cast<size_t>(extent.x) * extent.y * 8);

    std::filesystem::remove(outArchive);
}

// The SSAO property assertion (this plan's dedicated property pin beyond the
// re-blessed golden). A sphere resting on a receiver plane forms a concave contact
// crease where the plane meets the sphere; screen-space ambient occlusion darkens
// that crease by attenuating the hemispheric ambient term there. With AO ON the
// contact region's ambient is lower than with AO OFF — the two renders differ only
// in the AO topology toggle (Configure recompiles the pass set + the lighting
// variant). A grazing light keeps direct lighting low in the crease so the ambient
// (and thus the AO) delta dominates the measurement.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: SSAO darkens a sphere-on-plane contact region, and "
                  "Configure(AO=false) recompiles")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const path outArchive = CookAndMountBrick(assets, "veng_gpu_ssao.vengpack");

    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{0x232B});
    REQUIRE(material.has_value());

    // A flat dielectric so the ambient term (which SSAO modulates) is a meaningful
    // fraction of the shading at the contact crease.
    Material& mat = const_cast<Material&>(*material->Get());
    mat.SetParam("RoughnessFactor", 1.0f);
    mat.SetParam("MetallicFactor", 0.0f);

    // A sphere resting on a plane: the plane's +Y face is the receiver, the sphere
    // sits just above the origin so its lower hemisphere creases into the plane.
    const Ref<Mesh> plane = Mesh::Create(Context, Primitives::Plane(vec2(6.0f), uvec2(1), *material), "SSAO Plane");
    const Ref<Mesh> sphere = Mesh::Create(Context, Primitives::Sphere(0.7f, 24, 32, *material), "SSAO Sphere");

    const Unique<Scene> scene = Scene::Create(Types);

    const Entity planeEntity = scene->CreateEntity();
    scene->Add<Transform>(planeEntity);
    scene->Add<MeshRenderer>(planeEntity).Mesh = assets.Adopt(plane);

    const Entity sphereEntity = scene->CreateEntity();
    scene->Add<Transform>(sphereEntity).Position = vec3(0.0f, 0.7f, 0.0f);
    scene->Add<MeshRenderer>(sphereEntity).Mesh = assets.Adopt(sphere);

    // A light high overhead pointing down so the plane and sphere are lit but the
    // contact crease (occluded from the ambient hemisphere) is where AO acts.
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.2f, -1.0f, 0.2f), .Color = vec3(1.0f), .Intensity = 1.0f,
    };

    constexpr uvec2 extent{160, 160};

    // Look down at the contact from a shallow angle so the crease around the
    // sphere's base fills a band of the frame.
    Camera camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 1.6f, 3.2f), vec3(0.0f, 0.4f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context, .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(), .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .AO = true},
    });

    // The mean luminance over the lower-center band, where the sphere meets the
    // plane — the region the contact occlusion darkens.
    auto ContactLuma = [&](const vector<u8>& pixels) -> f64
    {
        const u32 x0 = extent.x / 2 - 40;
        const u32 x1 = extent.x / 2 + 40;
        const u32 y0 = extent.y / 2 + 8;
        const u32 y1 = extent.y / 2 + 48;
        f64 sum = 0.0;
        u32 count = 0;
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = x0; x < x1; ++x)
            {
                const vec3 c = DecodeTexel(pixels, extent.x, x, y);
                sum += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
                ++count;
            }
        return sum / count;
    };

    // AO ON: the contact crease is darkened by the screen-space occlusion.
    const f64 contactAoOn = ContactLuma(RenderOutput(Context, *renderer, *scene, camera));

    // AO OFF (Configure recompiles the topology — the SSAO pass is removed and the
    // baked-occlusion-only lighting variant is selected). The same contact region is
    // brighter without the screen-space occlusion attenuating its ambient.
    renderer->Configure({.Mode = DebugView::Final, .Bloom = false, .AO = false});
    const f64 contactAoOff = ContactLuma(RenderOutput(Context, *renderer, *scene, camera));

    CHECK(contactAoOff > contactAoOn + 0.002);

    // Configure back to AO on recompiles again and restores the darker contact.
    renderer->Configure({.Mode = DebugView::Final, .Bloom = false, .AO = true});
    const f64 contactRestored = ContactLuma(RenderOutput(Context, *renderer, *scene, camera));
    CHECK(contactRestored == doctest::Approx(contactAoOn).epsilon(0.05));

    std::filesystem::remove(outArchive);
}

#ifdef GPU_POSTPROCESS_FIXTURE_DIR

// The PostProcess fullscreen-material proof. An identity PostProcess material —
// its fragment samples one runtime-bound input and writes it unchanged — is cooked
// and driven by a PostProcessScenePass against an RGBA8 output. A source target is
// cleared to a known color in the same command stream, registered into bindless,
// and the pass writes the live source bindless index into the material's
// runtime-bound input handle field each frame, samples it, and writes it through.
// The downloaded output must match the source color: build-from-material (layout at
// load, pipeline built by the pass against its format), set-0 bind, upstream sample
// (the derived barrier transitions the cleared source for the read), the
// domain-keyed selector push at offset 0, and the fullscreen draw all work.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene renderer: a PostProcess material samples an upstream target and writes it unchanged")
{
    const path fixtureDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_postprocess.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(fixtureDir / "postprocess_pack.json", outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    // The identity PostProcess material loads: its layout is built at load, but its
    // GraphicsPipeline is NOT (the pass builds it against its color format), and its
    // domain is PostProcess.
    const AssetResult<AssetHandle<Material>> material = assets.LoadSync<Material>(AssetId{9503});
    REQUIRE(material.has_value());
    REQUIRE(material->IsLoaded());

    const Material& mat = *material->Get();
    CHECK(mat.GetDomain() == MaterialDomain::PostProcess);
    CHECK(mat.GetIndex() != MaterialHandle::Invalid);
    CHECK(mat.GetPipeline() == nullptr);            // built by the pass, not the loader
    CHECK(mat.GetPipelineLayout() != nullptr);      // built by the loader for both domains

    constexpr uvec2 extent{64, 48};
    constexpr ClearColor sourceColor{0.25f, 0.55f, 0.80f, 1.0f};

    // The upstream source the pass samples — cleared to a known color, sampled
    // (so the graph derives its attachment → shader-read barrier).
    const Ref<Image> sourceImage = Image::Create(Context, {
        .Name = "PostProcess Source",
        .Extent = {extent.x, extent.y, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled,
    });
    const Ref<ImageView> sourceView = ImageView::Create(Context, {.Name = "PostProcess Source View", .Image = sourceImage});

    const Ref<Image> outputImage = Image::Create(Context, {
        .Name = "PostProcess Output",
        .Extent = {extent.x, extent.y, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
    });
    const Ref<ImageView> outputView = ImageView::Create(Context, {.Name = "PostProcess Output View", .Image = outputImage});

    const Ref<Sampler> sampler = Sampler::Create(Context, {
        .Name = "PostProcess Sampler",
        .AddressModeU = AddressMode::ClampToEdge,
        .AddressModeV = AddressMode::ClampToEdge,
        .AddressModeW = AddressMode::ClampToEdge,
    });

    BindlessRegistry& bindless = Context.GetBindlessRegistry();
    const TextureHandle sourceHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);

    // Drive one graph: clear the source to the known color, then the postprocess
    // pass samples it and writes the output. The pass declares .Sample(source), so
    // the clear-write → sample-read barrier is derived by the graph.
    Context.ImmediateCommands([&](CommandBuffer& cmd)
    {
        RenderGraph graph(Context);
        const ResourceId sourceId = graph.Import("PostProcess Source");
        const ResourceId outputId = graph.Import("PostProcess Output");

        graph.AddPass("Clear Source")
            .Color({
                .Resource = sourceId,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = sourceColor,
            })
            .Execute([](PassContext&) {});

        PostProcessScenePass pass(Context, *material,
            PostProcessInput{
                .Source = sourceId,
                .SourceTexture = sourceHandle,
                .Sampler = samplerHandle,
                .TextureField = "Source",
                .SamplerField = "SourceSampler",
            },
            outputId, Format::RGBA8Unorm, extent);
        pass.Declare(graph, PassIO{});

        const RenderGraph::ImportBinding bindings[] = {
            {sourceId, sourceView},
            {outputId, outputView},
        };
        graph.Compile()->Execute(cmd, bindings);
    });

    const vector<u8> pixels = outputImage->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(extent.x) * extent.y * 4);

    // The center texel reads the source color unchanged (within 8-bit quantization).
    const usize center = (static_cast<usize>(extent.y / 2) * extent.x + extent.x / 2) * 4;
    auto Approx8 = [](u8 actual, f32 expected) {
        return std::fabs(static_cast<f32>(actual) / 255.0f - expected) < 0.02f;
    };
    CHECK(Approx8(pixels[center + 0], sourceColor.R));
    CHECK(Approx8(pixels[center + 1], sourceColor.G));
    CHECK(Approx8(pixels[center + 2], sourceColor.B));
    CHECK(pixels[center + 3] == 255);

    bindless.Release(sourceHandle);
    bindless.Release(samplerHandle);
    std::filesystem::remove(outArchive);
}

#endif // GPU_POSTPROCESS_FIXTURE_DIR

#endif // GPU_GBUFFER_FIXTURE_DIR

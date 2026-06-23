// Splitscreen case. The headline payoff of the placement inversion: N Presented
// viewports assembled into sub-rectangles of one window. It builds four quadrant
// viewports over one cube scene, drives the engine render phase (Viewport::Render
// per viewport, in registration order), gathers their outputs into the gather
// pass's owned offscreen assembly target through their quadrant regions, and
// asserts both halves: that the gather pass received each viewport's GetRegion()
// (placement-level, via GetPlacements()) and that the assembled image carries each
// quadrant's distinguishing color with the gather clear in the uncovered gutter
// (pixel-level).
//
// The composite tail (SwapChainCompositePass) is a windowed-only HDR-encode of the
// gather's already-assembled target — it needs an ImGuiLayer and a live swapchain
// extent, neither of which exists headless. So the test stands in a headless
// composite: it samples the gather assembly into an offscreen target it owns (the
// composite's job is to read the gather's single source and write a presented
// surface; here that surface is a downloadable image), then reads back the result —
// the device-level proof of the region-driven placement.

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Viewport.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Builds a cube scene with a materialless MeshRenderer and returns the owning
    // mesh so the caller keeps it resident for the viewports' lifetime.
    Ref<Mesh> PopulateCubeScene(Context& context, AssetManager& assets, Scene& scene)
    {
        const Ref<Mesh> cube = Mesh::BuildSync(context, Primitives::Cube(1.0f), "Splitscreen Cube");

        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

        return cube;
    }

    CameraView FrontCamera(uvec2 extent)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f),
                              static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f,
                              100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // Clears `view` to `clear` through a one-pass render graph — the deterministic
    // per-viewport distinguishing signal the pixel assertion reads back.
    void ClearOutput(Context& context, CommandBuffer& cmd, const Ref<ImageView>& view,
                     const ClearColor& clear)
    {
        RenderGraph graph(context);
        const ResourceId target = graph.Import("Splitscreen Output");
        graph.AddPass("clear viewport output")
            .Color({
                .Resource = target,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = clear,
            })
            .Execute([](PassContext&) {});
        const RenderGraph::ImportBinding binding{.Id = target, .View = view};
        graph.Compile()->Execute(cmd, {&binding, 1});
    }

    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    // The stand-in headless composite: samples `source` through set-0 bindless into a
    // fresh RGBA16Sfloat offscreen target (TransferSrc, so it can be downloaded) and
    // returns the downloaded pixels. The gather's own assembly target is sampled-only,
    // so the read-back has to land in a target the test owns — exactly what the real
    // composite does into the swapchain.
    vector<u8> CompositeToOffscreen(Context& context, AssetManager& assets,
                                    const Ref<ImageView>& source, uvec2 extent)
    {
        const AssetResult<AssetHandle<Shader>> vs = assets.LoadSync<Shader>(AssetId{0x1F42});
        const AssetResult<AssetHandle<Shader>> fs = assets.LoadSync<Shader>(AssetId{0x1F44});
        REQUIRE(vs.has_value());
        REQUIRE(fs.has_value());

        const Ref<PipelineLayout> layout = PipelineLayout::Create(
            context, {
                         .Name = "Splitscreen Composite Layout",
                         .PushConstantRanges =
                             {
                                 PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment),
                             },
                     });

        const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            context, {
                         .Name = "Splitscreen Composite Pipeline",
                         .ColorAttachments = {{.Format = Format::RGBA16Sfloat}},
                         .PipelineLayout = layout,
                         .ShaderStages =
                             {
                                 {.Stage = ShaderStage::Vertex, .Module = vs->Get()->Module},
                                 {.Stage = ShaderStage::Fragment, .Module = fs->Get()->Module},
                             },
                     });

        const Ref<Sampler> sampler =
            Sampler::Create(context, {
                                         .Name = "Splitscreen Composite Sampler",
                                         .MagFilter = Filter::Nearest,
                                         .MinFilter = Filter::Nearest,
                                         .AddressModeU = AddressMode::ClampToEdge,
                                         .AddressModeV = AddressMode::ClampToEdge,
                                         .AddressModeW = AddressMode::ClampToEdge,
                                     });

        const Ref<Image> target = Image::Create(
            context, {
                         .Name = "Splitscreen Composite Output",
                         .Extent = {extent.x, extent.y, 1},
                         .Format = Format::RGBA16Sfloat,
                         .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                     });
        const Ref<ImageView> targetView = ImageView::Create(
            context, {.Name = "Splitscreen Composite Output View", .Image = target});

        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const TextureHandle textureHandle = bindless.Register(source);
        const SamplerHandle samplerHandle = bindless.Register(sampler);

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId sourceId = graph.Import("Splitscreen Assembly");
                const ResourceId outputId = graph.Import("Splitscreen Composite Target");

                graph.AddPass("composite splitscreen")
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
                    {.Id = sourceId, .View = source},
                    {.Id = outputId, .View = targetView},
                };
                graph.Compile()->Execute(cmd, bindings);
            });

        vector<u8> pixels = target->Download();
        bindless.Release(textureHandle);
        bindless.Release(samplerHandle);
        return pixels;
    }

    // One RGBA16F texel decoded to a linear vec3 (the assembly + composite targets are
    // RGBA16Sfloat, matching the SceneRenderer output format).
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // The center texel of `region`, decoded from the assembly buffer.
    vec3 RegionCenter(const vector<u8>& assembly, u32 width, const ViewportRegion& region)
    {
        const u32 x = static_cast<u32>(region.Offset.x) + region.Extent.x / 2;
        const u32 y = static_cast<u32>(region.Offset.y) + region.Extent.y / 2;
        return DecodeTexel(assembly, width, x, y);
    }
}

// Four Presented viewports tiling one window into quadrants. Each renders the cube
// scene (the engine render phase) and is then cleared to its own color so the
// assembled quadrants are identifiable; the gather pass assembles all four into its
// owned offscreen target through their regions.
TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "splitscreen: N Presented viewports assemble into quadrant regions — the gather "
                  "receives each region and the assembled image carries each quadrant's content")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    // The window the quadrants tile. The quadrants cover the top covered-height band;
    // the window is taller by a gutter the gather must clear (the clear-elsewhere path).
    constexpr u32 gutterHeight = 16;
    constexpr uvec2 quadrantExtent{64, 40};
    constexpr uvec2 windowExtent{quadrantExtent.x * 2, quadrantExtent.y * 2 + gutterHeight};

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    // Four quadrant regions, in a known registration order: TL, TR, BL, BR. Each
    // carries a distinct solid color (well inside [0,1], unambiguous in RGBA16F).
    struct Quadrant
    {
        ViewportRegion Region;
        vec3 Color;
    };
    const std::array<Quadrant, 4> quadrants = {{
        {.Region = {.Offset = {0, 0}, .Extent = quadrantExtent}, .Color = vec3(0.8f, 0.1f, 0.1f)},
        {.Region = {.Offset = {static_cast<i32>(quadrantExtent.x), 0}, .Extent = quadrantExtent},
         .Color = vec3(0.1f, 0.8f, 0.1f)},
        {.Region = {.Offset = {0, static_cast<i32>(quadrantExtent.y)}, .Extent = quadrantExtent},
         .Color = vec3(0.1f, 0.1f, 0.8f)},
        {.Region = {.Offset = {static_cast<i32>(quadrantExtent.x),
                               static_cast<i32>(quadrantExtent.y)},
                    .Extent = quadrantExtent},
         .Color = vec3(0.8f, 0.8f, 0.1f)},
    }};

    // Build and register four Presented viewports in quadrant order. A bare
    // vector<Viewport*> stands in for Application's drive-list: registration order
    // is render order, and each viewport self-unregisters on destruction.
    vector<Viewport*> driveList;
    std::array<Unique<Viewport>, 4> viewports;
    for (usize i = 0; i < quadrants.size(); ++i)
    {
        viewports[i] = Viewport::Create({
            .Context = Context,
            .Assets = assets,
            .Region = quadrants[i].Region,
            .Settings = {},
            .Role = ViewportRole::Presented,
        });
        driveList.push_back(viewports[i].get());
        viewports[i]->AttachToDriveList(driveList);
        viewports[i]->SetViewState({
            .World = scene.get(),
            .Camera = FrontCamera(quadrantExtent),
            .Delta = 0.0f,
        });
    }

    CHECK(driveList.size() == 4);
    for (usize i = 0; i < viewports.size(); ++i)
    {
        CHECK(viewports[i]->GetRole() == ViewportRole::Presented);
        CHECK(viewports[i]->GetRegion().Offset == quadrants[i].Region.Offset);
        CHECK(viewports[i]->GetRegion().Extent == quadrants[i].Region.Extent);
    }

    const Unique<GatherPass> gather = GatherPass::Create({
        .Context = Context,
        .Assets = assets,
        .Extent = windowExtent,
    });

    // Drive the engine render phase (every viewport, in registration order), then
    // clear each output to its quadrant color so the assembled regions are
    // identifiable. The clear is recorded after Render in the same stream: it leaves
    // each output sampleable for the gather, with its distinguishing color.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            for (Viewport* viewport : driveList)
            {
                viewport->Render(cmd);
            }
            for (usize i = 0; i < viewports.size(); ++i)
            {
                ClearOutput(Context, cmd, viewports[i]->GetOutput(),
                            ClearColor{.R = quadrants[i].Color.r,
                                       .G = quadrants[i].Color.g,
                                       .B = quadrants[i].Color.b,
                                       .A = 1.0f});
            }
        });

    // Build the placement list from each viewport's live output + region, in
    // registration order, and hand it to the gather pass.
    vector<CompositePlacement> placements;
    placements.reserve(viewports.size());
    for (const Viewport* viewport : driveList)
    {
        placements.push_back({
            .Texture = viewport->GetOutput(),
            .Region = viewport->GetRegion(),
        });
    }
    gather->SetPlacements(placements);

    // Placement-level assertion: the gather pass received exactly the four viewports'
    // regions, in registration order, with their live output textures.
    const std::span<const CompositePlacement> received = gather->GetPlacements();
    REQUIRE(received.size() == viewports.size());
    for (usize i = 0; i < viewports.size(); ++i)
    {
        CHECK(received[i].Region.Offset == viewports[i]->GetRegion().Offset);
        CHECK(received[i].Region.Extent == viewports[i]->GetRegion().Extent);
        CHECK(received[i].Texture.get() == viewports[i]->GetOutput().get());
    }

    // Assemble: scissor-blit each placement into its region on the gather pass's
    // owned offscreen target, clearing the uncovered gutter.
    RenderGraph graph(Context);
    const Unique<CompiledGraph> compiled = gather->Compile(graph);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { gather->Execute(cmd, *compiled); });

    // Composite the assembly into a downloadable offscreen target (the headless
    // stand-in for the swapchain composite), then read it back.
    const vector<u8> assembly =
        CompositeToOffscreen(Context, assets, gather->GetOutput(), windowExtent);
    REQUIRE(assembly.size() ==
            static_cast<size_t>(windowExtent.x) * windowExtent.y * 8); // RGBA16F = 8 bytes/texel

    // Pixel-level assertion: check each quadrant center carries its viewport's color,
    // and the uncovered gutter below the quadrants carries the gather's clear (black).

    for (usize i = 0; i < quadrants.size(); ++i)
    {
        const vec3 center = RegionCenter(assembly, windowExtent.x, quadrants[i].Region);
        CHECK(center.r == doctest::Approx(quadrants[i].Color.r).epsilon(0.02f));
        CHECK(center.g == doctest::Approx(quadrants[i].Color.g).epsilon(0.02f));
        CHECK(center.b == doctest::Approx(quadrants[i].Color.b).epsilon(0.02f));
    }

    // A texel in the uncovered gutter (below the covered band) reads the gather clear.
    const vec3 gutter = DecodeTexel(assembly, windowExtent.x, windowExtent.x / 2,
                                    windowExtent.y - gutterHeight / 2);
    CHECK(gutter.r == doctest::Approx(0.0f).epsilon(0.02f));
    CHECK(gutter.g == doctest::Approx(0.0f).epsilon(0.02f));
    CHECK(gutter.b == doctest::Approx(0.0f).epsilon(0.02f));
}

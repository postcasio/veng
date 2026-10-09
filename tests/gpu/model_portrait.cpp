// ModelPortrait end to end, driven the way an application drives it: a compositor whose one viewport
// presents a host scene carrying the component. The model is a runtime prefab of one recipe cube, so
// its mesh streams in through the ordinary async build the portrait waits on. The cases assert:
//
//  - a shaded portrait covers its model and nothing else (coverage in alpha), and a depth-normal
//    portrait's centre normal faces its camera;
//  - the caller-driven half: an explicit camera is taken as given and moves the coverage, a per-frame
//    pose turns the model with no second instantiation, a repopulate swaps the attached parts and
//    only them, and the model-only bounds exclude what a populate attached;
//  - the lifecycle: a released renderer is reused by the next portrait of its configuration with no
//    build, create/destroy cycles hold the bindless occupancy steady, and a portrait in a scene no
//    viewport renders is never rendered.

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/ModelPortrait.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <cmath>

#include <gpu/fixture.h>
#include "Renderer/PortraitRenderer.h"
#include "support/TempPath.h"
#include "support/TestCook.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 PortraitExtent{64, 64};

    // An opaque, depth-writing surface material in the capture fixture pack, for the recipe cube.
    constexpr AssetId CubeMaterialInstance{0x24D1};

    // The test shader pack's fullscreen vertex stage and its sample-a-handle fragment.
    constexpr AssetId SampleVertexId{0x1F42};
    constexpr AssetId SampleFragmentId{0x1F44};

    struct SamplePushConstants
    {
        u32 TextureIndex;
        u32 SamplerIndex;
    };

    path CookMaterialPack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_model_portrait.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "capture_surface_pack.json",
                                              outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                                              {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        return outArchive;
    }

    vec4 DecodeTexel(const vector<u8>& rgba16f, const u32 width, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec4(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]), glm::unpackHalf1x16(halves[base + 3]));
    }

    // The coverage-weighted horizontal centroid of a shaded portrait, in [0, 1]; -1 with no coverage.
    f32 CoverageCentroidX(const vector<u8>& pixels)
    {
        f32 weighted = 0.0f;
        f32 total = 0.0f;
        for (u32 y = 0; y < PortraitExtent.y; ++y)
        {
            for (u32 x = 0; x < PortraitExtent.x; ++x)
            {
                const f32 alpha = DecodeTexel(pixels, PortraitExtent.x, x, y).a;
                weighted += alpha * (static_cast<f32>(x) + 0.5f);
                total += alpha;
            }
        }
        return total > 0.0f ? weighted / (total * static_cast<f32>(PortraitExtent.x)) : -1.0f;
    }

    // Samples a bindless handle into an RGBA16F target and downloads it, for a target that carries
    // no transfer usage of its own (the lean path's world normal).
    vector<u8> SampleHandle(Context& context, AssetManager& assets, const Ref<ImageView>& source,
                            const TextureHandle handle, const uvec2 extent)
    {
        const AssetResult<AssetHandle<Shader>> vertex = assets.LoadSync<Shader>(SampleVertexId);
        const AssetResult<AssetHandle<Shader>> fragment = assets.LoadSync<Shader>(SampleFragmentId);
        REQUIRE(vertex.has_value());
        REQUIRE(fragment.has_value());
        const Ref<PipelineLayout> layout = PipelineLayout::Create(
            context, {.Name = "Portrait Sample Layout",
                      .PushConstantRanges = {
                          PushConstantRange::Of<SamplePushConstants>(ShaderStage::Fragment)}});
        const Ref<GraphicsPipeline> pipeline = GraphicsPipeline::Create(
            context, {.Name = "Portrait Sample Pipeline",
                      .ColorAttachments = {{.Format = Format::RGBA16Sfloat}},
                      .PipelineLayout = layout,
                      .ShaderStages = {
                          {.Stage = ShaderStage::Vertex, .Module = vertex->Get()->Module},
                          {.Stage = ShaderStage::Fragment, .Module = fragment->Get()->Module},
                      }});
        const Ref<Image> target = Image::Create(
            context, {.Name = "Portrait Sample Output",
                      .Extent = {extent.x, extent.y, 1},
                      .Format = Format::RGBA16Sfloat,
                      .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc});
        const Ref<ImageView> targetView =
            ImageView::Create(context, {.Name = "Portrait Sample Output View", .Image = target});
        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const SamplerHandle sampler =
            bindless
                .AcquireSampler({.Name = "Portrait Sample Sampler",
                                 .MagFilter = Filter::Nearest,
                                 .MinFilter = Filter::Nearest,
                                 .AddressModeU = AddressMode::ClampToEdge,
                                 .AddressModeV = AddressMode::ClampToEdge,
                                 .AddressModeW = AddressMode::ClampToEdge})
                .Handle;

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(context);
                const ResourceId sourceId = graph.Import("Portrait Source");
                const ResourceId outputId = graph.Import("Portrait Sample Output");
                graph.AddPass("Sample Portrait Target")
                    .Color({.Resource = outputId,
                            .Load = LoadOp::Clear,
                            .Store = StoreOp::Store,
                            .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f}})
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
                                .TextureIndex = handle.Index, .SamplerIndex = sampler.Index});
                            passCmd.DrawFullscreenTriangle();
                        });
                const RenderGraph::ImportBinding bindings[] = {
                    {.Id = sourceId, .View = source},
                    {.Id = outputId, .View = targetView},
                };
                graph.Compile()->Execute(cmd, bindings);
            });
        return target->Download();
    }

    template <class T>
    Prefab::Component Comp(const TypeRegistry& types, const T& value)
    {
        Prefab::Component component;
        component.Type = types.IdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    // A compositor presenting a host scene through one viewport, and a unit-cube model prefab.
    struct PortraitFixture : Veng::Test::GpuFixture
    {
        Unique<AssetManager> Assets;
        AssetHandle<MaterialInstance> Material;
        AssetHandle<Prefab> Cube;
        Unique<Scene> Host;
        Unique<ViewportCompositor> Compositor;
        Unique<Viewport> View;

        PortraitFixture()
        {
            RegisterBuiltinTypes(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            REQUIRE(Assets->Mount(CookMaterialPack()).has_value());
            REQUIRE(Assets->Mount(path(TEST_SHADER_PACK)).has_value());
            Material = *Assets->LoadSync<MaterialInstance>(CubeMaterialInstance);

            MeshRenderer renderer;
            MeshSource source;
            *static_cast<CubeShape*>(source.SetActive(TypeIdOf<CubeShape>())) =
                CubeShape{.Extent = 1.0f, .Material = Material};
            renderer.Source = source;
            vector<Prefab::PrefabEntity> entities;
            entities.push_back({.Components = {Comp(Types, Transform{}), Comp(Types, renderer)}});
            Cube = Assets->Adopt<Prefab>(Prefab::Create(std::move(entities), {}));

            Host = Scene::Create(Types);
            Compositor = CreateUnique<ViewportCompositor>(Context);
            View = Viewport::Create({
                .Context = Context,
                .Assets = *Assets,
                .Region = {.Offset = {0, 0}, .Extent = {32, 32}},
                .ColorFormat = Format::RGBA16Sfloat,
                .Role = ViewportRole::Offscreen,
            });
            Compositor->RegisterViewport(*View);
        }

        ~PortraitFixture()
        {
            // The scenes and their portraits go before the compositor whose pool they return to.
            View.reset();
            Host.reset();
        }

        PortraitFixture(const PortraitFixture&) = delete;
        PortraitFixture& operator=(const PortraitFixture&) = delete;

        // Settles the async mesh builds a spawn started, then drives and renders one frame.
        ModelPortraitDriveResult Frame()
        {
            Tasks.WaitForAll();
            Tasks.PumpMainThread();
            Assets->PumpFinalizes();
            CameraView camera;
            camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
            View->SetViewState({.World = Host.get(), .Camera = camera, .Delta = 0.016f});
            Context.ImmediateCommands([&](CommandBuffer& cmd)
                                      { Compositor->RenderRegistered(cmd); });
            return Compositor->GetModelPortraitDrive();
        }

        // Frames until every listed portrait is ready, failing after a handful.
        void FrameUntilReady(std::initializer_list<const ModelPortrait*> portraits)
        {
            for (int frame = 0; frame < 8; ++frame)
            {
                Frame();
                if (std::ranges::all_of(portraits, [](const ModelPortrait* portrait)
                                        { return portrait->GetOutput().Ready; }))
                {
                    return;
                }
            }
            FAIL("a portrait never became ready");
        }

        Entity AddPortrait(Scene& scene, const PortraitOutput output = PortraitOutput::Shaded)
        {
            const Entity entity = scene.CreateEntity();
            auto& portrait = scene.Add<ModelPortrait>(entity);
            portrait.Model = Cube;
            portrait.Extent = PortraitExtent;
            portrait.Output = output;
            return entity;
        }

        vector<u8> ColorPixels(const ModelPortrait& portrait) const
        {
            return portrait.GetRenderer()->GetColorView()->GetImage()->Download();
        }
    };
}

TEST_CASE_FIXTURE(PortraitFixture,
                  "model portrait: a shaded portrait covers its model alone, and a depth-normal "
                  "portrait's normal faces its camera")
{
    const Entity shadedEntity = AddPortrait(*Host);
    const Entity shapeEntity = AddPortrait(*Host, PortraitOutput::GeometryDepthNormal);
    auto& shape = Host->Get<ModelPortrait>(shapeEntity);
    shape.Framing.Yaw = 0.0f;
    shape.Framing.Pitch = 0.0f;
    const ModelPortrait& shaded = Host->Get<ModelPortrait>(shadedEntity);
    FrameUntilReady({&shaded, &shape});

    const ModelPortraitOutput shadedOut = shaded.GetOutput();
    CHECK(shadedOut.Color.IsValid());
    CHECK_FALSE(shadedOut.Normal.IsValid());
    CHECK(shadedOut.Extent == PortraitExtent);

    // Coverage, not colour: the model's centre is covered and every corner is clear.
    const vector<u8> pixels = ColorPixels(shaded);
    const vec4 centre =
        DecodeTexel(pixels, PortraitExtent.x, PortraitExtent.x / 2, PortraitExtent.y / 2);
    f32 cornerCoverage = 0.0f;
    for (const uvec2 corner :
         {uvec2(1, 1), uvec2(PortraitExtent.x - 2, 1), uvec2(1, PortraitExtent.y - 2),
          uvec2(PortraitExtent.x - 2, PortraitExtent.y - 2)})
    {
        cornerCoverage =
            std::max(cornerCoverage, DecodeTexel(pixels, PortraitExtent.x, corner.x, corner.y).a);
    }
    CHECK(centre.a == doctest::Approx(1.0f));
    CHECK(cornerCoverage == doctest::Approx(0.0f));

    // The depth-normal portrait looks straight down -Z at the cube's +Z face.
    const ModelPortraitOutput shapeOut = shape.GetOutput();
    CHECK_FALSE(shapeOut.Color.IsValid());
    REQUIRE(shapeOut.Normal.IsValid());
    REQUIRE(shapeOut.Depth.IsValid());
    const SceneRenderer& lean = shape.GetRenderer()->GetSceneRenderer();
    const vector<u8> normals =
        SampleHandle(Context, *Assets, lean.GetNormalView(), shapeOut.Normal, PortraitExtent);
    const vec3 normal =
        vec3(DecodeTexel(normals, PortraitExtent.x, PortraitExtent.x / 2, PortraitExtent.y / 2));
    const vec3 toCamera =
        glm::normalize(shapeOut.Camera.GetPosition() - shapeOut.ModelBounds.Center());
    CHECK(glm::dot(glm::normalize(normal), toCamera) > 0.9f);
}

TEST_CASE_FIXTURE(PortraitFixture,
                  "model portrait: an explicit camera, a per-frame pose, repopulation and the "
                  "model-only bounds")
{
    const Entity entity = AddPortrait(*Host);
    auto& portrait = Host->Get<ModelPortrait>(entity);
    portrait.Framing.Mode = PortraitFramingMode::Explicit;
    portrait.Framing.CameraPosition = vec3(0.0f, 0.0f, 4.0f);
    portrait.Framing.FieldOfView = 30.0f;
    FrameUntilReady({&portrait});

    // The camera is the given one.
    const ModelPortraitOutput placed = portrait.GetOutput();
    CHECK(glm::length(placed.Camera.GetPosition() - vec3(0.0f, 0.0f, 4.0f)) < 1e-4f);
    CHECK(std::abs(placed.Camera.Projection()[1][1]) ==
          doctest::Approx(1.0f / std::tan(glm::radians(15.0f))).epsilon(1e-4));
    const f32 before = CoverageCentroidX(ColorPixels(portrait));
    CHECK(before == doctest::Approx(0.5f).epsilon(0.05));

    // Moving the camera right moves the model's coverage left.
    portrait.Framing.CameraPosition.x = 1.2f;
    Frame();
    const f32 after = CoverageCentroidX(ColorPixels(portrait));
    CHECK(after >= 0.0f);
    CHECK(after < before - 0.1f);

    // A pose turns the instance in place.
    portrait.ModelPose.Rotation = glm::angleAxis(glm::radians(45.0f), vec3(0.0f, 1.0f, 0.0f));
    Frame();
    CHECK(portrait.GetInstantiationCount() == 1);
    CHECK(portrait.GetOutput().ModelTransform[0].x ==
          doctest::Approx(std::cos(glm::radians(45.0f))));

    // A populate attaches a part beyond the model (under the turned root, so it turns too); the
    // model-only bounds leave it out.
    const Ref<Mesh> partMesh = Mesh::BuildSync(Context, Primitives::Cube(1.0f, Material), "Part");
    f32 partOffset = 3.0f;
    portrait.SetOnPopulate(
        [&](Scene& scene, const Entity root)
        {
            const Entity part = scene.CreateEntity();
            scene.Add<Transform>(part).Position = vec3(partOffset, 0.0f, 0.0f);
            scene.Add<MeshRenderer>(part).Mesh = Assets->Adopt(partMesh);
            scene.SetParent(part, root);
        });
    Frame();
    const ModelPortraitOutput populated = portrait.GetOutput();
    CHECK(populated.Bounds.Max.x > 2.0f);
    CHECK(populated.ModelBounds.Max.x < 1.0f);

    // Repopulating swaps the part and keeps the instance.
    const Entity root = portrait.GetModelRoot();
    partOffset = -3.0f;
    portrait.Repopulate();
    Frame();
    u32 forward = 0;
    u32 backward = 0;
    for (auto [part, transform, renderer] : portrait.GetScene()->View<Transform, MeshRenderer>())
    {
        forward += transform.Position.x > 2.0f ? 1u : 0u;
        backward += transform.Position.x < -2.0f ? 1u : 0u;
    }
    CHECK(forward == 0);
    CHECK(backward == 1);
    CHECK(portrait.GetModelRoot() == root);
    CHECK(portrait.GetInstantiationCount() == 1);
    CHECK(portrait.GetOutput().Bounds.Min.x < -2.0f);

    // An emptied model shows nothing; restored, it is instantiated afresh.
    portrait.Model = {};
    Frame();
    CHECK_FALSE(portrait.GetOutput().Ready);
    portrait.Model = Cube;
    FrameUntilReady({&portrait});
    CHECK(portrait.GetInstantiationCount() == 2);
}

TEST_CASE_FIXTURE(PortraitFixture,
                  "model portrait: a released renderer is reused without a build, create/destroy "
                  "cycles hold the bindless occupancy, and an unpresented scene renders nothing")
{
    const Unique<Scene> unpresented = Scene::Create(Types);
    const Entity hidden = AddPortrait(*unpresented);

    const Entity first = AddPortrait(*Host);
    FrameUntilReady({&Host->Get<ModelPortrait>(first)});
    CHECK_FALSE(unpresented->Get<ModelPortrait>(hidden).HasRenderer());
    CHECK_FALSE(unpresented->Get<ModelPortrait>(hidden).GetOutput().Ready);

    // Removed, its renderer waits in the pool; the next portrait of its configuration takes it.
    Host->DestroyEntity(first);
    CHECK(Compositor->GetPortraitPool().GetHeldCount() == 1);
    const Entity second = AddPortrait(*Host);
    const ModelPortraitDriveResult reuse = Frame();
    CHECK(reuse.RenderersReused == 1);
    CHECK(reuse.RenderersBuilt == 0);
    Host->DestroyEntity(second);

    // A release is deferred past every frame in flight, so each reading follows a full cycle.
    const auto SettleReleases = [&]
    {
        Tasks.WaitForAll();
        Tasks.PumpMainThread();
        Assets->PumpFinalizes();
        for (u32 frame = 0; frame < Context.GetMaxFramesInFlight() + 1; ++frame)
        {
            Context.BeginFrame();
            Context.EndFrame();
        }
    };
    const auto Cycle = [&]
    {
        const Entity entity = AddPortrait(*Host);
        Frame();
        Frame();
        Host->DestroyEntity(entity);
        SettleReleases();
    };

    const BindlessRegistry& registry = Context.GetBindlessRegistry();
    Cycle();
    const BindlessCapacity before = registry.GetFreeSlots();
    bool steady = true;
    for (int cycle = 0; cycle < 16; ++cycle)
    {
        Cycle();
        const BindlessCapacity after = registry.GetFreeSlots();
        steady = steady && after.Textures == before.Textures && after.Samplers == before.Samplers &&
                 after.StorageImages == before.StorageImages &&
                 after.StorageBuffers == before.StorageBuffers &&
                 after.Materials == before.Materials;
    }
    CHECK(steady);
    CHECK_FALSE(unpresented->Get<ModelPortrait>(hidden).HasRenderer());
}

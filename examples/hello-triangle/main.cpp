#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Texture.h>
#include <Veng/UI/UI.h>

#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdlib>
#include <fstream>

using namespace Veng;

// A game-defined component spun about a fixed axis. Registered through the same
// public TypeRegistry::Register<T> path the engine uses for its builtins, so the
// scene stores, queries, and serializes a type the engine never sees at compile time.
struct Spinner
{
    f32 SpeedRadiansPerSec = 1.0f;
};

VE_REFLECT(Spinner, 0xAEF00D5EFC2444DAULL)
    VE_FIELD(SpeedRadiansPerSec, .DisplayName = "Speed", .Tooltip = "Radians per second", .Min = 0.0)
VE_REFLECT_END();

class HelloTriangleApp final : public Application
{
public:
    HelloTriangleApp(const ApplicationInfo& info, TypeRegistry& types) : Application(info, types)
    {
    }

protected:
    void OnInitialize() override
    {
        auto& context = GetRenderContext();

        const uvec2 sceneExtent = context.GetInternalRenderExtent();

        m_SmokeOutput = std::getenv("HT_SMOKE");

        // One SceneRenderer drives the main view; the sample composites its output,
        // the smoke path downloads it.
        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = GetAssetManager(),
            .OutputFormat = context.GetOutputFormat(),
            .Extent = sceneExtent,
            .Settings = m_SceneSettings,
        });

        // Mount from the executable's directory so the pack resolves wherever the
        // launcher is copied.
        const VoidResult mountResult = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        // The primitive generator records this material instance on the produced
        // submesh, so it must be resident before Mesh::Create hands it in.
        const AssetResult<AssetHandle<Veng::Material>> brickMaterial =
            GetAssetManager().LoadSync<Veng::Material>(AssetId{0x3EB});
        VE_ASSERT(brickMaterial.has_value(), "{}", brickMaterial.error().Detail);
        m_BrickMaterial = *brickMaterial;

        // Built at runtime, not cooked: an icosphere's near-uniform tessellation shows
        // the brick UV mapping without a UV sphere's pole clustering.
        const Ref<Veng::Mesh> sphere = Veng::Mesh::Create(
            context, Veng::Primitives::Icosphere(0.8f, 4, m_BrickMaterial), "Demo Sphere");

        // The composite path (ImGui overlay + swapchain present) is windowed-only; the
        // headless smoke run renders just the scene and downloads it.
        if (GetImGuiLayer())
        {
            // Edge-clamped so the "Scene" window's UI::Image never samples past the
            // renderer output.
            m_SceneSampler = Renderer::Sampler::Create(context, {
                .Name = "Scene Composite Sampler",
                .AddressModeU = Renderer::AddressMode::ClampToEdge,
                .AddressModeV = Renderer::AddressMode::ClampToEdge,
                .AddressModeW = Renderer::AddressMode::ClampToEdge,
            });
            m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());

            // Blends the ImGui overlay (holding the "Scene" window) over the scene
            // into the swapchain.
            m_Composite = Renderer::SwapChainCompositePass::Create({
                .Context = context,
                .ImGui = *GetImGuiLayer(),
                .Assets = GetAssetManager(),
                .SceneSource = m_SceneRenderer->GetOutput(),
                .SwapChainFormat = context.GetSwapChainFormat(),
            });

            // A swapchain resize invalidates the composite pass's baked extent, so
            // rebuild the composite graph against the new size. The SceneRenderer keeps
            // a fixed internal extent, so its output stays valid.
            context.AddSwapChainInvalidationCallback([this]
            {
                m_CompositeGraph = BuildCompositeGraph();
            });
        }

        // Spawn a cooked prefab carrying each entity's Transform, a MeshRenderer (its
        // Mesh field "no asset", since a runtime primitive has no content identity),
        // and the game-defined Spinner.
        m_Scene = Scene::Create(GetTypeRegistry());

        const AssetResult<AssetHandle<Veng::Prefab>> prefab =
            GetAssetManager().LoadSync<Veng::Prefab>(AssetId{0xA123F30FD219F2D5ULL});
        VE_ASSERT(prefab.has_value(), "{}", prefab.error().Detail);

        const vector<Entity> roots = prefab->Get()->SpawnInto(*m_Scene, GetAssetManager());
        VE_ASSERT(roots.size() >= 2, "prefab spawned fewer than the expected sphere + receiver-plane roots");

        // A flat receiver plane beneath the sphere, sharing the brick material, so the
        // shadow passes have a surface to catch the sphere's shadow. Built at runtime.
        const Ref<Veng::Mesh> plane = Veng::Mesh::Create(
            context, Veng::Primitives::Plane(vec2(4.0f), uvec2(1), m_BrickMaterial), "Receiver Plane");

        // Adopt the runtime meshes into AssetHandles and assign them to the spawned
        // renderers — wired in code because a prefab cannot reference a runtime resource
        // by id. The adopted handle owns the mesh's residency.
        m_Scene->Get<MeshRenderer>(roots[0]).Mesh = GetAssetManager().Adopt(sphere);
        m_Scene->Get<MeshRenderer>(roots[1]).Mesh = GetAssetManager().Adopt(plane);

        // Fixed direction so the smoke pose is lit reproducibly; intensity pushes facets
        // past 1.0 in linear HDR to give bloom something to act on.
        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 4.0f,
        };

        // A warm point light off to the right, exercising the lighting pass's
        // distance-attenuated accumulation alongside the directional light.
        const Entity pointEntity = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(pointEntity).Position = vec3(1.5f, 0.5f, 1.5f);
        m_Scene->Add<Light>(pointEntity) = Light{
            .Type = LightType::Point,
            .Color = vec3(1.0f, 0.6f, 0.3f),
            .Intensity = 6.0f,
            .Range = 6.0f,
        };

        // A Camera looking down -Z at the origin from (0,0,3).
        const f32 aspect = static_cast<f32>(sceneExtent.x) / static_cast<f32>(sceneExtent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        m_Camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        // Compiled once and replayed every frame; a swapchain resize rebuilds it via
        // the callback above.
        if (GetImGuiLayer())
            m_CompositeGraph = BuildCompositeGraph();
    }

    void OnUpdate(const f32 delta) override
    {
        m_LastDelta = delta;

        // Smoke mode pins the fixed pose so the capture is reproducible and
        // golden-compared; the windowed app advances each spinner's Transform by
        // speed * delta.
        if (m_SmokeOutput)
        {
            m_Scene->Each<Transform, Spinner>([](Entity, Transform& transform, Spinner&)
            {
                transform.Rotation = glm::angleAxis(SmokeAngle, SpinAxis);
            });
        }
        else if (!m_PauseSpin)
        {
            // Pausing skips this non-const Each, so the scene's spatial version stops
            // and the broadphase reads `static` — a still scene rebuilds the tree not at all.
            m_Scene->Each<Transform, Spinner>([delta](Entity, Transform& transform, Spinner& spinner)
            {
                const quat step = glm::angleAxis(spinner.SpeedRadiansPerSec * delta, SpinAxis);
                transform.Rotation = glm::normalize(step * transform.Rotation);
            });
        }

        // After a few rendered frames, dump the scene image and exit. Runs before this
        // frame's commands record, so the image holds the previous frame's contents.
        if (m_SmokeOutput && ++m_FrameCount == 20)
        {
            WriteSceneCapture(m_SmokeOutput);
            RequestExit();
        }
    }

    void OnRender() override
    {
        auto& context = GetRenderContext();
        auto& cmd = context.GetCurrentCommandBuffer();

        // BloomThreshold/BloomIntensity are per-frame SceneView values (no recompile):
        // a knee just below the brightest facets, with a modest mix, so highlights
        // bloom without washing out the scene.
        const Renderer::SceneView view{
            .World = *m_Scene,
            .Camera = m_Camera,
            .Delta = m_LastDelta,
            .BloomThreshold = 0.5f,
            .BloomIntensity = 1.5f,
        };
        m_SceneRenderer->Execute(cmd, view);

        // Headless (smoke) renders only the scene; the ImGui overlay and the
        // composite-to-swapchain pass are windowed-only.
        if (GetImGuiLayer())
        {
            // ImGui samples the scene output outside the graph, so transition it to a
            // sampleable layout before the overlay's read.
            cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);

            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        // Release every engine resource before the context tears down (see
        // Application::OnDispose).
        m_SceneRenderer.reset();
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Scene.reset();
        m_BrickMaterial = {};
    }

private:
    // Apply the current scene settings and re-bind the GetOutput()-derived handles:
    // Configure can recreate the output image, so the ImGui texture and the composite
    // pass's scene source must both be re-fetched.
    void ReconfigureScene()
    {
        m_SceneRenderer->Configure(m_SceneSettings);
        m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
        m_Composite->SetSceneSource(m_SceneRenderer->GetOutput());
    }

    void RenderUserInterface()
    {
        if (auto sceneWindow = UI::Window("Scene"))
        {
            // Entries mirror the DebugView enum in declaration order, so the combo index
            // is the enum value. Selecting one recompiles via ReconfigureScene.
            static constexpr std::array<string_view, 11> modeNames{
                "Final", "Albedo", "Normal", "Depth",
                "Roughness", "Metallic", "Occlusion",
                "AO", "Shadows", "Cascades", "Punctual shadows"};
            i32 mode = static_cast<i32>(m_SceneSettings.Mode);
            if (UI::Combo("View", mode, modeNames))
            {
                m_SceneSettings.Mode = static_cast<Renderer::DebugView>(mode);
                ReconfigureScene();
            }

            // The SSAO toggle is a topology change; recompiles via ReconfigureScene.
            if (UI::Checkbox("SSAO", m_SceneSettings.AO))
            {
                ReconfigureScene();
            }

            // Directional-shadow knobs. Shadows on/off and the cascade count / resolution
            // size the shadow atlas, so each recompiles via ReconfigureScene. The split
            // lambda is a per-frame value, routed through Configure only because the
            // example holds its settings there.
            if (UI::Checkbox("Shadows", m_SceneSettings.Shadows))
            {
                ReconfigureScene();
            }

            i32 cascadeCount = static_cast<i32>(m_SceneSettings.CascadeCount);
            if (UI::Slider("Cascades##count", cascadeCount, 1, static_cast<i32>(Renderer::MaxCascades)))
            {
                m_SceneSettings.CascadeCount = static_cast<u32>(cascadeCount);
                ReconfigureScene();
            }

            i32 shadowResolution = static_cast<i32>(m_SceneSettings.ShadowResolution);
            if (UI::Drag("Shadow resolution", shadowResolution,
                         {.Speed = 16.0f,
                          .Min = 256.0f,
                          .Max = static_cast<f32>(m_SceneRenderer->GetMaxShadowResolution())}))
            {
                m_SceneSettings.ShadowResolution = static_cast<u32>(shadowResolution);
                ReconfigureScene();
            }

            if (UI::Slider("Split lambda", m_SceneSettings.CascadeSplitLambda, {.Min = 0.0f, .Max = 1.0f}))
            {
                ReconfigureScene();
            }

            // Punctual-shadow knobs. On/off inserts/removes the punctual depth pass +
            // per-light sample, and the per-tile resolution sizes the punctual atlas, so
            // each recompiles via ReconfigureScene. The shadowed lights are the first
            // MaxShadowedPunctual the renderer selects.
            if (UI::Checkbox("Punctual shadows", m_SceneSettings.PunctualShadows))
            {
                ReconfigureScene();
            }

            i32 punctualResolution = static_cast<i32>(m_SceneSettings.PunctualShadowResolution);
            if (UI::Drag("Punctual shadow resolution", punctualResolution,
                         {.Speed = 16.0f,
                          .Min = 256.0f,
                          .Max = static_cast<f32>(m_SceneRenderer->GetMaxPunctualShadowResolution())}))
            {
                m_SceneSettings.PunctualShadowResolution = static_cast<u32>(punctualResolution);
                ReconfigureScene();
            }

            // Frustum culling toggling recompiles via ReconfigureScene (a no-op rebuild,
            // since the cull rewires no topology).
            if (UI::Checkbox("Frustum culling", m_SceneSettings.FrustumCull))
            {
                ReconfigureScene();
            }

            const vec2 available = UI::ContentRegionAvail();
            const Ref<Renderer::ImageView> output = m_SceneRenderer->GetOutput();
            const f32 aspect = static_cast<f32>(output->GetImage()->GetHeight()) /
                               static_cast<f32>(output->GetImage()->GetWidth());
            UI::Image(m_SceneTexture, {available.x, available.x * aspect});
        }

        if (auto statsWindow = UI::Window("Stats"))
        {
            UI::Text(fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()));

            // Drawn (post-cull, post-material-readiness) over gathered candidates. On
            // this minimal scene a steady n / n is expected; the readout earns its keep
            // on denser scenes.
            const u32 drawn = m_SceneRenderer->GetLastDrawnCount();
            const u32 total = m_SceneRenderer->GetLastVisibleCount();
            UI::Text(fmt::format("Meshes: {} / {}", drawn, total));

            // The broadphase rebuilds its BVH only on a frame the scene's spatial version
            // moved; a still scene reads `static`. The pause-spin toggle below stops the
            // per-frame Transform write, so the readout flips live.
            const bool rebuilt = m_SceneRenderer->DidBroadphaseRebuildLastFrame();
            UI::Text(fmt::format("Broadphase: {} ({} nodes)",
                                 rebuilt ? "rebuilt" : "static",
                                 m_SceneRenderer->GetBroadphaseNodeCount()));

            // The checkbox writes m_PauseSpin directly; its "changed" return is unused —
            // the pause takes effect next frame in OnUpdate regardless.
            (void)UI::Checkbox("Pause spin", m_PauseSpin);
        }
    }

    void WriteSceneCapture(const char* outPath) const
    {
        const Ref<Renderer::Image> output = m_SceneRenderer->GetOutput()->GetImage();
        const auto data = output->Download();
        const u32 width = output->GetWidth();
        const u32 height = output->GetHeight();

        // Scene output is RGBA16F; decode to 8-bit RGB for a binary PPM.
        const auto* halves = reinterpret_cast<const u16*>(data.data());

        std::ofstream out(outPath, std::ios::binary);
        out << "P6\n" << width << " " << height << "\n255\n";

        for (u32 pixel = 0; pixel < width * height; pixel++)
        {
            for (u32 channel = 0; channel < 3; channel++)
            {
                const f32 value = glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                out.put(static_cast<char>(value * 255.0f + 0.5f));
            }
        }

        Log::Info("Wrote scene capture to {}", outPath);
    }

    // Import the swapchain target and hand it to the composite pass's Compile. A
    // swapchain resize re-runs this against the new extent.
    Unique<Renderer::CompiledGraph> BuildCompositeGraph()
    {
        Renderer::RenderGraph graph(GetRenderContext());
        const Renderer::ResourceId swapId = graph.Import("SwapChain");
        return m_Composite->Compile(graph, swapId);
    }

    void CompositeToSwapChain(Renderer::CommandBuffer& cmd)
    {
        m_Composite->Execute(cmd, *m_CompositeGraph, GetRenderContext().GetCurrentSwapChainImageView());
    }

    Unique<Renderer::SceneRenderer> m_SceneRenderer;
    Renderer::SceneRendererSettings m_SceneSettings;

    AssetHandle<Veng::Material> m_BrickMaterial;

    // The scene output surfaced in the "Scene" window via UI::Image: an ImGui
    // texture over the renderer output, recreated when Configure invalidates it.
    Ref<Renderer::Sampler> m_SceneSampler;
    Ref<ImGuiTexture> m_SceneTexture;

    // The fullscreen scene-behind-ImGui composite into the swapchain + its bindless
    // slots and pipeline.
    Unique<Renderer::SwapChainCompositePass> m_Composite;

    // Compiled once and replayed every frame; re-Compile()d on swapchain resize.
    Unique<Renderer::CompiledGraph> m_CompositeGraph;

    // The fixed rotation the smoke capture renders, in radians.
    static constexpr f32 SmokeAngle = 0.9f;
    // The axis the spinner rotates about.
    static inline const vec3 SpinAxis = glm::normalize(vec3(0.5f, 1.0f, 0.2f));

    Unique<Scene> m_Scene;
    Camera m_Camera;

    f32 m_LastDelta = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;

    // Windowed-only: when set, OnUpdate skips the spinner's per-frame Transform write,
    // so the broadphase reads `static`. Never set in smoke mode, so the golden capture
    // is untouched.
    bool m_PauseSpin = false;
};

// The module's entry point: the launcher dlopens this library and calls it once to
// register the factory that constructs the Application. The factory captures the
// headless decision and the ApplicationInfo, so the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The game registers its own component types through the same public path the
    // engine uses for its builtins. The engine has no compile-time knowledge of Spinner.
    host->Types.Register<Spinner>();

    // Smoke mode runs headless: no window or swapchain, render off-screen and dump
    // it — the display-free CI path.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    host->App.RegisterApplication([smoke](TypeRegistry& types)
    {
        return Unique<Application>(new HelloTriangleApp(ApplicationInfo{
            .Name = "Hello Triangle",
            .InternalRenderExtent = {1280, 720},
            .WindowInfo = {
                .Extent = {1280, 720},
                .Resizable = false,
                .EventCallback = [](Event&) {},
                .Title = "veng — Hello Triangle",
                .CaptureMouse = false,
            },
            .Headless = smoke,
            // Persist the pipeline cache beside the launcher, the same
            // executable-relative resolution the asset pack uses.
            .PipelineCachePath = ExecutableDirectory() / "pipeline_cache.bin",
        }, types));
    });
}

VE_EXPORT_MODULE_ABI()

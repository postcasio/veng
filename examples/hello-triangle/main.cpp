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

// A game-defined component: spins its entity about a fixed axis. The engine has
// no compile-time knowledge of this type — it is registered through the same
// public TypeRegistry::Register<T> path the engine uses for its own builtins,
// proving the scene stores, queries, and (via the reflection layer) serializes
// a type defined entirely in the game's translation unit.
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

        // The main view renders through one SceneRenderer: it owns its offscreen
        // output (created at the context's output format), the deferred g-buffer,
        // an internal compiled graph, and the deferred draw. It loads its
        // fullscreen-blit shaders from the engine core pack through the asset
        // manager. The sample composites GetOutput() and the smoke path downloads
        // it.
        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = GetAssetManager(),
            .OutputFormat = context.GetOutputFormat(),
            .Extent = sceneExtent,
            .Settings = m_SceneSettings,
        });

        // Cooked at build time (see CMakeLists.txt) and copied beside the launcher
        // by veng_add_game; mount it from the executable's directory so the trio
        // (launcher + module + pack) resolves wherever it is copied. Loading the
        // brick material pulls in its vertex/fragment shaders and the brick texture
        // as eager dependencies, builds its bindless pipeline, and writes its
        // parameter block into the registry's per-material buffer.
        const VoidResult mountResult = GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        // The primitive generator records this material instance on the produced
        // submesh, so it must be resident before Mesh::Create hands it in.
        const AssetResult<AssetHandle<Veng::Material>> brickMaterial =
            GetAssetManager().LoadSync<Veng::Material>(AssetId{0x3EB});
        VE_ASSERT(brickMaterial.has_value(), "{}", brickMaterial.error().Detail);
        m_BrickMaterial = *brickMaterial;

        // Build the geometry at runtime rather than loading a cooked mesh: a
        // geodesic icosphere in the canonical layout, carrying the brick material
        // instance on its single submesh. Its near-uniform tessellation shows the
        // brick UV mapping without the pole clustering of a UV sphere.
        // Mesh::Create uploads synchronously, so it is ready to draw.
        const Ref<Veng::Mesh> sphere = Veng::Mesh::Create(
            context, Veng::Primitives::Icosphere(0.8f, 4, m_BrickMaterial), "Demo Sphere");

        // The compositing path (ImGui overlay + swapchain present) only exists in
        // windowed mode. The headless smoke run renders just the scene and
        // downloads it — no ImGui layer, no swapchain.
        if (GetImGuiLayer())
        {
            // The scene shows inside a "Scene" ImGui window via UI::Image: an ImGui
            // texture over the renderer output, edge-clamped so the window never
            // samples past the image.
            m_SceneSampler = Renderer::Sampler::Create(context, {
                .Name = "Scene Composite Sampler",
                .AddressModeU = Renderer::AddressMode::ClampToEdge,
                .AddressModeV = Renderer::AddressMode::ClampToEdge,
                .AddressModeW = Renderer::AddressMode::ClampToEdge,
            });
            m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());

            // The fullscreen pass that blends the rendered ImGui overlay (which holds
            // that "Scene" window) over the scene into the swapchain.
            m_Composite = Renderer::SwapChainCompositePass::Create({
                .Context = context,
                .ImGui = *GetImGuiLayer(),
                .Assets = GetAssetManager(),
                .SceneSource = m_SceneRenderer->GetOutput(),
                .SwapChainFormat = context.GetSwapChainFormat(),
            });

            // A swapchain resize invalidates the composite pass's baked extent;
            // re-Compile() the composite graph against the new size. The
            // SceneRenderer keeps a fixed internal extent, so its output view stays
            // valid and is not re-registered. The headless smoke path has a fixed
            // extent, so this never fires there.
            context.AddSwapChainInvalidationCallback([this]
            {
                m_CompositeGraph = BuildCompositeGraph();
            });
        }

        // Build the runtime scene by spawning a cooked prefab: it carries the
        // entity's local Transform, a MeshRenderer (its Mesh field "no asset",
        // since a runtime primitive has no content identity), and the game-defined
        // Spinner. The Scene is an engine primitive — created empty and populated
        // by spawning, never loaded.
        m_Scene = Scene::Create(GetTypeRegistry());

        const AssetResult<AssetHandle<Veng::Prefab>> prefab =
            GetAssetManager().LoadSync<Veng::Prefab>(AssetId{0xA123F30FD219F2D5ULL});
        VE_ASSERT(prefab.has_value(), "{}", prefab.error().Detail);

        const vector<Entity> roots = prefab->Get()->SpawnInto(*m_Scene, GetAssetManager());
        VE_ASSERT(roots.size() >= 2, "prefab spawned fewer than the expected sphere + receiver-plane roots");

        // A flat receiver plane beneath the sphere, sharing the brick material, so
        // the deferred lighting reads across two surfaces (and later plans have
        // geometry to shadow/occlude). Built at runtime like the sphere.
        const Ref<Veng::Mesh> plane = Veng::Mesh::Create(
            context, Veng::Primitives::Plane(vec2(4.0f), uvec2(1), m_BrickMaterial), "Receiver Plane");

        // Adopt the runtime meshes into AssetHandles and assign them to the spawned
        // renderers — the one piece wired in code, because the prefab cannot
        // reference a runtime resource by id. The adopted handle owns the mesh's
        // residency; dropping the scene drops the component, the handle, and the
        // mesh in turn.
        m_Scene->Get<MeshRenderer>(roots[0]).Mesh = GetAssetManager().Adopt(sphere);
        m_Scene->Get<MeshRenderer>(roots[1]).Mesh = GetAssetManager().Adopt(plane);

        // A directional light so the deferred lighting pass shades the scene. Its
        // direction is fixed (top-front-right toward the origin), so the smoke pose
        // is lit reproducibly run to run. The intensity drives the lit facets past
        // 1.0 in linear HDR, so the bloom chain has a bright region to act on.
        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 4.0f,
        };

        // A warm point light off to the right, placed by its Transform, exercising
        // the lighting pass's distance-attenuated accumulation loop alongside the
        // directional light.
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

        // The composite graph is compiled once and replayed every frame; a swapchain
        // resize re-Compile()s it via the invalidation callback above. The
        // SceneRenderer compiles its own graph in Create().
        if (GetImGuiLayer())
            m_CompositeGraph = BuildCompositeGraph();
    }

    void OnUpdate(const f32 delta) override
    {
        m_LastDelta = delta;

        // Drive rotation through the scene query: each spinning entity advances
        // its Transform's quaternion about the fixed axis by its speed * delta.
        // Smoke mode pins the fixed pose so the capture is reproducible run to
        // run and can be golden-compared; the windowed app rotates by
        // accumulated wall-clock delta.
        if (m_SmokeOutput)
        {
            m_Scene->Each<Transform, Spinner>([](Entity, Transform& transform, Spinner&)
            {
                transform.Rotation = glm::angleAxis(SmokeAngle, SpinAxis);
            });
        }
        else
        {
            m_Scene->Each<Transform, Spinner>([delta](Entity, Transform& transform, Spinner& spinner)
            {
                const quat step = glm::angleAxis(spinner.SpeedRadiansPerSec * delta, SpinAxis);
                transform.Rotation = glm::normalize(step * transform.Rotation);
            });
        }

        // Smoke-test mode: after a few rendered frames, dump the scene image to
        // disk and exit. Runs before this frame's commands are recorded, so the
        // image holds the previous frame's completed contents.
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
        // a knee just below the lit brick's brightest facets, with a modest mix, so
        // the bright highlights bloom visibly without washing out the scene.
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
            // RenderUserInterface() draws the scene texture via UI::Image(), and
            // GetImGuiLayer()->Render(cmd) is what actually records that sampled read.
            // ImGui samples outside the graph, so transition the output to a
            // sampleable layout before that read.
            cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);

            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        // The SceneRenderer owns the scene output + depth transient; the composite
        // graph owns its imports' bindings; the composite pass owns the composite
        // pipeline and its bindless slots; the ImGui texture + sampler surface the
        // scene in a window. Release them so their GPU resources retire before the
        // context tears down.
        m_SceneRenderer.reset();
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Scene.reset();
        m_BrickMaterial = {};
    }

private:
    void RenderUserInterface()
    {
        if (auto sceneWindow = UI::Window("Scene"))
        {
            // The DebugView combo drives SceneRenderer::Configure, re-wiring the pass
            // set (Final / Albedo / Normal / Depth) — a live exercise of the recompile
            // seam. Configure recreates the output image, so the ImGui texture and the
            // composite pass's scene bindless slot must both be re-bound after it (the
            // GetOutput()-invalidated-by-Configure contract).
            static constexpr std::array<string_view, 4> modeNames{"Final", "Albedo", "Normal", "Depth"};
            i32 mode = static_cast<i32>(m_SceneSettings.Mode);
            if (UI::Combo("View", mode, modeNames))
            {
                m_SceneSettings.Mode = static_cast<Renderer::DebugView>(mode);
                m_SceneRenderer->Configure(m_SceneSettings);
                m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
                m_Composite->SetSceneSource(m_SceneRenderer->GetOutput());
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

    // Build and compile the composite graph through the engine composite pass: the
    // app imports the swapchain target and hands it to Compile; the pass declares
    // the scene/ImGui imports, wires the fullscreen pass, and Compile()s. A
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
    // The axis the spinner rotates about — the same axis the prior hand-rolled
    // model matrix used, so the rendered orientation is unchanged.
    static inline const vec3 SpinAxis = glm::normalize(vec3(0.5f, 1.0f, 0.2f));

    Unique<Scene> m_Scene;
    Camera m_Camera;

    f32 m_LastDelta = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;
};

// The module's entry point: the launcher dlopens this library and calls this
// once, and the game registers the factory that constructs its Application. The
// factory captures the HT_SMOKE/headless decision and the ApplicationInfo, so
// they live in the module beside the app — the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The game registers its own component types here, through the same public
    // path the engine pre-registers its builtins (already present on the host
    // registry). The engine has no compile-time knowledge of Spinner.
    host->Types.Register<Spinner>();

    // Smoke mode runs headless: no window or swapchain, render the scene
    // off-screen and dump it. This is the display-free CI path enabled by the
    // headless context.
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
            // Persist the pipeline cache beside the launcher — the same
            // executable-relative resolution the asset pack uses, so the cache
            // stays with the relocatable trio.
            .PipelineCachePath = ExecutableDirectory() / "pipeline_cache.bin",
        }, types));
    });
}

VE_EXPORT_MODULE_ABI()

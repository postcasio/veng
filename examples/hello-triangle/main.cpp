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
#include <Veng/Asset/Prefab.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Texture.h>
#include <Veng/UI/UI.h>

#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace Veng;

// A game-defined component spun about a fixed axis; registered through the public
// TypeRegistry so the scene stores, queries, and serializes it without engine knowledge.
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
    HelloTriangleApp(const ApplicationInfo& info, TypeRegistry& types) : Application(info, types) {}

protected:
    void OnInitialize() override
    {
        auto& context = GetRenderContext();

        const uvec2 sceneExtent = context.GetInternalRenderExtent();

        m_SmokeOutput = std::getenv("HT_SMOKE");

        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = GetAssetManager(),
            .OutputFormat = context.GetOutputFormat(),
            .Extent = sceneExtent,
            .Settings = m_SceneSettings,
        });

        // Executable-relative so the pack resolves wherever the launcher is copied.
        const VoidResult mountResult =
            GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        if (GetImGuiLayer())
        {
            // Edge-clamped so the "Scene" window's UI::Image never samples past the
            // renderer output.
            m_SceneSampler = Renderer::Sampler::Create(
                context, {
                             .Name = "Scene Composite Sampler",
                             .AddressModeU = Renderer::AddressMode::ClampToEdge,
                             .AddressModeV = Renderer::AddressMode::ClampToEdge,
                             .AddressModeW = Renderer::AddressMode::ClampToEdge,
                         });
            m_SceneTexture =
                GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());

            m_Composite = Renderer::SwapChainCompositePass::Create({
                .Context = context,
                .ImGui = *GetImGuiLayer(),
                .Assets = GetAssetManager(),
                .SceneSource = m_SceneRenderer->GetOutput(),
                .SwapChainFormat = context.GetSwapChainFormat(),
            });

            // Swapchain resize invalidates the composite graph's baked extent; SceneRenderer
            // keeps a fixed internal extent so its output stays valid.
            context.AddSwapChainInvalidationCallback([this]
                                                     { m_CompositeGraph = BuildCompositeGraph(); });
        }

        m_Scene = Scene::Create(GetTypeRegistry());

        const AssetResult<AssetHandle<Veng::Prefab>> prefab =
            GetAssetManager().LoadSync<Veng::Prefab>(AssetId{0xA123F30FD219F2D5ULL});
        VE_ASSERT(prefab.has_value(), "{}", prefab.error().Detail);

        const vector<Entity> roots = prefab->Get()->SpawnInto(*m_Scene, GetAssetManager());
        VE_ASSERT(roots.size() >= 2,
                  "prefab spawned fewer than the expected sphere + receiver-plane roots");

        // Smoke renders a fixed pose, so block until the streamed primitives are resident
        // before the capture frame; the windowed app lets them appear over a few frames.
        if (m_SmokeOutput)
        {
            WaitForPrimitiveResidency();
        }

        // Fixed direction for a reproducible smoke pose; intensity pushes facets past 1.0
        // in linear HDR so bloom has something to act on.
        const Entity lightEntity = m_Scene->CreateEntity();
        m_Scene->Add<Light>(lightEntity) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(-0.4f, -0.7f, -0.5f)),
            .Color = vec3(1.0f, 1.0f, 1.0f),
            .Intensity = 4.0f,
        };

        // Warm point light: exercises distance-attenuated accumulation alongside the directional.
        const Entity pointEntity = m_Scene->CreateEntity();
        m_Scene->Add<Transform>(pointEntity).Position = vec3(1.5f, 0.5f, 1.5f);
        m_Scene->Add<Light>(pointEntity) = Light{
            .Type = LightType::Point,
            .Color = vec3(1.0f, 0.6f, 0.3f),
            .Intensity = 6.0f,
            .Range = 6.0f,
        };

        const f32 aspect = static_cast<f32>(sceneExtent.x) / static_cast<f32>(sceneExtent.y);
        m_Camera.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        m_Camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

        if (GetImGuiLayer())
        {
            m_CompositeGraph = BuildCompositeGraph();
        }
    }

    void OnUpdate(const f32 delta) override
    {
        m_LastDelta = delta;

        // Smoke mode pins a fixed pose for golden comparison; otherwise advance each spinner.
        if (m_SmokeOutput)
        {
            m_Scene->Each<Transform, Spinner>(
                [](Entity, Transform& transform, Spinner&)
                { transform.Rotation = glm::angleAxis(SmokeAngle, SpinAxis); });
        }
        else if (!m_PauseSpin)
        {
            // Skipping this non-const Each stops the spatial version, so the broadphase reads `static`.
            m_Scene->Each<Transform, Spinner>(
                [delta](Entity, Transform& transform, Spinner& spinner)
                {
                    const quat step = glm::angleAxis(spinner.SpeedRadiansPerSec * delta, SpinAxis);
                    transform.Rotation = glm::normalize(step * transform.Rotation);
                });
        }

        // Runs before this frame's commands record, so the image holds the previous frame's contents.
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

        // Knee just below the brightest facets; modest mix so highlights bloom without
        // washing out the scene.
        const Renderer::SceneView view{
            .World = *m_Scene,
            .Camera = m_Camera,
            .Delta = m_LastDelta,
            .BloomThreshold = 0.5f,
            .BloomIntensity = 1.5f,
        };
        m_SceneRenderer->Execute(cmd, view);

        if (GetImGuiLayer())
        {
            // ImGui samples the scene output outside the graph; transition before the overlay reads it.
            cmd.PrepareForAccess(m_SceneRenderer->GetOutput(), Renderer::AccessKind::Sample);

            RenderUserInterface();
            GetImGuiLayer()->Render(cmd);
            CompositeToSwapChain(cmd);
        }
    }

    void OnDispose() override
    {
        m_SceneRenderer.reset();
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Scene.reset();
    }

private:
    // Drains the task system until every Primitive's streamed mesh is resident.
    // The async build finalizes on the main-thread continuation pump, so each iteration
    // pumps it and yields the worker a moment to complete the upload.
    void WaitForPrimitiveResidency()
    {
        const auto allResident = [this]
        {
            bool resident = true;
            m_Scene->Each<Primitive, MeshRenderer>(
                [&resident](Entity, Primitive&, MeshRenderer& renderer)
                {
                    if (!renderer.Mesh.IsLoaded())
                    {
                        resident = false;
                    }
                });
            return resident;
        };

        while (!allResident())
        {
            GetTaskSystem().PumpMainThread();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Configure can recreate the output image, so the ImGui texture and composite
    // scene source must be re-fetched after each call.
    void ReconfigureScene()
    {
        m_SceneRenderer->Configure(m_SceneSettings);
        m_SceneTexture =
            GetImGuiLayer()->CreateTexture(*m_SceneSampler, *m_SceneRenderer->GetOutput());
        m_Composite->SetSceneSource(m_SceneRenderer->GetOutput());
    }

    void RenderUserInterface()
    {
        if (auto sceneWindow = UI::Window("Scene"))
        {
            // Entries mirror the DebugView enum in declaration order; combo index == enum value.
            static constexpr std::array<string_view, 11> modeNames{
                "Final",     "Albedo", "Normal",  "Depth",    "Roughness",       "Metallic",
                "Occlusion", "AO",     "Shadows", "Cascades", "Punctual shadows"};
            i32 mode = static_cast<i32>(m_SceneSettings.Mode);
            if (UI::Combo("View", mode, modeNames))
            {
                m_SceneSettings.Mode = static_cast<Renderer::DebugView>(mode);
                ReconfigureScene();
            }

            // SSAO toggle is a topology change.
            if (UI::Checkbox("SSAO", m_SceneSettings.AO))
            {
                ReconfigureScene();
            }

            // Shadows on/off and cascade count/resolution size the atlas; each requires ReconfigureScene.
            if (UI::Checkbox("Shadows", m_SceneSettings.Shadows))
            {
                ReconfigureScene();
            }

            i32 cascadeCount = static_cast<i32>(m_SceneSettings.CascadeCount);
            if (UI::Slider("Cascades##count", cascadeCount, 1,
                           static_cast<i32>(Renderer::MaxCascades)))
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

            if (UI::Slider("Split lambda", m_SceneSettings.CascadeSplitLambda,
                           {.Min = 0.0f, .Max = 1.0f}))
            {
                ReconfigureScene();
            }

            // On/off adds/removes the punctual depth pass; resolution sizes the atlas. Each requires ReconfigureScene.
            if (UI::Checkbox("Punctual shadows", m_SceneSettings.PunctualShadows))
            {
                ReconfigureScene();
            }

            i32 punctualResolution = static_cast<i32>(m_SceneSettings.PunctualShadowResolution);
            if (UI::Drag(
                    "Punctual shadow resolution", punctualResolution,
                    {.Speed = 16.0f,
                     .Min = 256.0f,
                     .Max = static_cast<f32>(m_SceneRenderer->GetMaxPunctualShadowResolution())}))
            {
                m_SceneSettings.PunctualShadowResolution = static_cast<u32>(punctualResolution);
                ReconfigureScene();
            }

            // No topology change, but ReconfigureScene is the uniform path for all settings.
            if (UI::Checkbox("Frustum culling", m_SceneSettings.FrustumCull))
            {
                ReconfigureScene();
            }

            // The GPU arm is a different pass topology, so the selector and the occlusion
            // toggle both drive ReconfigureScene. CullMode::GPU degrades to CPU on a device
            // without multiDrawIndirect; the active-mode line in Stats shows the real path.
            static constexpr std::array<string_view, 2> cullNames{"CPU", "GPU"};
            i32 cull = static_cast<i32>(m_SceneSettings.Cull);
            if (UI::Combo("Cull mode", cull, cullNames))
            {
                m_SceneSettings.Cull = static_cast<Renderer::SceneRendererSettings::CullMode>(cull);
                ReconfigureScene();
            }

            if (UI::Checkbox("GPU occlusion", m_SceneSettings.Occlusion))
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
            UI::Text(
                fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()));

            // The cull funnel: gathered submesh candidates → frustum survivors → draws issued.
            const u32 gathered = m_SceneRenderer->GetLastVisibleCount();
            const u32 frustum = m_SceneRenderer->GetFrustumSurvivedCount();
            const u32 drawn = m_SceneRenderer->GetLastDrawnCount();
            UI::Text(fmt::format("Gathered: {}", gathered));
            UI::Text(fmt::format("Frustum survived: {}", frustum));
            UI::Text(fmt::format("Drawn: {}", drawn));

            // Under the GPU path the occlusion test zeroes occluded commands' instanceCount;
            // the survivor count is read back one frame late. The active line shows the real
            // mode (GPU degrades to CPU on a device without multiDrawIndirect).
            const bool gpuActive = m_SceneRenderer->GetActiveCullMode() ==
                                   Renderer::SceneRendererSettings::CullMode::GPU;
            UI::Text(fmt::format("Cull mode: {}", gpuActive ? "GPU" : "CPU"));
            if (gpuActive)
            {
                UI::Text(fmt::format("Occlusion survived: {}",
                                     m_SceneRenderer->GetLastGpuSurvivorCount()));
            }

            // Flips live as the pause-spin toggle stops/resumes the per-frame Transform write.
            const bool rebuilt = m_SceneRenderer->DidBroadphaseRebuildLastFrame();
            UI::Text(fmt::format("Broadphase: {} ({} nodes)", rebuilt ? "rebuilt" : "static",
                                 m_SceneRenderer->GetBroadphaseNodeCount()));

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
                const f32 value =
                    glm::clamp(glm::unpackHalf1x16(halves[pixel * 4 + channel]), 0.0f, 1.0f);
                out.put(static_cast<char>(value * 255.0f + 0.5f));
            }
        }

        Log::Info("Wrote scene capture to {}", outPath);
    }

    // Re-run on swapchain resize to rebuild against the new extent.
    Unique<Renderer::CompiledGraph> BuildCompositeGraph()
    {
        Renderer::RenderGraph graph(GetRenderContext());
        const Renderer::ResourceId swapId = graph.Import("SwapChain");
        return m_Composite->Compile(graph, swapId);
    }

    void CompositeToSwapChain(Renderer::CommandBuffer& cmd)
    {
        m_Composite->Execute(cmd, *m_CompositeGraph,
                             GetRenderContext().GetCurrentSwapChainImageView());
    }

    Unique<Renderer::SceneRenderer> m_SceneRenderer;
    Renderer::SceneRendererSettings m_SceneSettings;

    // Recreated when Configure invalidates the output image.
    Ref<Renderer::Sampler> m_SceneSampler;
    Ref<ImGuiTexture> m_SceneTexture;

    Unique<Renderer::SwapChainCompositePass> m_Composite;

    // Re-Compile()d on swapchain resize.
    Unique<Renderer::CompiledGraph> m_CompositeGraph;

    // Fixed rotation for the smoke capture, in radians.
    static constexpr f32 SmokeAngle = 0.9f;
    static inline const vec3 SpinAxis = glm::normalize(vec3(0.5f, 1.0f, 0.2f));

    Unique<Scene> m_Scene;
    CameraView m_Camera;

    f32 m_LastDelta = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;

    // Skips the per-frame Transform write so the broadphase reads `static`; never set in smoke mode.
    bool m_PauseSpin = false;
};

// Factory captures the headless flag so the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Spinner>();

    // Smoke mode: no window or swapchain, render off-screen and dump — the display-free CI path.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    host->App.RegisterApplication(
        [smoke](TypeRegistry& types)
        {
            return Unique<Application>(new HelloTriangleApp(
                ApplicationInfo{
                    .Name = "Hello Triangle",
                    .InternalRenderExtent = {1280, 720},
                    .WindowInfo =
                        {
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
                },
                types));
        });
}

VE_EXPORT_MODULE_ABI()

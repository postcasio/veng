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
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Texture.h>
#include <Veng/UI/UI.h>

#include <Veng/Input.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Task/TaskSystem.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace Veng;

// A game-defined component that spins its entity about its own axis at its own speed;
// registered through the public TypeRegistry so the scene stores, queries, and serializes
// it without engine knowledge.
struct Spinner
{
    f32 SpeedRadiansPerSec = 1.0f;
    vec3 Axis = vec3(0.0f, 1.0f, 0.0f);
};

VE_REFLECT(Spinner, 0xAEF00D5EFC2444DAULL)
VE_FIELD(SpeedRadiansPerSec, .DisplayName = "Speed", .Tooltip = "Radians per second", .Min = 0.0)
VE_FIELD(Axis, .DisplayName = "Axis", .Tooltip = "Spin axis (normalized at runtime)")
VE_REFLECT_END();

// Advances every Spinner each frame about its own axis — the gameplay tick the windowed
// app drives through a SceneSimulation. Registered into the host SystemRegistry alongside
// the Spinner type.
class SpinnerSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32 delta, const SystemContext&) override
    {
        scene.Each<Transform, Spinner>(
            [delta](Entity, Transform& transform, Spinner& spinner)
            {
                const quat step = glm::angleAxis(spinner.SpeedRadiansPerSec * delta,
                                                 glm::normalize(spinner.Axis));
                transform.Rotation = glm::normalize(step * transform.Rotation);
            });
    }
};

VE_SYSTEM(SpinnerSystem, 0xB5BB5153EC6ACDDEULL, "Spinner");

// The game's button-bit layout for a PlayerInput's bitset. Bit meanings are game
// policy; the engine treats the bitset as opaque.
enum class PlayerButton : u32
{
    Jump = 1u << 0,
};

// Maps a captured PlayerInput snapshot to an abstract Intent — the game-specific control
// policy. Pure: the same snapshot always yields the same Intent, whether it came from the
// device, a recording, or the wire, so it is unit-testable without an Input or a scene.
Intent MapInputToIntent(const PlayerInput& input)
{
    Intent intent;
    intent.Move = input.Move;
    // Look maps device deltas straight through; the Mover's TurnSpeed scales them.
    intent.Look = input.Look;
    intent.Actions = input.Buttons;
    return intent;
}

// The game-specific control system: reads the always-present Veng::Input each tick into
// the local player's PlayerInput snapshot, then maps that snapshot to the possessed pawn's
// Intent through MapInputToIntent. It reads Input unconditionally — in headless the service
// reports all-zeros, so it naturally produces a zero Intent and the pawn stays put, with no
// null to guard. It writes Intent through the scene accessor, never a retained reference.
class ControlSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext& context) override
    {
        const Input& input = context.Input;

        // WASD strafes/advances in the pawn's local frame; Space requests a jump action.
        vec3 move{0.0f};
        if (input.IsKeyDown(Key::W))
        {
            move.z += 1.0f;
        }
        if (input.IsKeyDown(Key::S))
        {
            move.z -= 1.0f;
        }
        if (input.IsKeyDown(Key::D))
        {
            move.x += 1.0f;
        }
        if (input.IsKeyDown(Key::A))
        {
            move.x -= 1.0f;
        }

        u32 buttons = 0;
        if (input.IsKeyDown(Key::Space))
        {
            buttons |= static_cast<u32>(PlayerButton::Jump);
        }

        const vec2 look = input.GetMouseDelta();

        scene.Each<PlayerInput, Possesses>(
            [&](Entity, PlayerInput& player, Possesses& possesses)
            {
                player.Move = move;
                player.Look = look;
                player.Buttons = buttons;

                // The seat may possess no pawn, or one that lacks an Intent slot; skip
                // rather than fault, so an unwired seat is inert.
                if (possesses.Pawn == Entity::Null || !scene.IsAlive(possesses.Pawn) ||
                    !scene.Has<Intent>(possesses.Pawn))
                {
                    return;
                }

                scene.Get<Intent>(possesses.Pawn) = MapInputToIntent(player);
            });
    }
};

VE_SYSTEM(ControlSystem, 0x1C2F5C03357C19B2ULL, "Control");

// The game mode's spawn rule: a Sim-phase system that instantiates the configured player
// prefab when the Session is Playing, and tears it down when the session ends or play stops.
// The player prefab authors its own Viewer/Possesses/Camera/CameraFollow wiring, so the
// rule only picks (the GameModeConfig's PlayerPrefab) and spawns — no imperative wiring. It
// spawns at OnStart, before the first Update, so the spawn is deterministic and the pinned
// smoke frame (which never ticks Update) renders the authored camera pose.
class SpawnPlayerRule final : public SceneSystem
{
public:
    void OnStart(Scene& scene, const SystemContext& context) override
    {
        const Entity session = FindSession(scene);
        if (session == Entity::Null || scene.Get<Session>(session).Phase != SessionPhase::Playing)
        {
            return;
        }

        const GameModeConfig& config = scene.Get<GameModeConfig>(session);

        // The config's player prefab is eager-loaded as a dependency of the scene prefab,
        // so it is resident by the time the simulation starts; skip if it is not.
        if (!config.PlayerPrefab.IsLoaded())
        {
            return;
        }

        m_Spawned = config.PlayerPrefab.Get()->SpawnInto(scene, context.Assets);
    }

    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        // The spawn happens once at OnStart; a scoring / win-condition rule is the obvious
        // second system. Here the only per-tick rule action is tearing the player down when
        // the session ends.
        const Entity session = FindSession(scene);
        if (session != Entity::Null && !m_Spawned.empty() &&
            scene.Get<Session>(session).Phase == SessionPhase::Ended)
        {
            Despawn(scene);
        }
    }

    void OnStop(Scene& scene, const SystemContext&) override { Despawn(scene); }

private:
    // Returns the well-known session entity (the one carrying both Session and its config),
    // or Entity::Null if the scene authors no game mode.
    static Entity FindSession(Scene& scene)
    {
        Entity found = Entity::Null;
        scene.Each<Session, GameModeConfig>([&found](const Entity entity, Session&, GameModeConfig&)
                                            { found = entity; });
        return found;
    }

    void Despawn(Scene& scene)
    {
        for (const Entity entity : m_Spawned)
        {
            if (scene.IsAlive(entity))
            {
                scene.DestroyEntity(entity);
            }
        }
        m_Spawned.clear();
    }

    vector<Entity> m_Spawned;
};

VE_SYSTEM(SpawnPlayerRule, 0x70CCE23C99D1C3A1ULL, "Spawn Player Rule");

class HelloTriangleApp final : public Application
{
public:
    HelloTriangleApp(const ApplicationInfo& info, TypeRegistry& types, SystemRegistry& systems)
        : Application(info, types, systems)
    {
    }

protected:
    void OnInitialize() override
    {
        auto& context = GetRenderContext();

        const uvec2 sceneExtent = context.GetInternalRenderExtent();

        m_SmokeOutput = std::getenv("HT_SMOKE");

        // Executable-relative so the pack resolves wherever the launcher is copied.
        const VoidResult mountResult =
            GetAssetManager().Mount(ExecutableDirectory() / "sample.vengpack");
        VE_ASSERT(mountResult, "{}", mountResult.error());

        // The level is the authored home for the world, the active system set, the game-mode
        // config, and the render settings. Loading it resolves the world prefab (and the
        // game-mode player prefab) as ordinary load-time dependencies.
        const AssetResult<AssetHandle<Level>> level =
            GetAssetManager().LoadSync<Level>(AssetId{0x95C2E76206A11F08ULL});
        VE_ASSERT(level.has_value(), "{}", level.error().Detail);

        // The level's render subset seeds the renderer settings and the per-frame knobs before
        // the renderer is created, so the first frame already renders with the authored values.
        const LevelRenderSettings& render = level->Get()->GetRender();
        m_SceneSettings.Bloom = render.Bloom;
        m_SceneSettings.Shadows = render.Shadows;
        m_SceneSettings.AO = render.AO;
        m_Exposure = render.Exposure;
        m_BloomIntensity = render.BloomIntensity;

        m_SceneRenderer = Renderer::SceneRenderer::Create({
            .Context = context,
            .Assets = GetAssetManager(),
            .OutputFormat = context.GetOutputFormat(),
            .Extent = sceneExtent,
            .Settings = m_SceneSettings,
        });

        if (GetImGuiLayer())
        {
            // Translucent debug windows so the lit scene shows through behind the
            // overlay. The window chrome (WindowBg, title bars, popups, frames) reads
            // its fill from the Background/Surface roles, so scaling their alpha is the
            // whole effect; accent and text roles stay opaque for legibility.
            UI::Theme theme = UI::BuiltInDarkTheme();
            const auto translucent = [](vec4& role, f32 alpha) { role.a *= alpha; };
            translucent(theme.Background, 0.78f);
            translucent(theme.Surface, 0.78f);
            translucent(theme.SurfaceRaised, 0.78f);
            translucent(theme.SurfaceHovered, 0.78f);
            translucent(theme.SurfaceActive, 0.78f);
            UI::SetTheme(theme);
            GetImGuiLayer()->ApplyTheme();

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

        // Starting the game: the level spawns its world into a fresh scene, builds the
        // simulation from its ordered system set, and seeds the Session entity from the
        // game-mode config. The app owns and drives the returned bundle.
        LevelInstance instance = level->Get()->LoadInto(GetAssetManager(), GetSystemRegistry());
        m_Scene = std::move(instance.World);
        m_Simulation = std::move(instance.Simulation);

        // Smoke renders a fixed pose, so block until the streamed primitives are resident
        // before the capture frame; the windowed app lets them appear over a few frames.
        if (m_SmokeOutput)
        {
            WaitForPrimitiveResidency();
        }

        if (GetImGuiLayer())
        {
            m_CompositeGraph = BuildCompositeGraph();
        }

        // Start the simulation, so the Sim-phase SpawnPlayerRule instantiates the player at
        // OnStart in headless smoke too. Smoke still never ticks Update, so nothing moves after
        // the deterministic spawn and the View-phase camera rig does not run — the capture is
        // the spawned camera's authored pose.
        m_Simulation->Start(*m_Scene,
                            SystemContext{.Assets = GetAssetManager(), .Input = GetInput()});
    }

    void OnUpdate(const f32 delta) override
    {
        m_LastDelta = delta;

        // Smoke mode pins a fixed pose for golden comparison and never ticks the simulation,
        // so the capture is byte-identical run to run; the windowed app advances the spinners
        // through the registered systems.
        if (m_SmokeOutput)
        {
            m_Scene->Each<Transform, Spinner>(
                [](Entity, Transform& transform, Spinner& spinner)
                { transform.Rotation = glm::angleAxis(SmokeAngle, glm::normalize(spinner.Axis)); });
        }
        else if (!m_PauseSpin && m_Simulation)
        {
            // Skipping the tick stops every Transform write, so the broadphase reads `static`.
            m_Simulation->Update(*m_Scene, delta,
                                 SystemContext{.Assets = GetAssetManager(), .Input = GetInput()});
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

        // Aspect comes from the scene-render target the camera projects into, not the
        // swapchain; the camera lives in the scene and is resolved through its Viewer seat.
        const Ref<Renderer::ImageView> output = m_SceneRenderer->GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());
        const CameraView camera =
            ResolvePrimaryCameraView(*m_Scene, aspect).value_or(DefaultCameraView(aspect));

        // Per-frame tonemap/bloom knobs read straight off the app-side members the
        // "Scene" window edits; no Configure on these paths.
        const Renderer::SceneView view{
            .World = *m_Scene,
            .Camera = camera,
            .Delta = m_LastDelta,
            .Exposure = m_Exposure,
            .BloomThreshold = m_BloomThreshold,
            .BloomIntensity = m_BloomIntensity,
            .BloomRadius = m_BloomRadius,
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
        m_Simulation.reset();
        m_SceneRenderer.reset();
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_SceneTexture.reset();
        m_SceneSampler.reset();
        m_Scene.reset();
    }

private:
    // The fallback view when the scene resolves no camera: the fixed pose that frames the
    // 10x10 grid from above, pulled back and elevated.
    static CameraView DefaultCameraView(const f32 aspect)
    {
        CameraView view;
        view.SetPerspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
        view.SetView(vec3(0.0f, 10.0f, 14.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return view;
    }

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
            static constexpr std::array<string_view, 13> modeNames{
                "Final",         "Albedo", "Normal",  "Depth",    "Roughness",        "Metallic",
                "Occlusion",     "AO",     "Shadows", "Cascades", "Punctual shadows", "Bloom",
                "Motion vectors"};
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

            // TAA is a topology change: it inserts the resolve/history passes and jitters
            // the projection. Visible as the orbiting view converges to a crisp image.
            if (UI::Checkbox("TAA", m_SceneSettings.TAA))
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

            // Tonemap exposure is a per-frame SceneView value; the drag edits the member.
            (void)UI::Drag("Exposure", m_Exposure, {.Speed = 0.01f, .Min = 0.0f, .Max = 16.0f});

            // Bloom on/off and the kernel are topology; threshold/intensity/radius are
            // per-frame SceneView members. The per-bloom knobs grey out when bloom is off.
            if (UI::Checkbox("Bloom", m_SceneSettings.Bloom))
            {
                ReconfigureScene();
            }
            {
                auto bloomDisabled = UI::Disabled(!m_SceneSettings.Bloom);
                (void)UI::Drag("Bloom threshold", m_BloomThreshold,
                               {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
                (void)UI::Drag("Bloom intensity", m_BloomIntensity,
                               {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
                (void)UI::Drag("Bloom radius", m_BloomRadius,
                               {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});

                static constexpr std::array<string_view, 2> kernelNames{"COD (13-tap/tent)",
                                                                        "Dual Kawase"};
                i32 kernel = static_cast<i32>(m_SceneSettings.Kernel);
                if (UI::Combo("Bloom kernel", kernel, kernelNames))
                {
                    m_SceneSettings.Kernel = static_cast<Renderer::BloomKernel>(kernel);
                    ReconfigureScene();
                }
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

    Unique<Scene> m_Scene;

    // Drives the registered scene systems. Started in both paths so the spawn rule runs;
    // only the windowed path ticks Update.
    Unique<SceneSimulation> m_Simulation;

    f32 m_LastDelta = 0.0f;
    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;

    // Per-frame tonemap/bloom knobs the "Scene" window edits, fed into each frame's SceneView.
    // Lifted from 1.0 so the weak directional + orbiting point lights read on the grid.
    f32 m_Exposure = 2.5f;
    f32 m_BloomThreshold = 0.5f;
    f32 m_BloomIntensity = 1.5f;
    f32 m_BloomRadius = 1.0f;

    // Skips the per-frame Transform write so the broadphase reads `static`; never set in smoke mode.
    bool m_PauseSpin = false;
};

// Factory captures the headless flag so the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Spinner>();
    host->Systems.Register<SpinnerSystem>();

    // The game-mode rule runs first: it spawns the configured player prefab at the
    // session's start, so the pawn and seat exist before the control pipeline ticks.
    host->Systems.Register<SpawnPlayerRule>();

    // The control pipeline runs in order: the game-specific control mapping produces
    // Intent, then the engine's generic movement system consumes it. Registration order
    // is run order, so ControlSystem must precede MovementSystem. All three are Sim-phase,
    // so they finish before the View-phase camera rig trails the moved pawn.
    host->Systems.Register<ControlSystem>();
    host->Systems.Register<MovementSystem>();
    host->Systems.Register<CameraRigSystem>();

    // Smoke mode: no window or swapchain, render off-screen and dump — the display-free CI path.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    host->App.RegisterApplication(
        [smoke](TypeRegistry& types, SystemRegistry& systems)
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
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()

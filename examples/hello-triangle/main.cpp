#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Level.h>
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

#include <algorithm>
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

VE_REFLECT(::Spinner, 0xAEF00D5EFC2444DAULL)
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
    // Only the yaw drives the pawn; the pitch tilts the follow camera, not the body. The
    // Mover's TurnSpeed scales the yaw.
    intent.Look = vec2(input.Look.x, 0.0f);
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

        // Control runs only while the cursor is captured (gameplay focus). Released for the
        // debug UI, the snapshot still mirrors input, so gating on capture leaves the pawn
        // and follow camera still while ImGui owns the mouse — all axes read zero.
        const bool active = input.IsMouseCaptured();

        // WASD strafes/advances in the pawn's local frame; Space requests a jump action.
        // The pawn faces its local -Z (the camera trails behind looking that way), so W
        // advances toward -Z and S retreats toward +Z.
        vec3 move{0.0f};
        u32 buttons = 0;
        if (active)
        {
            if (input.IsKeyDown(Key::W))
            {
                move.z -= 1.0f;
            }
            if (input.IsKeyDown(Key::S))
            {
                move.z += 1.0f;
            }
            if (input.IsKeyDown(Key::D))
            {
                move.x += 1.0f;
            }
            if (input.IsKeyDown(Key::A))
            {
                move.x -= 1.0f;
            }

            if (input.IsKeyDown(Key::Space))
            {
                buttons |= static_cast<u32>(PlayerButton::Jump);
            }
        }

        // GetMouseDelta is raw pixels. Mouse X yaws the pawn, negated so moving the mouse
        // right turns the view right (the engine integrates Look.x * TurnSpeed * delta about
        // world up). Mouse Y tilts the follow camera; it accumulates as a direct, clamped
        // angle below, so it uses its own small per-pixel scale rather than the yaw rate.
        constexpr f32 YawSensitivity = 0.05f;
        constexpr f32 PitchSensitivity = 0.005f;
        const vec2 mouse = active ? input.GetMouseDelta() : vec2(0.0f);
        const vec2 look = {-mouse.x * YawSensitivity, 0.0f};
        const f32 cameraPitchDelta = -mouse.y * PitchSensitivity;

        scene.Each<PlayerInput, Possesses>(
            [&](const Entity seat, PlayerInput& player, Possesses& possesses)
            {
                player.Move = move;
                player.Look = look;
                player.Buttons = buttons;

                // Mouse Y pitches the seat's follow camera around the pawn, clamped so it
                // never orbits over the top or under the floor — the body stays upright.
                if (const Viewer* viewer = scene.TryGet<Viewer>(seat);
                    viewer != nullptr && viewer->Camera != Entity::Null &&
                    scene.IsAlive(viewer->Camera) && scene.Has<CameraFollow>(viewer->Camera))
                {
                    constexpr f32 PitchLimit = 1.2f;
                    auto& follow = scene.Get<CameraFollow>(viewer->Camera);
                    follow.Pitch =
                        std::clamp(follow.Pitch + cameraPitchDelta, -PitchLimit, PitchLimit);
                }

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

        // The level's render subset seeds the renderer settings and the per-frame knobs, applied
        // to the engine-owned managed viewport so the first frame renders with the authored values.
        const LevelRenderSettings& render = level->Get()->GetRender();
        m_SceneSettings.Bloom = render.Bloom;
        m_SceneSettings.Shadows = render.Shadows;
        m_SceneSettings.AO = render.AO;
        m_Exposure = render.Exposure;
        m_BloomIntensity = render.BloomIntensity;

        // The engine owns the primary viewport (its SceneRenderer, output, and the gather +
        // composite tail); the app pushes only a ViewState. Apply the level's topology settings.
        GetPrimaryViewport()->Configure(m_SceneSettings);

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
            // viewport output.
            m_SceneSampler = Renderer::Sampler::Create(
                GetRenderContext(), {
                                        .Name = "Scene Composite Sampler",
                                        .AddressModeU = Renderer::AddressMode::ClampToEdge,
                                        .AddressModeV = Renderer::AddressMode::ClampToEdge,
                                        .AddressModeW = Renderer::AddressMode::ClampToEdge,
                                    });
            m_SceneTexture =
                GetImGuiLayer()->CreateTexture(*m_SceneSampler, *GetPrimaryViewport()->GetOutput());
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

        // Start the simulation, so the Sim-phase SpawnPlayerRule instantiates the player at
        // OnStart in headless smoke too. Smoke still never ticks Update, so nothing moves after
        // the deterministic spawn and the View-phase camera rig does not run — the capture is
        // the spawned camera's authored pose.
        m_Simulation->Start(*m_Scene,
                            SystemContext{.Assets = GetAssetManager(), .Input = GetInput()});

        // The shipped game owns input: capture the mouse in the window so the player's
        // mouse-look runs against a hidden, locked cursor (Escape frees it for the debug UI;
        // a click on the scene re-captures it). Smoke is headless with no window, so it skips.
        if (!m_SmokeOutput)
        {
            GetInputRouter().PushFocus(InputFocus::Gameplay);
        }
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
        else
        {
            // Escape frees the mouse to drive the debug UI; a left click on the scene (outside
            // any ImGui window) re-captures it and resumes mouse-look. ImGui's NewFrame already
            // ran this frame, so WantCaptureMouse reflects this frame's cursor.
            if (GetInputRouter().IsGameplayFocused())
            {
                if (GetInput().WasKeyPressed(Key::Escape))
                {
                    GetInputRouter().PopFocus();
                }
            }
            else if (GetInput().WasMouseButtonPressed(MouseButton::Left) && !UI::WantCaptureMouse())
            {
                GetInputRouter().PushFocus(InputFocus::Gameplay);
            }

            if (!m_PauseSpin && m_Simulation)
            {
                // Skipping the tick stops every Transform write, so the broadphase reads `static`.
                m_Simulation->Update(
                    *m_Scene, delta,
                    SystemContext{.Assets = GetAssetManager(), .Input = GetInput()});
            }
        }

        // Push this frame's render source into the engine-owned managed viewport: the resolved
        // camera plus the per-frame tonemap/bloom knobs the "Scene" window edits. The engine
        // renders the viewport in its drive-list phase, before OnRender builds the UI.
        const Ref<Renderer::ImageView> output = GetPrimaryViewport()->GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());
        const CameraView camera =
            ResolvePrimaryCameraView(*m_Scene, aspect).value_or(DefaultCameraView(aspect));

        GetPrimaryViewport()->SetViewState({
            .World = m_Scene.get(),
            .Camera = camera,
            .Delta = m_LastDelta,
            .Exposure = m_Exposure,
            .BloomThreshold = m_BloomThreshold,
            .BloomIntensity = m_BloomIntensity,
            .BloomRadius = m_BloomRadius,
        });

        // Runs before this frame's commands record, so the image holds the previous frame's contents.
        if (m_SmokeOutput && ++m_FrameCount == 20)
        {
            WriteSceneCapture(m_SmokeOutput);
            RequestExit();
        }
    }

    void OnRender() override
    {
        if (GetImGuiLayer())
        {
            RenderUserInterface();
        }
    }

    void OnDispose() override
    {
        m_Simulation.reset();
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

    // Configure can recreate the viewport's output image, so the ImGui texture must be re-fetched
    // after each call. The engine's gather reads the viewport output fresh per frame as its
    // placement, so it picks up the new view with no re-pointing here.
    void ReconfigureScene()
    {
        GetPrimaryViewport()->Configure(m_SceneSettings);
        m_SceneTexture =
            GetImGuiLayer()->CreateTexture(*m_SceneSampler, *GetPrimaryViewport()->GetOutput());
    }

    void RenderUserInterface()
    {
        const Renderer::SceneRenderer& renderer = GetPrimaryViewport()->GetRenderer();

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
                          .Max = static_cast<f32>(renderer.GetMaxShadowResolution())}))
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
            if (UI::Drag("Punctual shadow resolution", punctualResolution,
                         {.Speed = 16.0f,
                          .Min = 256.0f,
                          .Max = static_cast<f32>(renderer.GetMaxPunctualShadowResolution())}))
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
            const Ref<Renderer::ImageView> output = renderer.GetOutput();
            const f32 aspect = static_cast<f32>(output->GetImage()->GetHeight()) /
                               static_cast<f32>(output->GetImage()->GetWidth());
            UI::Image(m_SceneTexture, {available.x, available.x * aspect});
        }

        if (auto statsWindow = UI::Window("Stats"))
        {
            UI::Text(
                fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()));

            // The cull funnel: gathered submesh candidates → frustum survivors → draws issued.
            const u32 gathered = renderer.GetLastVisibleCount();
            const u32 frustum = renderer.GetFrustumSurvivedCount();
            const u32 drawn = renderer.GetLastDrawnCount();
            UI::Text(fmt::format("Gathered: {}", gathered));
            UI::Text(fmt::format("Frustum survived: {}", frustum));
            UI::Text(fmt::format("Drawn: {}", drawn));

            // Under the GPU path the occlusion test zeroes occluded commands' instanceCount;
            // the survivor count is read back one frame late. The active line shows the real
            // mode (GPU degrades to CPU on a device without multiDrawIndirect).
            const bool gpuActive =
                renderer.GetActiveCullMode() == Renderer::SceneRendererSettings::CullMode::GPU;
            UI::Text(fmt::format("Cull mode: {}", gpuActive ? "GPU" : "CPU"));
            if (gpuActive)
            {
                UI::Text(fmt::format("Occlusion survived: {}", renderer.GetLastGpuSurvivorCount()));
            }

            // Flips live as the pause-spin toggle stops/resumes the per-frame Transform write.
            const bool rebuilt = renderer.DidBroadphaseRebuildLastFrame();
            UI::Text(fmt::format("Broadphase: {} ({} nodes)", rebuilt ? "rebuilt" : "static",
                                 renderer.GetBroadphaseNodeCount()));

            (void)UI::Checkbox("Pause spin", m_PauseSpin);
        }
    }

    void WriteSceneCapture(const char* outPath) const
    {
        const Ref<Renderer::Image> output = GetPrimaryViewport()->GetOutput()->GetImage();
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

    // Topology/sizing knobs applied to the managed viewport through Configure; the per-frame
    // tonemap/bloom values ride the ViewState instead.
    Renderer::SceneRendererSettings m_SceneSettings;

    // Recreated when Configure invalidates the viewport's output image.
    Ref<Renderer::Sampler> m_SceneSampler;
    Ref<ImGuiTexture> m_SceneTexture;

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
                    .HeadlessExtent = {1280, 720},
                    .WindowInfo =
                        {
                            .Extent = {1280, 720},
                            .Resizable = false,
                            .Title = "veng — Hello Triangle",
                            .CaptureMouse = false,
                        },
                    .Headless = smoke,
                    // Persist the pipeline cache beside the launcher, the same
                    // executable-relative resolution the asset pack uses.
                    .PipelineCachePath = ExecutableDirectory() / "pipeline_cache.bin",
                    // The engine owns the primary viewport (its SceneRenderer + the gather +
                    // composite tail); the app pushes only a ViewState. Topology is applied
                    // through Configure once the level's render subset is loaded.
                    .ManagedViewport = ManagedViewportInfo{},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()

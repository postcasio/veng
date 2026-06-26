#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
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
#include <Veng/UI/DebugPanels.h>

#include <Veng/Input.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/AnimationSystem.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/RootMotion.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneViewport.h>
#include <Veng/Task/TaskSystem.h>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

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

        // The player prefab uses a cooked, already-resident mesh, so nothing waits on the
        // spawn's batch here; a primitive player would carry a pending batch this rule could
        // surface. Each spawn owns its own batch — the level's does not cover sim-spawned content.
        m_Spawned = config.PlayerPrefab.Get()->SpawnInto(scene, context.Assets).Roots;
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
            // Show the title-bar collapse arrow so the debug windows fold away with a click.
            theme.ShowWindowCollapseButton = true;
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
    }

    // The engine has mounted the pack, loaded the startup level, spawned the world, and seeded the
    // managed view from the level's render settings; the sample seeds its own editable topology
    // copy here, adds its extras, and (smoke) waits on residency before the deterministic capture.
    void OnWorldLoaded(Scene&, ResidencyBatch& pending) override
    {
        // Seed the editable topology copy from the same level render settings the engine applied
        // to the per-frame view, so the debug RenderSettingsEditor starts in sync. The HDRI
        // environment, exposure, and bloom intensity already rode the engine's view push.
        const LevelRenderSettings& render = GetWorldLevel().Get()->GetRender();
        ApplyLevelRenderSettings(render, m_SceneSettings, GetWorldViewState());

        // SSR is off by default in the engine; the sample opts in to show reflections off the
        // gradient-roughness ground plane (at the engine-default half SSR resolution).
        m_SceneSettings.SSR = true;

        // BloomThreshold is not a level field; the sample lifts the knee so the weak lights bloom.
        GetWorldViewState().BloomThreshold = 0.5f;

        // HT_DEBUG_VIEW pins a debug visualization mode by its DebugView enum index (the headless
        // capture has no combo): it overrides the level's Final mode so a g-buffer/battery target
        // can be captured and inspected.
        if (const char* dv = std::getenv("HT_DEBUG_VIEW"))
        {
            m_SceneSettings.Mode = static_cast<Renderer::DebugView>(std::atoi(dv));
        }

        // Apply the sample's topology to the managed viewport; the engine already configured it
        // from the level, so this layers the sample's extras on. Recreates the scene texture.
        ReconfigureScene();

        // HT_RENDER_SCALE pins a fixed render scale (the headless capture has no slider): it drives
        // the dynamic-resolution sub-rect so a reduced-resolution render can be captured and diffed.
        if (const char* scaleEnv = std::getenv("HT_RENDER_SCALE"))
        {
            GetPrimaryViewport()->SetRenderScale(std::strtof(scaleEnv, nullptr));
        }

        if (m_SmokeOutput)
        {
            // Smoke renders a fixed pose: pause the simulation so the spinners hold the pinned
            // SmokeAngle (set in OnUpdate) and the View-phase camera rig does not trail, and block
            // until the world spawn's streamed meshes are resident before the capture frame.
            SetWorldPaused(true);
            pending.WaitResident(GetTaskSystem());
        }
        else
        {
            // The shipped game owns input: capture the mouse in the window so the player's
            // mouse-look runs against a hidden, locked cursor (Escape frees it for the debug UI;
            // a click on the scene re-captures it).
            GetInputRouter().PushFocus(InputFocus::Gameplay);
        }
    }

    void OnUpdate(const f32) override
    {
        // The engine ticks the managed world's simulation (control, movement, spinners) and pushes
        // the resolved camera into the viewport each frame; the sample handles only smoke capture
        // and gameplay input focus here.
        if (m_SmokeOutput)
        {
            // Smoke pins a fixed pose for golden comparison: the world is paused (no tick), so the
            // sample writes the deterministic SmokeAngle each frame and the engine pushes the
            // authored camera — byte-identical run to run.
            GetWorld()->Each<Transform, Spinner>(
                [](Entity, Transform& transform, Spinner& spinner)
                { transform.Rotation = glm::angleAxis(SmokeAngle, glm::normalize(spinner.Axis)); });

            // Runs before this frame's commands record, so the image holds the previous frame.
            if (++m_FrameCount == 20)
            {
                WriteSceneCapture(m_SmokeOutput);
                RequestExit();
            }
            return;
        }

        // Escape frees the mouse to drive the debug UI; a left click on the scene (outside any
        // ImGui window) re-captures it and resumes mouse-look. ImGui's NewFrame already ran this
        // frame, so WantCaptureMouse reflects this frame's cursor.
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
        m_SceneTexture.reset();
        m_SceneSampler.reset();
    }

private:
    // Configure can recreate the viewport's output image, so the ImGui texture must be re-fetched
    // after each call. The engine's gather reads the viewport output fresh per frame as its
    // placement, so it picks up the new view with no re-pointing here. Headless (smoke) has no
    // ImGui layer, so only the topology applies — there is no scene texture to refresh.
    void ReconfigureScene()
    {
        GetPrimaryViewport()->Configure(m_SceneSettings);
        if (GetImGuiLayer())
        {
            m_SceneTexture =
                GetImGuiLayer()->CreateTexture(*m_SceneSampler, *GetPrimaryViewport()->GetOutput());
            m_SceneTextureGeneration = GetPrimaryViewport()->GetOutputGeneration();
        }
    }

    void RenderUserInterface()
    {
        Renderer::Viewport& viewport = *GetPrimaryViewport();

        // The output is replaced whenever the render scale changes the render extent (the manual
        // override, an adaptive-resolution adjustment) or Configure recreates it; the generation
        // bump tells us to re-point the ImGui texture (the gather reads the output fresh anyway).
        if (viewport.GetOutputGeneration() != m_SceneTextureGeneration)
        {
            m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *viewport.GetOutput());
            m_SceneTextureGeneration = viewport.GetOutputGeneration();
        }

        // The renderer toggles, sliders, and per-frame view knobs are engine UI; a true return
        // means a topology field changed, so the sample owns the Configure (the engine helper
        // reports the edit but never reconfigures). The per-frame view knobs are the engine's
        // managed-world ViewState, edited in place and pushed by the engine each frame.
        if (auto settingsWindow = UI::Window("Render Settings"))
        {
            if (UI::RenderSettingsEditor(m_SceneSettings, GetWorldViewState(), viewport))
            {
                ReconfigureScene();
            }
        }

        // The renderer's read-only stats, plus the sample's own pause-spin control beneath them.
        if (auto statsWindow = UI::Window("Stats"))
        {
            UI::RendererStatsPanel(viewport);

            // Pauses the managed world's simulation, flipping the broadphase read-out between
            // rebuilt/static by stopping every per-tick Transform write — a game-specific control.
            if (UI::Checkbox("Pause spin", m_PauseSpin))
            {
                SetWorldPaused(m_PauseSpin);
            }
        }

        // The GPU frame-time history graph; the stateful helper samples the device timer itself.
        if (auto graphWindow = UI::Window("Frame Time"))
        {
            m_FrameTimeGraph.Draw(viewport);
        }

        // The scene's composited output, drawn last so it fills its own window.
        if (auto sceneWindow = UI::Window("Scene"))
        {
            const vec2 available = UI::ContentRegionAvail();
            const Ref<Renderer::ImageView> output = viewport.GetRenderer().GetOutput();
            const f32 aspect = static_cast<f32>(output->GetImage()->GetHeight()) /
                               static_cast<f32>(output->GetImage()->GetWidth());
            UI::Image(m_SceneTexture, {available.x, available.x * aspect});
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

    // The GPU frame-time history graph, owning its sample ring across frames.
    UI::FrameTimeGraph m_FrameTimeGraph;

    // Topology/sizing knobs applied to the managed viewport through Configure; seeded from the
    // level (plus the sample's SSR/debug-view extras) in OnWorldLoaded. The per-frame tonemap/bloom
    // values ride the engine's managed-world ViewState (GetWorldViewState) instead.
    Renderer::SceneRendererSettings m_SceneSettings;

    // Recreated when Configure invalidates the viewport's output image.
    Ref<Renderer::Sampler> m_SceneSampler;
    Ref<ImGuiTexture> m_SceneTexture;

    // Output generation the ImGui scene texture was last built from; a mismatch (a render-scale
    // change, an adaptive-resolution resize, or Configure replacing the output) drives a re-fetch.
    u64 m_SceneTextureGeneration = 0;

    // Fixed rotation for the smoke capture, in radians.
    static constexpr f32 SmokeAngle = 0.9f;

    u32 m_FrameCount = 0;
    const char* m_SmokeOutput = nullptr;

    // Pauses the managed world's simulation so the broadphase reads `static`; never set in smoke.
    bool m_PauseSpin = false;
};

// Factory captures the headless flag so the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The game registers only its own component and systems; the engine's reusable systems
    // (MovementSystem, CameraRigSystem, RootMotionDriveSystem, AnimationSystem) are pre-registered
    // by the host. The level's ordered SystemId set names the run order across both.
    host->Types.Register<Spinner>();
    host->Systems.Register<SpinnerSystem>();

    // The game-mode rule spawns the configured player prefab at the session's start, so the pawn
    // and seat exist before the control pipeline ticks.
    host->Systems.Register<SpawnPlayerRule>();

    // The game-specific control mapping: reads input into a PlayerInput snapshot and produces the
    // Intent the engine's MovementSystem consumes.
    host->Systems.Register<ControlSystem>();

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
                    .ManagedViewport =
                        ManagedViewportInfo{
                            // Render at the full backing extent — native resolution on a HiDPI
                            // display, not supersampling. The allocation-tier outer loop reclaims
                            // footprint under sustained load; a lower ceiling is the knob for a fixed
                            // perf budget, not the default posture.
                            .MaxAllocationScale = 1.0f,
                            // The windowed app opts into both adaptive-resolution loops: the inner
                            // loop drives the per-frame sub-rect, the outer-loop tier controller
                            // follows the sustained sub-rect and sizes the allocation down a tier
                            // under durable load. The smoke capture leaves them off so the golden
                            // renders at the fixed baseline (the controller is inert headless
                            // anyway — no GPU timing means the sub-rect holds at the ceiling).
                            .DynamicResolution =
                                smoke
                                    ? std::nullopt
                                    : optional<
                                          Renderer::
                                              DynamicResolutionSettings>{Renderer::
                                                                             DynamicResolutionSettings{}},
                            .AllocationTier =
                                smoke
                                    ? std::nullopt
                                    : optional<
                                          Renderer::
                                              AllocationTierSettings>{Renderer::
                                                                          AllocationTierSettings{}},
                        },
                    // The engine bootstraps the world: it mounts the pack, loads its cooked startup
                    // level, owns the running scene + simulation, and ticks + pushes the view each
                    // frame. The sample customizes the loaded world in OnWorldLoaded.
                    .World = GameWorldInfo{.AssetPack = "sample.vengpack"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()

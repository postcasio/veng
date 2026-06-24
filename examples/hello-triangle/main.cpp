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
#include <Veng/Scene/AnimationSystem.h>
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
#include <limits>
#include <span>
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

        // HT_RENDER_SCALE pins a fixed render scale (the headless capture has no slider): it drives
        // the dynamic-resolution sub-rect so a reduced-resolution render can be captured and diffed.
        if (const char* scaleEnv = std::getenv("HT_RENDER_SCALE"))
        {
            m_RenderScale = std::strtof(scaleEnv, nullptr);
            GetPrimaryViewport()->SetRenderScale(m_RenderScale);
        }

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

        // Sample both timelines once per frame for the frame-time graph: the CPU delta the
        // engine just measured, and the GPU frame time read back from the device timers (a
        // frame or two late, which is fine for a rolling history). Smoke renders 20 frames and
        // never opens the UI, so the buffers simply go unread there.
        m_CpuFrameTimes.Push(delta * 1000.0f);
        if (GetRenderContext().IsGpuTimingSupported())
        {
            m_GpuFrameTimes.Push(GetRenderContext().GetLastGpuFrameTimeMs());

            // Per-pass GPU breakdown, grouped (bloom/hi-Z mip sweeps collapsed) and keyed by
            // group name so each pass keeps its own rolling history across frames. A pass absent
            // this frame (a topology change) stops receiving samples until it returns.
            for (const PassCost& pass : AggregatePasses(GetRenderContext().GetLastGpuPassTimings()))
            {
                m_PassFrameTimes[pass.Name].Push(pass.Milliseconds);
            }
        }

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
    // A fixed-capacity ring buffer of millisecond samples for the frame-time graph. New
    // samples overwrite the oldest once full; Head is the next write slot (and, once full,
    // the oldest sample). Count saturates at Capacity.
    struct FrameTimeHistory
    {
        static constexpr usize Capacity = 240;

        std::array<f32, Capacity> Samples{};
        usize Head = 0;
        usize Count = 0;

        void Push(const f32 milliseconds)
        {
            Samples[Head] = milliseconds;
            Head = (Head + 1) % Capacity;
            Count = std::min(Count + 1, Capacity);
        }

        // The most recently pushed sample, or 0 before the first push.
        f32 Last() const { return Count == 0 ? 0.0f : Samples[(Head + Capacity - 1) % Capacity]; }
    };

    // Summary of a history window for the readout line above each plot.
    struct FrameStats
    {
        f32 Min = 0.0f;
        f32 Max = 0.0f;
        f32 Average = 0.0f;
    };

    // Reduces the valid samples of a history to min / max / average. The valid range is the
    // first Count slots: the buffer fills from index 0 before it wraps, so [0, Count) is
    // always the populated set regardless of Head.
    static FrameStats ComputeStats(const FrameTimeHistory& history)
    {
        if (history.Count == 0)
        {
            return {};
        }

        FrameStats stats{.Min = std::numeric_limits<f32>::max(), .Max = 0.0f, .Average = 0.0f};
        f32 sum = 0.0f;
        for (usize i = 0; i < history.Count; i++)
        {
            const f32 sample = history.Samples[i];
            stats.Min = std::min(stats.Min, sample);
            stats.Max = std::max(stats.Max, sample);
            sum += sample;
        }
        stats.Average = sum / static_cast<f32>(history.Count);
        return stats;
    }

    // One pass's GPU time after grouping: the bloom and hi-Z mip sweeps each collapse to a
    // single named entry, every other pass stays itself.
    struct PassCost
    {
        string Name;
        f32 Milliseconds = 0.0f;
    };

    // Folds a per-scope pass name onto its display group: the per-mip bloom and hi-Z passes
    // ("Bloom Down Mip 3", "HiZ Reduce Mip 2", …) collapse to "Bloom" / "Hi-Z" so the sweep
    // reads as one pass; any other name passes through unchanged.
    static string PassGroup(string_view name)
    {
        if (name.starts_with("Bloom"))
        {
            return "Bloom";
        }
        if (name.starts_with("HiZ") || name.starts_with("Hi-Z"))
        {
            return "Hi-Z";
        }
        return string(name);
    }

    // Aggregates the frame's per-scope timings into grouped pass costs, summing each group's
    // contiguous mip passes and preserving first-seen (execution) order.
    static vector<PassCost>
    AggregatePasses(std::span<const Renderer::Context::GpuPassTiming> passes)
    {
        vector<PassCost> costs;
        for (const Renderer::Context::GpuPassTiming& pass : passes)
        {
            string group = PassGroup(pass.Name);
            const auto existing = std::ranges::find(costs, group, &PassCost::Name);
            if (existing == costs.end())
            {
                costs.push_back({.Name = std::move(group), .Milliseconds = pass.Milliseconds});
            }
            else
            {
                existing->Milliseconds += pass.Milliseconds;
            }
        }
        return costs;
    }

    // A stable, legible color for a pass's line and legend swatch: a fixed palette indexed by
    // a name hash, so a pass keeps its color regardless of its position in the frame's order.
    static vec4 PassColor(string_view name)
    {
        static const std::array<vec4, 12> Palette{
            vec4{0.90f, 0.30f, 0.30f, 1.0f}, vec4{0.95f, 0.60f, 0.25f, 1.0f},
            vec4{0.90f, 0.85f, 0.30f, 1.0f}, vec4{0.55f, 0.85f, 0.30f, 1.0f},
            vec4{0.30f, 0.80f, 0.40f, 1.0f}, vec4{0.25f, 0.80f, 0.75f, 1.0f},
            vec4{0.30f, 0.75f, 0.95f, 1.0f}, vec4{0.35f, 0.55f, 0.95f, 1.0f},
            vec4{0.55f, 0.45f, 0.95f, 1.0f}, vec4{0.70f, 0.45f, 0.90f, 1.0f},
            vec4{0.90f, 0.40f, 0.80f, 1.0f}, vec4{0.95f, 0.45f, 0.60f, 1.0f},
        };

        // FNV-1a over the name.
        u32 hash = 2166136261u;
        for (const char c : name)
        {
            hash = (hash ^ static_cast<u8>(c)) * 16777619u;
        }
        return Palette[hash % Palette.size()];
    }

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
        m_SceneTextureGeneration = GetPrimaryViewport()->GetOutputGeneration();
    }

    // Enables or disables the viewport's adaptive resolution controller from the demo state.
    void ApplyDynamicResolution()
    {
        if (m_DynamicResolution)
        {
            GetPrimaryViewport()->SetDynamicResolution({
                .TargetFrameTimeMs = 1000.0f / m_TargetFps,
                .MinScale = m_DrsMinScale,
                .MaxScale = m_DrsMaxScale,
            });
        }
        else
        {
            GetPrimaryViewport()->ClearDynamicResolution();
        }
    }

    void RenderUserInterface()
    {
        const Renderer::Viewport& viewport = *GetPrimaryViewport();

        // The output is replaced whenever the render scale changes the render extent (the manual
        // slider, an adaptive-resolution adjustment) or Configure recreates it; the generation
        // bump tells us to re-point the ImGui texture (the gather reads the output fresh anyway).
        if (viewport.GetOutputGeneration() != m_SceneTextureGeneration)
        {
            m_SceneTexture = GetImGuiLayer()->CreateTexture(*m_SceneSampler, *viewport.GetOutput());
            m_SceneTextureGeneration = viewport.GetOutputGeneration();
        }

        // Mirror the live scale so the (possibly greyed) slider tracks automatic adjustments.
        m_RenderScale = viewport.GetRenderScale();

        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();

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

            // A scale drag field stepping by 0.05: the drag speed is the step, and the value snaps
            // to a 0.05 multiple (clamped) so it lands on clean increments.
            const auto scaleDrag = [](const char* label, f32& value, f32 min, f32 max)
            {
                if (UI::Drag(label, value,
                             {.Speed = 0.05f, .Min = min, .Max = max, .Format = "%.2f"}))
                {
                    value = glm::clamp(glm::round(value / 0.05f) * 0.05f, min, max);
                    return true;
                }
                return false;
            };

            // Adaptive resolution drives the render scale from measured GPU frame time toward a
            // target rate, kept between a min and max scale (needs device timestamp support). The
            // checkbox greys out without it.
            {
                auto timingDisabled = UI::Disabled(!GetRenderContext().IsGpuTimingSupported());
                if (UI::Checkbox("Dynamic resolution", m_DynamicResolution))
                {
                    ApplyDynamicResolution();
                }
            }
            if (m_DynamicResolution)
            {
                if (UI::Slider("Target FPS", m_TargetFps, {.Min = 30.0f, .Max = 120.0f}))
                {
                    ApplyDynamicResolution();
                }
                // The controller's scale bounds. Each keeps min <= max so the band never inverts.
                if (scaleDrag("Min scale", m_DrsMinScale, 0.25f, 1.0f))
                {
                    m_DrsMaxScale = glm::max(m_DrsMaxScale, m_DrsMinScale);
                    ApplyDynamicResolution();
                }
                if (scaleDrag("Max scale", m_DrsMaxScale, 0.25f, 2.0f))
                {
                    m_DrsMinScale = glm::min(m_DrsMinScale, m_DrsMaxScale);
                    ApplyDynamicResolution();
                }
            }

            // Render scale is a per-viewport property, not a SceneRendererSettings: it renders into
            // a sub-rect of the allocation while the on-screen region stays full size, so the
            // tonemap upscales. Below 1.0 the image visibly softens; the Stats window shows the real
            // extent. The manual field steps by 0.05 from a 0.25 floor and greys out while the
            // adaptive controller owns the scale.
            {
                auto manualDisabled = UI::Disabled(m_DynamicResolution);
                if (scaleDrag("Render scale", m_RenderScale, 0.25f, 2.0f))
                {
                    GetPrimaryViewport()->SetRenderScale(m_RenderScale);
                }
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

            // The measured GPU frame time the adaptive controller drives off (when supported).
            if (GetRenderContext().IsGpuTimingSupported())
            {
                UI::Text(fmt::format("GPU frame: {:.2f} ms",
                                     GetRenderContext().GetLastGpuFrameTimeMs()));
            }

            // The live render scale (the controller writes it while dynamic resolution is on) and
            // the render-target extent, which shrinks with the scale while the window stays full size.
            UI::Text(fmt::format("Render scale: {:.2f}{}", viewport.GetRenderScale(),
                                 m_DynamicResolution ? " (auto)" : ""));
            const Ref<Renderer::Image> target = renderer.GetOutput()->GetImage();
            UI::Text(fmt::format("Render target: {}x{}", target->GetWidth(), target->GetHeight()));

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

        RenderFrameTimeGraph();
    }

    // Plots one rolling history: a label/stat line above a fixed-axis line graph. The axis is
    // pinned to [0, max·1.25] (floored at one 60 Hz frame) so a line's height reads as absolute
    // milliseconds, and the overlay reports the current sample.
    static void PlotHistory(string_view name, const FrameTimeHistory& history, f32 height)
    {
        const FrameStats stats = ComputeStats(history);
        UI::Text(fmt::format("{}: {:.2f} ms  (avg {:.2f}  min {:.2f}  max {:.2f})", name,
                             history.Last(), stats.Average, stats.Min, stats.Max));

        // A full buffer wraps, so the oldest sample sits at the write head; until then it is
        // filled from index 0 and plots in array order.
        const i32 offset =
            history.Count == FrameTimeHistory::Capacity ? static_cast<i32>(history.Head) : 0;
        const f32 scaleMax = glm::max(stats.Max * 1.25f, 1000.0f / 60.0f);
        UI::PlotLines(fmt::format("##{}", name), {history.Samples.data(), history.Count},
                      {
                          .OverlayText = fmt::format("{:.2f} ms", history.Last()),
                          .ScaleMin = 0.0f,
                          .ScaleMax = scaleMax,
                          .Offset = offset,
                          .Size = {0.0f, height},
                      });
    }

    // A rolling graph of CPU and GPU whole-frame time plus, below them, a per-pass GPU breakdown
    // — one compact plot per render-graph pass, in execution order. The GPU sections appear only
    // when the device exposes timestamp queries; otherwise a note stands in.
    void RenderFrameTimeGraph()
    {
        if (auto graphWindow = UI::Window("Frame Time"))
        {
            PlotHistory("CPU", m_CpuFrameTimes, 80.0f);

            UI::Spacing();
            if (!GetRenderContext().IsGpuTimingSupported())
            {
                UI::TextDisabled("GPU timing unsupported on this device");
                return;
            }

            PlotHistory("GPU", m_GpuFrameTimes, 80.0f);

            // The whole per-pass breakdown in one graph: a histogram with one bar per grouped
            // pass (bloom/hi-Z mip sweeps collapsed), in execution order. The legend below maps
            // each bar, left-to-right, to its pass name, current cost, and rolling average.
            const vector<PassCost> passes =
                AggregatePasses(GetRenderContext().GetLastGpuPassTimings());
            if (passes.empty())
            {
                return;
            }

            UI::SeparatorText("Passes (GPU)");

            // Every pass's rolling history overlaid as colored lines on one shared chart, each
            // line colored by its pass so it matches the legend swatch below.
            vector<UI::PlotSeries> series;
            series.reserve(passes.size());
            for (const PassCost& pass : passes)
            {
                const FrameTimeHistory& history = m_PassFrameTimes[pass.Name];
                const i32 offset = history.Count == FrameTimeHistory::Capacity
                                       ? static_cast<i32>(history.Head)
                                       : 0;
                series.push_back({
                    .Color = PassColor(pass.Name),
                    .Values = {history.Samples.data(), history.Count},
                    .Offset = offset,
                });
            }
            UI::PlotLinesMulti("##passes", series, {.ScaleMin = 0.0f, .Size = {0.0f, 140.0f}});

            // Two-column legend: a color swatch matching each line, then the pass and its cost.
            if (auto legend = UI::Table("PassLegend", 2))
            {
                const f32 swatch = UI::GetTextLineHeight();
                for (const PassCost& pass : passes)
                {
                    UI::TableNextColumn();
                    UI::Badge("", PassColor(pass.Name), {swatch, swatch});
                    UI::SameLine();
                    UI::Text(fmt::format("{}: {:.3f} ms", pass.Name, pass.Milliseconds));
                }
            }
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

    // Per-frame millisecond histories driving the frame-time graph: the engine-measured CPU
    // delta and the device GPU frame time (populated only when timestamp queries are supported).
    FrameTimeHistory m_CpuFrameTimes;
    FrameTimeHistory m_GpuFrameTimes;

    // Per-pass GPU histories keyed by render-graph pass name, so each pass's plot persists
    // across frames while the live frame's timing list drives display order.
    map<string, FrameTimeHistory> m_PassFrameTimes;

    // Topology/sizing knobs applied to the managed viewport through Configure; the per-frame
    // tonemap/bloom values ride the ViewState instead.
    Renderer::SceneRendererSettings m_SceneSettings;

    // Recreated when Configure invalidates the viewport's output image.
    Ref<Renderer::Sampler> m_SceneSampler;
    Ref<ImGuiTexture> m_SceneTexture;

    // Render scale on the managed viewport: 1.0 renders at the window extent, lower renders below
    // it and the gather upscales. Mirrors the viewport's live scale (which the adaptive controller
    // also writes), so the slider tracks automatic adjustments while greyed out.
    f32 m_RenderScale = 1.0f;
    // Output generation the ImGui scene texture was last built from; a mismatch (a manual scale
    // change, an adaptive-resolution resize, or Configure replacing the output) drives a re-fetch.
    u64 m_SceneTextureGeneration = 0;

    // Adaptive resolution demo: when on, the viewport drives its own render scale from GPU frame
    // time toward m_TargetFps, between m_DrsMinScale and m_DrsMaxScale; the manual slider greys out.
    bool m_DynamicResolution = false;
    f32 m_TargetFps = 60.0f;
    f32 m_DrsMinScale = 0.5f;
    f32 m_DrsMaxScale = 1.0f;

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

    // Poses skinned characters each tick (View phase): samples the Animator's clip and writes
    // the entity's SkinnedPose for the renderer's skinning palette.
    host->Systems.Register<AnimationSystem>();

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

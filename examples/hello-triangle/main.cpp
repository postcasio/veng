#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Math/Random.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>
#include <Veng/UI/UI.h>
#include <Veng/UI/DebugPanels.h>

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/InputMappingSystem.h>
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
#include <array>
#include <cstdlib>
#include <fstream>
#include <span>

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
VE_FIELD(SpeedRadiansPerSec, .DisplayName = "Speed", .Tooltip = "Radians per second",
         .Display = {.Min = 0.0})
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

// The game's named input actions. Each is a minted ActionId a control system references and
// a binding context targets — the AssetId pattern for input. An action exists by being
// declared in a context, so there is no registry. These constants match the ids the cooked
// gameplay.inputmap asset declares.
namespace Actions
{
    // Movement axes: X strafes (D +1 / A -1), Y advances (W +1 / S -1).
    constexpr ActionId Move{0x74080D78CF763EC4ULL};
    // Look axes: raw mouse delta (X yaw, Y pitch).
    constexpr ActionId Look{0x6DB6F4088653942DULL};
    // Jump button.
    constexpr ActionId Jump{0xB64A2DFE34C4E523ULL};
}

// Maps a resolved PlayerInput to an abstract Intent — the game-specific control policy,
// reading actions by name. Pure: the same action state always yields the same Intent,
// whether the actions came from the device, a recording, or the wire, so it is unit-testable
// without an Input or a scene. WASD advances in the pawn's local frame — the pawn faces its
// local -Z (the follow camera trails behind looking that way), so the forward action drives
// move toward -Z. Only the yaw drives the pawn; pitch tilts the follow camera, not the body,
// and is applied in the control system. The Mover's TurnSpeed scales the yaw.
Intent MapInputToIntent(const PlayerInput& input)
{
    // Mouse X yaws the pawn, negated so moving the mouse right turns the view right (the
    // engine integrates Look.x * TurnSpeed * delta about world up).
    constexpr f32 YawSensitivity = 0.05f;
    const vec2 move = input.GetValue(Actions::Move);
    const vec2 look = input.GetValue(Actions::Look);

    Intent intent;
    intent.Move = vec3(move.x, 0.0f, -move.y);
    intent.Look = vec2(-look.x * YawSensitivity, 0.0f);
    intent.Actions = input.IsHeld(Actions::Jump) ? 1u : 0u;
    return intent;
}

// The game-specific control system: reads each seat's resolved PlayerInput (filled by the
// engine's InputMappingSystem, which must run before this) and maps it to the possessed
// pawn's Intent through MapInputToIntent. It reads no raw device state — in headless the
// resolved actions are all-None, so it produces a zero Intent and the pawn stays put, with
// no null to guard. It writes Intent through the scene accessor, never a retained reference.
class ControlSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        scene.Each<PlayerInput, Possesses>(
            [&](const Entity seat, PlayerInput& player, Possesses& possesses)
            {
                // Mouse Y pitches the seat's follow camera around the pawn, clamped so it
                // never orbits over the top or under the floor — the body stays upright.
                constexpr f32 PitchSensitivity = 0.005f;
                const f32 cameraPitchDelta = -player.GetValue(Actions::Look).y * PitchSensitivity;
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
        // The game-mode config is a scene component on the level's settings entity; find it by
        // type rather than a well-known entity (Scene::TryGetFirst).
        const Session* session = scene.TryGetFirst<Session>();
        if (session == nullptr || session->Phase != SessionPhase::Playing)
        {
            return;
        }

        const GameModeConfig* config = scene.TryGetFirst<GameModeConfig>();

        // The config's player prefab is eager-loaded as a dependency of the scene prefab,
        // so it is resident by the time the simulation starts; skip if it is not.
        if (config == nullptr || !config->PlayerPrefab.IsLoaded())
        {
            return;
        }

        // The player prefab uses a cooked, already-resident mesh, so nothing waits on the
        // spawn's batch here; a primitive player would carry a pending batch this rule could
        // surface. Each spawn owns its own batch — the level's does not cover sim-spawned content.
        m_Spawned = config->PlayerPrefab.Get()->SpawnInto(scene, context.Assets).Roots;
    }

    void OnUpdate(Scene& scene, const f32, const SystemContext&) override
    {
        // The spawn happens once at OnStart; a scoring / win-condition rule is the obvious
        // second system. Here the only per-tick rule action is tearing the player down when
        // the session ends.
        const Session* session = scene.TryGetFirst<Session>();
        if (session != nullptr && !m_Spawned.empty() && session->Phase == SessionPhase::Ended)
        {
            Despawn(scene);
        }
    }

    void OnStop(Scene& scene, const SystemContext&) override { Despawn(scene); }

private:
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

        StartMcpServerIfRequested();

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
    void OnWorldLoaded(Scene& world, ResidencyBatch& pending) override
    {
        // Seed the editable topology copy from the scene — the level's post knobs (a seeded
        // LevelRenderSettings component) — read by the same query the engine used, so the debug
        // RenderSettingsEditor starts in sync. The exposure and bloom already rode the engine's
        // view push. The sky is the scene's Sky component, resolved by the renderer itself each
        // Execute. Absent settings leave the defaults.
        if (const LevelRenderSettings* render = world.TryGetFirst<LevelRenderSettings>())
        {
            ApplyLevelRenderSettings(*render, m_SceneSettings, GetWorldViewState());
        }

        // HT_SKY_MATERIAL opts in to the authored Sky-domain material demo (off by default so the
        // smoke golden is untouched): an authored gradient sky reading a small point buffer through
        // the storage-buffer material input, written onto the scene's one Sky component.
        SetupSkyMaterialIfRequested(world);

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

        // HT_POINTFIELD opts into the point-field mode: a large culled, LOD'd field of random
        // colored points drawn around the scene. Off by default so the golden/smoke path is
        // untouched. Zoom the camera (mouse-wheel dolly in the windowed run) to watch individual
        // stars resolve up close and collapse to an aggregate density glow far out; the frustum
        // cull drops the off-screen cells.
        if (std::getenv("HT_POINTFIELD"))
        {
            BuildPointField();
            m_SceneSettings.PointField = true;
        }

        // Apply the sample's topology to the managed viewport; the engine already configured it
        // from the level, so this layers the sample's extras on. Recreates the scene texture.
        ReconfigureScene();

        // Hand the built field to the (reconfigured) renderer; the borrowed pointer takes effect
        // the next Execute. A no-op when the point-field mode is off (m_PointField stays null).
        GetPrimaryViewport()->GetRenderer().SetPointField(m_PointField.get());

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

            // HT_SPLITSCREEN launches straight into the two-seat mode (equivalent to pressing F2
            // once the world is up); the flush at the top of the first Update applies it. Off by
            // default, so the ordinary run and the smoke path stay single-seat.
            m_SplitScreenRequested = std::getenv("HT_SPLITSCREEN") != nullptr;
        }
    }

    void OnUpdate(const f32) override
    {
        // The engine ticks the managed world's simulation (control, movement, spinners) and pushes
        // the resolved camera into the viewport each frame; the sample handles only smoke capture,
        // the optional MCP pump, and gameplay input focus here.

        // Drain the MCP request queue at the render-thread-safe point: the engine has ticked the
        // world's simulation before OnUpdate, and no View/Each iteration runs until render, so a
        // world/mutation tool reads and writes the scene outside any iteration.
        if (m_McpServer)
        {
            m_McpServer->Pump();
        }

        if (m_SmokeOutput)
        {
            // Smoke pins a fixed pose for golden comparison: the world is paused (no tick), so the
            // sample writes the deterministic SmokeAngle each frame and the engine pushes the
            // authored camera — byte-identical run to run.
            GetWorld()->Each<Transform, Spinner>(
                [](Entity, Transform& transform, Spinner& spinner)
                { transform.Rotation = glm::angleAxis(SmokeAngle, glm::normalize(spinner.Axis)); });

            // Runs before this frame's commands record, so the image holds the previous frame. The
            // MCP server suppresses the 20-frame auto-exit: a client needs a real serving window, so
            // the app keeps rendering the fixed pose and the conformance harness terminates it.
            if (++m_FrameCount == 20 && !m_McpServer)
            {
                WriteSceneCapture(m_SmokeOutput);
                RequestExit();
            }
            return;
        }

        // F2 requests toggling the two-seat split-screen mode; the request is applied here, at the
        // top-of-frame safe point outside any Scene iteration, so the seat spawn/despawn is legal.
        if (GetInput().WasKeyPressed(Key::F2))
        {
            m_SplitScreenRequested = !m_SplitScreenRequested;
        }
        ApplySplitScreenRequest();

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

        // The seat's base gameplay context is authored on the player prefab; the focus gate only
        // toggles it: with gameplay focused the seat's contexts resolve WASD/mouse/Space, and
        // when ImGui owns the mouse the stack is emptied so every action resolves to None (the
        // pawn and follow camera stay still). This replaces the old raw-capture gate. The seat is
        // spawned by the game-mode rule, so this runs each frame outside any iteration.
        SyncGameplayContext(GetInputRouter().IsGameplayFocused());
    }

    // Gates the seat's authored input contexts on gameplay focus. On losing focus it saves each
    // seat's active contexts and empties the stack (all actions None); on regaining focus it
    // restores the saved contexts. The base context is authored prefab data — this only pops it
    // for the focus gate and pushes it back, never fabricates the binding scheme in code.
    // Idempotent, so it is safe to call every frame.
    void SyncGameplayContext(const bool focused)
    {
        Scene* world = GetWorld();
        if (world == nullptr)
        {
            return;
        }

        world->Each<Viewer, InputContextStack>(
            [&](const Entity seat, Viewer&, InputContextStack& stack)
            {
                if (!focused && !stack.Active.empty())
                {
                    m_SuspendedContexts[seat] = std::move(stack.Active);
                    stack.Active.clear();
                }
                else if (focused && stack.Active.empty())
                {
                    if (const auto it = m_SuspendedContexts.find(seat);
                        it != m_SuspendedContexts.end())
                    {
                        stack.Active = std::move(it->second);
                        m_SuspendedContexts.erase(it);
                    }
                }
            });
    }

    // Brings the live split-screen state in line with the requested state, at the top-of-frame
    // safe point (called from OnUpdate, outside any Scene iteration) so the seat spawn/despawn and
    // the managed-viewport reconfigure are legal structural changes. A no-op when already in sync.
    void ApplySplitScreenRequest()
    {
        if (m_SplitScreenRequested == m_SplitScreenActive)
        {
            return;
        }

        if (m_SplitScreenRequested)
        {
            EnterSplitScreen();
        }
        else
        {
            ExitSplitScreen();
        }
        m_SplitScreenActive = m_SplitScreenRequested;
    }

    // Spawns seat B (a second player prefab: Viewer + camera + pawn + Possesses + InputContextStack
    // + SeatInput) and reconfigures the managed viewport list to two quadrants — seat A on the left
    // half, seat B on the right half bound to seat B's Viewer so the engine resolves + pushes its
    // camera and associates its region with the router. Seat B is retyped pad-only (a controller
    // guest) and its pawn nudged aside so the two players are visibly distinct; seat A keeps its
    // authored keyboard/mouse SeatInput. Both drive through the unchanged action → intent → movement
    // pipeline — no new gameplay system.
    void EnterSplitScreen()
    {
        Scene* world = GetWorld();
        const GameModeConfig* config =
            world != nullptr ? world->TryGetFirst<GameModeConfig>() : nullptr;
        if (world == nullptr || config == nullptr || !config->PlayerPrefab.IsLoaded())
        {
            // Nothing to split into (no world or the player prefab is not resident); leave the
            // request satisfied by the single-seat view rather than half-applying.
            m_SplitScreenRequested = false;
            return;
        }

        m_SeatBRoots = config->PlayerPrefab.Get()->SpawnInto(*world, GetAssetManager()).Roots;

        // Retype the spawned seat to a pad-only guest and offset its pawn so the two players do not
        // overlap. The prefab's seat carries Viewer + SeatInput; its pawn carries Intent.
        Entity seatB = Entity::Null;
        world->Each<Viewer, SeatInput>(
            [&](const Entity seat, Viewer&, SeatInput& devices)
            {
                if (!devices.UsesKeyboardMouse)
                {
                    return;
                }
                // The keyboard/mouse seats are seat A (authored) and the freshly-spawned copy; the
                // copy is the one whose seat entity is in m_SeatBRoots' subtree. Match by root.
                if (std::ranges::find(m_SeatBRoots, seat) != m_SeatBRoots.end())
                {
                    seatB = seat;
                    devices.UsesKeyboardMouse = false;
                    devices.Gamepad = GamepadId(0);
                    devices.WantsGamepad = false;
                }
            });

        if (seatB != Entity::Null)
        {
            if (const Possesses* possesses = world->TryGet<Possesses>(seatB);
                possesses != nullptr && world->IsAlive(possesses->Pawn) &&
                world->Has<Transform>(possesses->Pawn))
            {
                world->Get<Transform>(possesses->Pawn).Position += vec3(3.0f, 0.0f, 0.0f);
            }
            m_SeatBViewer = seatB;
        }

        ReconfigureSplitViewports();
    }

    // Despawns seat B and reconfigures the managed list back to one full-window viewport (dropping
    // seat B's viewport, which self-clears its router association), restoring seat A full-window.
    void ExitSplitScreen()
    {
        const ManagedViewportInfo single = SplitViewportInfo(ViewportLayout{}, Entity::Null);
        ReconfigureManagedViewports(std::span{&single, 1});

        Scene* world = GetWorld();
        if (world != nullptr)
        {
            for (const Entity entity : m_SeatBRoots)
            {
                if (world->IsAlive(entity))
                {
                    world->DestroyEntity(entity);
                }
            }
        }
        m_SeatBRoots.clear();
        m_SeatBViewer = Entity::Null;
    }

    // Reconfigures the managed list to two quadrant viewports: seat A's viewport narrows to the
    // left half (unbound — the engine already pushes the primary seat's camera), seat B's is the
    // right half bound to its Viewer so the engine resolves its camera and associates the region.
    void ReconfigureSplitViewports()
    {
        const std::array quadrants{
            SplitViewportInfo(ViewportLayout{.Offset = {0.0f, 0.0f}, .Extent = {0.5f, 1.0f}},
                              Entity::Null),
            SplitViewportInfo(ViewportLayout{.Offset = {0.5f, 0.0f}, .Extent = {0.5f, 1.0f}},
                              m_SeatBViewer),
        };
        ReconfigureManagedViewports(quadrants);
    }

    // Builds a managed-viewport info at the given layout and bound seat, carrying the sample's
    // current SceneRenderer topology (SSR + debug-view extras seeded in OnWorldLoaded) and the
    // windowed adaptive-resolution opt-in, so a reconfigured viewport renders identically to the
    // primary.
    [[nodiscard]] ManagedViewportInfo SplitViewportInfo(const ViewportLayout layout,
                                                        const Entity viewer) const
    {
        return ManagedViewportInfo{
            .Settings = m_SceneSettings,
            .MaxAllocationScale = 1.0f,
            .DynamicResolution = Renderer::DynamicResolutionSettings{},
            .Layout = layout,
            .Viewer = viewer,
        };
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
        // Drop the server first: its destructor stops the listener thread and closes the socket, so
        // no in-flight tool handler can touch engine state while the rest of the app tears down.
        m_McpServer.reset();
        m_SceneTexture.reset();
        m_SceneSampler.reset();

        // The renderer only borrows the field; clear its pointer before releasing the field so a
        // final Execute never reads a freed buffer, then drop the field.
        if (const Renderer::Viewport* const viewport = GetPrimaryViewport())
        {
            viewport->GetRenderer().SetPointField(nullptr);
        }
        m_PointField.reset();

        // Release the sky point buffer's bindless slot (a no-op when the demo was off), then drop
        // the buffer and the material handle so the Material asset retires before the asset manager
        // and the device tear down. The scene's Sky component drops with the world.
        GetRenderContext().GetBindlessRegistry().Release(m_SkyPointHandle);
        m_SkyPointBuffer.reset();
        m_SkyMaterial = {};
    }

private:
    // Loads the authored Sky material and binds a small game-supplied point buffer to it when
    // HT_SKY_MATERIAL is set. This is the opt-in demo of the engine's Sky material domain + the
    // storage-buffer material input: the game creates a storage Buffer of points, uploads them,
    // registers the buffer with the bindless registry, and binds it to the material by handle via
    // SetStorageBufferHandle — the sky shader reads the points typed from the set-0 g_Buffers[]
    // array. Env-gated so the default smoke/golden path never enables the sky and stays untouched.
    void SetupSkyMaterialIfRequested(Scene& world)
    {
        if (std::getenv("HT_SKY_MATERIAL") == nullptr)
        {
            return;
        }

        // The gradient Sky material's default instance (the .vmat's defaultInstance id).
        constexpr AssetId SkyMaterialId{0x8F81DD9B40954C95ULL};
        const AssetResult<AssetHandle<MaterialInstance>> sky =
            GetAssetManager().LoadSync<MaterialInstance>(SkyMaterialId);
        if (!sky)
        {
            Veng::Log::Warn("hello-triangle: sky material {} failed to load: {}",
                            SkyMaterialId.Value, sky.error().Detail);
            return;
        }
        m_SkyMaterial = *sky;

        // A handful of bright points along fixed directions. Matches the shader's SkyPoint struct
        // (float3 Direction, float Size, float3 Color, float Pad = 32 bytes).
        struct SkyPoint
        {
            vec3 Direction;
            f32 Size;
            vec3 Color;
            f32 Pad;
        };
        const vector<SkyPoint> points = {
            {.Direction = glm::normalize(vec3{0.3f, 0.8f, 0.5f}),
             .Size = 0.004f,
             .Color = vec3{1.0f, 0.9f, 0.8f},
             .Pad = 0.0f},
            {.Direction = glm::normalize(vec3{-0.6f, 0.4f, -0.7f}),
             .Size = 0.003f,
             .Color = vec3{0.8f, 0.85f, 1.0f},
             .Pad = 0.0f},
            {.Direction = glm::normalize(vec3{0.1f, 0.6f, -0.9f}),
             .Size = 0.005f,
             .Color = vec3{1.0f, 0.7f, 0.6f},
             .Pad = 0.0f},
        };

        m_SkyPointBuffer = Renderer::Buffer::Create(GetRenderContext(),
                                                    {
                                                        .Name = "Sky Points",
                                                        .Size = points.size() * sizeof(SkyPoint),
                                                        .Usage = Renderer::BufferUsage::Storage,
                                                        .HostMapped = true,
                                                    });
        std::memcpy(m_SkyPointBuffer->GetMappedData(), points.data(),
                    points.size() * sizeof(SkyPoint));

        // Register the buffer to get its bindless handle, then bind it to the material. The write
        // lands in the ring-buffered param block. PointCount is authored in the .vmat to match this
        // fixed point set.
        m_SkyPointHandle = GetRenderContext().GetBindlessRegistry().Register(m_SkyPointBuffer);
        auto& material = const_cast<MaterialInstance&>(*m_SkyMaterial.Get());
        material.SetStorageBufferHandle("Points", m_SkyPointHandle);

        // Author the sky onto the scene's one Sky component: a MaterialSky source with this
        // material. The renderer resolves it each Execute — no topology toggle or per-frame push.
        // The scene has one sky, so overwrite the existing Sky component (the level authors one)
        // rather than adding a second the resolve would ignore; add one only if none exists.
        Sky* skyComponent = world.TryGetFirst<Sky>();
        if (skyComponent == nullptr)
        {
            skyComponent = &world.Add<Sky>(world.CreateEntity());
        }
        auto* source =
            static_cast<MaterialSky*>(skyComponent->Source.SetActive(TypeIdOf<MaterialSky>()));
        source->Material = m_SkyMaterial;
        // Mode selects the direct per-pixel path or the baked-cube path; the two render the same
        // sky. Baked is the default (bake once, sample a cube per frame); HT_SKY_DIRECT opts into
        // the per-pixel path, so a run of each mode over the same material proves they agree.
        source->Mode = std::getenv("HT_SKY_DIRECT") != nullptr ? SkyMode::Direct : SkyMode::Baked;
        // The material sky lights nothing (only Baked can, activated later), so keep the tier at
        // background-only regardless of what the level's sky authored.
        skyComponent->Lighting = SkyLighting::None;
    }

    // Constructs the MCP server when HT_MCP=<port> is set (HT_MCP=0 picks an ephemeral port), so the
    // sample exposes its live world/render surface to an agent. This is the ~10-line consumer recipe:
    // fill an McpHost from the app's systems, construct the server, and pump it once per frame
    // (OnUpdate, above). Env-gated so the default HT_SMOKE/golden path opens no socket and no thread.
    // Writes (spawn/destroy/set-field) follow the second gate HT_MCP_WRITE; both default off.
    void StartMcpServerIfRequested()
    {
        const char* portEnv = std::getenv("HT_MCP");
        if (portEnv == nullptr)
        {
            return;
        }

        Mcp::McpServerInfo info;
        info.ServerName = "hello-triangle";
        info.Port = static_cast<u16>(std::atoi(portEnv));
        info.AllowMutations = std::getenv("HT_MCP_WRITE") != nullptr;

        // The host resolves live state per call on the render thread during Pump(). CurrentWorld
        // returns the managed world (null before it loads, which the world tools handle); Viewport /
        // ViewportNames expose the single primary viewport under "" and "primary". The server
        // captures the host by reference, so it is a member outliving m_McpServer.
        m_McpHost.emplace(Mcp::McpHost{
            .Types = GetTypeRegistry(),
            .Assets = GetAssetManager(),
            .CurrentWorld = [this] { return GetWorld(); },
            .Viewport = [this](string_view name) -> Renderer::Viewport*
            {
                if (name.empty() || name == "primary")
                {
                    return GetPrimaryViewport();
                }
                return nullptr;
            },
            .ViewportNames = [] { return vector<string>{"primary"}; },
        });

        m_McpServer = Mcp::McpServer::Create(info, *m_McpHost);
    }

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

    // Builds the opt-in point field: a box of random colored points around the scene origin,
    // deterministically seeded so the windowed and headless runs draw the same field. Sized well
    // under PointField::MaxPoints — this is a demonstration field, not a stress test.
    void BuildPointField()
    {
        constexpr u32 PointCount = 40000;
        constexpr f32 HalfExtent = 60.0f;

        vector<Renderer::FieldPoint> points;
        points.reserve(PointCount);
        Rng rng(0xF1E1DC0DEULL);
        for (u32 i = 0; i < PointCount; ++i)
        {
            const vec3 position(rng.NextFloat(-HalfExtent, HalfExtent),
                                rng.NextFloat(-HalfExtent, HalfExtent),
                                rng.NextFloat(-HalfExtent, HalfExtent));
            // Warm-to-cool random star colors, packed RGBA8 (R low byte).
            const u32 r = static_cast<u32>(rng.NextFloat(120.0f, 255.0f));
            const u32 g = static_cast<u32>(rng.NextFloat(120.0f, 255.0f));
            const u32 b = static_cast<u32>(rng.NextFloat(160.0f, 255.0f));
            const u32 color = r | (g << 8) | (b << 16) | (0xFFu << 24);
            points.push_back(Renderer::FieldPoint{
                .Position = position,
                .ColorRgba8 = color,
                .Size = rng.NextFloat(0.15f, 0.4f),
            });
        }

        m_PointField = Renderer::PointField::Create(GetRenderContext(),
                                                    {
                                                        .Name = "HelloTriangle Point Field",
                                                        .Points = points,
                                                        .CellSize = 12.0f,
                                                    });
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

    // The opt-in point field (HT_POINTFIELD), null when the mode is off. Built once in
    // OnWorldLoaded and drawn by the renderer's PointFieldScenePass each frame; released before
    // the renderer in OnDispose (the renderer only borrows it).
    Unique<Renderer::PointField> m_PointField;

    // The opt-in authored Sky material (HT_SKY_MATERIAL), off by default so the smoke golden is
    // untouched. When enabled, the sample loads a gradient sky material, fills a small point buffer,
    // registers it in the bindless registry, and binds it to the material by handle — exercising the
    // Sky material domain and the storage-buffer material input. The buffer + its handle are owned
    // here and released in OnDispose.
    AssetHandle<MaterialInstance> m_SkyMaterial;
    Ref<Renderer::Buffer> m_SkyPointBuffer;
    Renderer::StorageBufferHandle m_SkyPointHandle;

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

    // Each seat's authored input contexts, saved while gameplay is unfocused so the focus gate can
    // empty the stack (all actions None) and restore it on refocus, without fabricating the scheme.
    unordered_map<Entity, vector<AssetHandle<InputMappingContext>>> m_SuspendedContexts;

    // The optional MCP server and the provider seam it captures by reference. m_McpHost holds the
    // TypeRegistry/AssetManager references and the per-frame world/viewport closures; it must
    // outlive m_McpServer, so it is declared first (destroyed last) and reset after the server in
    // OnDispose. Both stay empty unless HT_MCP is set.
    optional<Mcp::McpHost> m_McpHost;
    Unique<Mcp::McpServer> m_McpServer;

    // Pauses the managed world's simulation so the broadphase reads `static`; never set in smoke.
    bool m_PauseSpin = false;

    // The opt-in two-seat split-screen mode, off by default. The requested state is toggled by F2
    // (and seeded from HT_SPLITSCREEN); ApplySplitScreenRequest brings the live state (m_Split-
    // ScreenActive) in line at the top-of-frame safe point. Windowed-only — the smoke path never
    // enters it, so the golden runs single-seat.
    bool m_SplitScreenRequested = false;
    bool m_SplitScreenActive = false;

    // Seat B's spawned root entities (empty when single-seat) and its Viewer seat, tracked so the
    // mode can despawn the seat and bind its right-half viewport on reconfigure.
    vector<Entity> m_SeatBRoots;
    Entity m_SeatBViewer = Entity::Null;
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
                            // display, not supersampling. The fixed allocation cap; a lower ceiling
                            // is the knob for a fixed perf budget, not the default posture.
                            .MaxAllocationScale = 1.0f,
                            // The windowed app opts into adaptive resolution: the per-frame sub-rect
                            // scale eases toward the GPU-frame-time budget over the fixed allocation,
                            // adapting cost without reallocating. The smoke capture leaves it off so
                            // the golden renders at the fixed baseline (the controller is inert
                            // headless anyway — no GPU timing means the sub-rect holds at the ceiling).
                            .DynamicResolution =
                                smoke
                                    ? std::nullopt
                                    : optional<
                                          Renderer::
                                              DynamicResolutionSettings>{Renderer::
                                                                             DynamicResolutionSettings{}},
                        },
                    // The engine bootstraps the world: it reads the cooked project, mounts its
                    // packs, loads the startup level, owns the running scene + simulation, and ticks
                    // + pushes the view each frame. The sample customizes the world in OnWorldLoaded.
                    .World = GameWorldInfo{.Project = "project.vengproj"},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()

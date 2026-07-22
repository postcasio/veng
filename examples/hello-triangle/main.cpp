#include <Veng/Application.h>
#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Module/Module.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/VolumeField.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>
#include <Veng/UI/UI.h>
#include <Veng/UI/DebugPanels.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Gui/StyleSheet.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Net/BlobCodec.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/Messages.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Session.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/AnimationSystem.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Requests.h>
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
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <vector>

#include <Veng/Math/AABB.h>

#include <glm/common.hpp>

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

// A tag the app seeds onto the level's settings entity when the launcher activated a net mode
// (`--server` or `--join`), so the sim-side spawn rule spawns the presentation seat alone (the
// pawn arrives from the spawn stream, or is spawned per-seat on the server) instead of the full
// local player. Absent — the default offline run — the rule spawns the whole player, unchanged.
struct MultiplayerMode
{
};

VE_TYPE(::MultiplayerMode, 0xF4220F737E702E78ULL);

// The sample's one game message channel: a minted ChannelId (vengc generate-id) both peers register
// against, carrying a ping/notify round-trip — the canonical message-channel usage. The joined
// client sends one reflected ChannelPing; the server's handler logs it and echoes it back on the
// same channel; the client's handler logs the notify.
constexpr Net::ChannelId DemoChannelId = 0xE762E43AD6721580ULL;

// The reflected value the demo channel carries. A message payload is an opaque Net::Blob the engine
// never interprets; the game packs a reflected value through the record form of the blob codec
// (EncodeBlobRecord/DecodeBlobRecord), which names its type on the blob so the receiving handler
// can discriminate and decode it.
struct ChannelPing
{
    u32 Sequence = 0;
};

VE_REFLECT(::ChannelPing, 0x98F30DB91D4AF9B7ULL)
VE_FIELD(Sequence, .DisplayName = "Sequence")
VE_REFLECT_END();

// Derives a stable account id from the --name launch token: FNV-1a over the name into each
// 64-bit half (differently seeded), so relaunching with the same name presents the same account
// and the server reattaches it, while distinct names collide only as a 128-bit hash would.
Net::AccountId AccountFromName(const string_view name)
{
    const auto fnv1a = [name](u64 hash)
    {
        for (const char c : name)
        {
            hash ^= static_cast<u8>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    Net::AccountId id{.Lo = fnv1a(14695981039346656037ULL), .Hi = fnv1a(0x9E3779B97F4A7C15ULL)};
    if (!id.IsValid())
    {
        id.Lo = 1;
    }
    return id;
}

// Advances every Spinner each frame about its own axis — the gameplay tick the windowed
// app drives through a SceneSimulation. Registered into the host SystemRegistry alongside
// the Spinner type.
class SpinnerSystem final : public SceneSystem
{
public:
    void OnUpdate(Scene& scene, const f32 delta, const SystemContext& context) override
    {
        scene.Each<Transform, Spinner>(
            [&](const Entity entity, Transform& transform, Spinner& spinner)
            {
                // The authority-filter idiom for a game Sim system: spin only an entity this peer
                // simulates. On a client the decorative props are client-local (Authority::Local,
                // loaded from the identical pack), so they spin here; a Server/Remote-tier entity is
                // skipped — its motion arrives on the wire, never fought locally.
                if (!HasAuthority(context, scene, entity))
                {
                    return;
                }
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

// The cooked HUD document instantiated over the primary viewport; its stylesheet (with the shared
// `--accent` / `--tick-idle` tokens) is referenced from the document markup and resolved by the engine.
constexpr AssetId HudDocumentId{0xB51B7421AFE8CD18ULL};

// The single-entity pawn a networked player drives: a capsule with Intent/Mover whose mesh is an
// inline recipe (so a prefab spawn carries it). Spawned per seat on the server — the listen host's
// own seat and each connection's — and instantiated from this same prefab id on every client, so a
// pawn's mesh arrives with the reliable spawn stream, not as replicated per-component state.
constexpr AssetId NetPawnPrefabId{0x01FE4465D602376BULL};

// The second hosted world the swap demo hops a joined client into under one persistent scene. The
// engine never interprets a WorldKey; the game names its regimes (here, the default world and this).
inline Net::WorldKey RegimeBKey()
{
    return Net::WorldKey::FromU64(0x0B);
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

// Drives the cursor seat's input focus from data: a View-phase system (once per frame) that reads
// the seat's current focus and the frame's raw input, and stamps a builtin FocusRequest expressing
// the focus the seat should hold — the engine owns the focus token behind it. It captures gameplay
// focus at start and on a scene click (outside any ImGui window), and releases it on Escape, so the
// mouse drives the game while captured and the debug UI while free — the focus policy every windowed
// consumer needs, now authored as a level system rather than hand-rolled in the app's OnUpdate. A
// gameplay system driving input focus is exactly what the FocusRequest seam exists for; the request
// is drained at the engine's frame-safe point, and its per-seat token composes with any overlay
// focus scope. Reuses one holder entity so a session of clicks does not accrete entities.
class GameplayFocusSystem final : public SceneSystem
{
public:
    [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

    void OnStart(Scene& scene, const SystemContext&) override
    {
        // Start captured, the shipped windowed default. Headless (no window) makes the token's cursor
        // grab a no-op, so a smoke/headless run is unaffected.
        Stamp(scene, InputFocus::Gameplay);
    }

    void OnUpdate(Scene& scene, const f32, const SystemContext& context) override
    {
        if (context.GameplayFocused)
        {
            // Escape frees the cursor for the debug UI.
            if (context.Input.WasKeyPressed(Key::Escape))
            {
                Stamp(scene, InputFocus::UI);
            }
        }
        else if (context.Input.WasMouseButtonPressed(MouseButton::Left) && !UI::WantCaptureMouse())
        {
            // A left click on the scene (not on an ImGui window) re-captures the cursor.
            Stamp(scene, InputFocus::Gameplay);
        }
    }

private:
    // Stamps the requested focus onto a single reused holder entity: the drain removes the component
    // when it reconciles the token, so re-add it when absent, else overwrite a not-yet-drained one.
    void Stamp(Scene& scene, const InputFocus focus)
    {
        if (m_Holder.IsNull() || !scene.IsAlive(m_Holder))
        {
            m_Holder = scene.CreateEntity();
            scene.Add<Name>(m_Holder).Value = "Focus Request";
        }
        if (auto* existing = scene.TryGet<FocusRequest>(m_Holder))
        {
            *existing = FocusRequest{.Focus = focus};
        }
        else
        {
            scene.Add<FocusRequest>(m_Holder, FocusRequest{.Focus = focus});
        }
    }

    // The reused request holder; recreated if a scene reset invalidates it.
    Entity m_Holder = Entity::Null;
};

VE_SYSTEM(GameplayFocusSystem, 0x538985F20107EF02ULL, "Gameplay Focus");

// The game mode's spawn rule: a Sim-phase system that instantiates the configured player
// prefab at start and tears it down when play stops. The player prefab authors its own
// Viewer/Possesses/Camera/CameraFollow wiring, so the rule only picks (the GameModeConfig's
// PlayerPrefab) and spawns — no imperative wiring. It spawns at OnStart, before the first
// Update, so the spawn is deterministic and the pinned smoke frame (which never ticks Update)
// renders the authored camera pose. A game with richer mode state authors its own components
// beside the config and adds rule systems that read them.
class SpawnPlayerRule final : public SceneSystem
{
public:
    void OnStart(Scene& scene, const SystemContext& context) override
    {
        // The game-mode config is a scene component on the level's settings entity; find it by
        // type rather than a well-known entity (Scene::TryGetFirst). A scene without one runs
        // the rule as a no-op.
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
        //
        // In a net session (the app tagged the world MultiplayerMode) the prefab's Server-tier pawn
        // is skipped: on a client the pawn arrives from the spawn stream and interpolates; on the
        // listen host the app pawns this seat with the shared net pawn (so the same capsule
        // replicates to every peer). Either way the local presentation — the Local-tier follow
        // camera and the SeatInput control seat — spawns here, and the possession/pawning wires it.
        // Offline (no tag) the whole player spawns as before, byte-for-byte the single-player path.
        const bool multiplayer = scene.TryGetFirst<MultiplayerMode>() != nullptr;
        const Prefab::SpawnOptions options{.SkipServerAuthoritative = multiplayer};
        m_Spawned = config->PlayerPrefab.Get()->SpawnInto(scene, context.Assets, options).Roots;
    }

    void OnStop(Scene& scene, const SystemContext&) override { Despawn(scene); }

    // The spawn happens once at OnStart and the teardown at OnStop; a scoring or win-condition
    // rule would be a second system reading its own mode-state components.
    void OnUpdate(Scene&, const f32, const SystemContext&) override {}

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

// The status HUD's presentation driver, named on the HUD overlay and instantiated by the engine
// once per claimed instance: it resolves the `count`-repeated tick pool by class on instantiate and
// sweeps a highlight across it each frame — the per-instance form of the find-and-bind HUD system a
// consumer used to hand-roll, with no per-frame app code and no entity-keyed state. It reads and
// writes only its own document, so it stamps no scene component and needs no ViewOutput tag.
class HudDriver final : public GuiDriver
{
public:
    void OnInstantiate(Gui::Document& document, Scene&, Entity) override
    {
        m_Ticks = document.FindAllByClass("tick");
    }

    void OnUpdate(const GuiDriverFrame& frame) override
    {
        if (m_Ticks.empty())
        {
            return;
        }

        // The two tick colors mirror the stylesheet's --accent / --tick-idle tokens; the driver
        // owns the sweep phase, so two split-screen instances light independently.
        const vec4 accent{0.31f, 0.639f, 1.0f, 1.0f};
        const vec4 idle{0.263f, 0.314f, 0.4f, 0.533f};

        m_Phase += frame.Delta;
        const usize active =
            static_cast<usize>(m_Phase * 3.0f) % m_Ticks.size(); // three ticks per second
        for (usize i = 0; i < m_Ticks.size(); i++)
        {
            frame.Document.SetTextColor(*m_Ticks[i], i == active ? accent : idle);
        }
    }

private:
    vector<Gui::Element*> m_Ticks;
    f32 m_Phase = 0.0f;
};

VE_GUI_DRIVER(HudDriver, 0xE46A19EB6642A7D3ULL, "HUD");

class HelloTriangleApp final : public Application
{
public:
    HelloTriangleApp(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems)
        : Application(WithIdentity(std::move(info), this), types, systems)
    {
    }

private:
    // Wires the sample's account identity into the net knobs: a stable hash of the --name launch
    // token, so a relaunch with the same name reattaches as the same account; with no name it
    // falls through to the engine's process-random ephemeral default. Also supplies the session
    // pose capture: at disconnect (and the save checkpoint) the engine asks the game to encode the
    // departing seat's pawn pose, and delivers it back on reattach — so a returning player stands
    // where they left, not at the spawn point. The hooks capture the app because they are evaluated
    // after bootstrap; only the pointer is taken here, never dereferenced during construction.
    static ApplicationInfo WithIdentity(ApplicationInfo info, HelloTriangleApp* app)
    {
        if (info.Net)
        {
            // The connect-and-enter front door (HT_ENTER=<host>): the client names the world it
            // enters on connect rather than auto-joining the default key, so the granted join
            // installs a fresh runner world and presents through the engine's present-on-ready
            // rebind — a scripted stand-in for a menu's "join into world X" button.
            if (std::getenv("HT_ENTER") != nullptr)
            {
                info.Net->AutoJoinDefaultWorld = false;
            }
            info.Net->Identity = [app]() -> Net::AccountId
            {
                const optional<string>& name = app->GetLaunchArguments().Name;
                return name.has_value() ? AccountFromName(*name) : Net::GenerateAccountId();
            };
            info.Net->CaptureTravelPose = [app](const WorldInstanceId world,
                                                const Entity seat) -> Net::Blob
            { return app->CapturePawnPose(world, seat); };
        }
        return info;
    }

    // Encodes the seat's possessed pawn Transform as the session pose payload — the game-defined
    // half of pose durability (the engine moves the bytes, never reads them). An unpawned or gone
    // seat yields an empty payload, leaving the record's last pose standing.
    [[nodiscard]] Net::Blob CapturePawnPose(const WorldInstanceId world, const Entity seat)
    {
        const World* resolved = GetWorldRunner().ResolveWorld(world);
        if (resolved == nullptr)
        {
            return {};
        }
        const Scene& scene = resolved->GetScene();
        if (seat.IsNull() || !scene.IsAlive(seat))
        {
            return {};
        }
        const auto* possesses = scene.TryGet<Possesses>(seat);
        if (possesses == nullptr || possesses->Pawn.IsNull() || !scene.IsAlive(possesses->Pawn))
        {
            return {};
        }
        const auto* transform = scene.TryGet<Transform>(possesses->Pawn);
        if (transform == nullptr)
        {
            return {};
        }
        return Net::EncodeBlobRecord(*transform, GetTypeRegistry());
    }

    // Decodes a session pose payload back onto a freshly spawned pawn — the reattach arrival. A
    // payload of another shape (or none) leaves the prefab's authored pose.
    void ApplySessionPose(Scene& world, const Entity pawn, const Net::Blob& pose)
    {
        if (!world.Has<Transform>(pawn))
        {
            return;
        }
        if (const optional<Transform> decoded =
                Net::DecodeBlobRecord<Transform>(pose, GetTypeRegistry()))
        {
            world.Get<Transform>(pawn) = *decoded;
        }
    }

protected:
    void OnInitialize() override
    {
        m_SmokeOutput = std::getenv("HT_SMOKE");
        m_DeferRestore = std::getenv("HT_DEFER_RESTORE") != nullptr;

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
            m_SceneTexture = GetImGuiLayer()->CreateTexture(
                *m_SceneSampler, *GetManagedViewports().Get(0)->GetOutput());
        }
    }

    // Resolves the engine-managed world's scene by handle through the runner, or null before the
    // world comes online (a client join not yet accepted).
    [[nodiscard]] Scene* ManagedScene()
    {
        const World* managed = GetWorldRunner().ResolveWorld(GetManagedWorldId());
        return managed != nullptr ? &managed->GetScene() : nullptr;
    }

    // The engine has mounted the pack, loaded the startup level, spawned the world, and seeded the
    // managed view from the level's render settings; the sample seeds its own editable topology
    // copy here, adds its extras, and (smoke) waits on residency before the deterministic capture.
    void OnWorldLoaded(WorldInstanceId world, Scene& scene, ResidencyBatch& pending) override
    {
        // Seed the editable topology copy from the scene — the level's post knobs (a seeded
        // LevelRenderSettings component) — read by the same query the engine used, so the debug
        // RenderSettingsEditor starts in sync. The exposure and bloom already rode the engine's
        // view push. The sky is the scene's Sky component, resolved by the renderer itself each
        // Execute. Absent settings leave the defaults. The world handle is the hook's argument —
        // GetManagedWorldId() is not yet bound while the runner is still opening this world.
        if (const LevelRenderSettings* render = scene.TryGetFirst<LevelRenderSettings>())
        {
            ApplyLevelRenderSettings(*render, m_SceneSettings, GetWorldViewState(world));
        }

        // SSR is off by default in the engine; the sample opts in to show reflections off the
        // gradient-roughness ground plane (at the engine-default half SSR resolution).
        m_SceneSettings.SSR = true;

        // BloomThreshold is not a level field; the sample lifts the knee so the weak lights bloom.
        GetWorldViewState(world).BloomThreshold = 0.5f;

        // HT_DEBUG_VIEW pins a debug visualization mode by its DebugView enum index (the headless
        // capture has no combo): it overrides the level's Final mode so a g-buffer/battery target
        // can be captured and inspected.
        if (const char* dv = std::getenv("HT_DEBUG_VIEW"))
        {
            m_SceneSettings.Mode = static_cast<Renderer::DebugView>(std::atoi(dv));
        }

        // Apply the sample's topology (SSR + any HT_DEBUG_VIEW override) to the managed viewport;
        // the engine already configured it from the level, so this layers the sample's extras on.
        // Recreates the scene texture.
        ReconfigureScene();

        if (m_SmokeOutput)
        {
            // Smoke renders a fixed pose: pause the simulation so the spinners hold the pinned
            // SmokeAngle (set in OnUpdate) and the View-phase camera rig does not trail, and block
            // until the world spawn's streamed meshes are resident before the capture frame.
            SetWorldPaused(world, true);
            pending.WaitResident(GetTaskSystem());
        }
        else
        {
            // A net launch (`--server` or `--join`) tags the world so the spawn rule seeds only the
            // local presentation seat and the app owns the pawns (the join stream on a client, the
            // per-seat spawn on the server). Seeded before the simulation starts, so the rule's
            // OnStart reads it. The default offline run leaves it untagged — the spawn rule spawns
            // the whole local player exactly as before.
            const LaunchArguments& launch = GetLaunchArguments();
            if (launch.Server || launch.Join.has_value())
            {
                Entity settings = Entity::Null;
                for (auto [entity, config] : scene.View<GameModeConfig>())
                {
                    settings = entity;
                    break;
                }
                if (!settings.IsNull())
                {
                    scene.Add<MultiplayerMode>(settings);
                }
            }

            SetupHud(scene);

            // Offline windowed: open a second, independently-simulated world through the runner and
            // present it in the corner picture-in-picture viewport (managed viewport #1). The two
            // worlds are flat peers — the single WorldRunner ticks both each frame and each managed
            // viewport pulls its own world's camera — so the sample is a live consumer of the
            // multi-world path, not only the tests. A net launch skips it so the hosted/joined world
            // stays the sole world on the wire.
            if (!launch.Server && !launch.Join.has_value())
            {
                OpenSecondaryWorld(GetWorldLevel(world));
            }
        }
    }

    // Opens a second flat-peer world spawning the same startup level and binds it to the corner
    // picture-in-picture viewport (managed viewport #1), so the engine presents two worlds at once
    // — well within the 16-simultaneous-view ceiling. A no-op when the PiP viewport is absent (the
    // smoke path configures a single viewport) or the level handle is invalid.
    void OpenSecondaryWorld(const AssetHandle<Level>& level)
    {
        if (GetManagedViewports().GetCount() < 2 || !level.Id().IsValid())
        {
            return;
        }

        m_SecondWorld = GetWorldRunner().OpenWorld(WorldOpenInfo{
            .Source = level,
            .SimTickRate = 60,
            .StartSimulation = true,
            .MakeStartContext =
                [this]
            {
                return SystemContext{.Assets = GetAssetManager(),
                                     .Input = GetInput(),
                                     .Tasks = GetTaskSystem(),
                                     .Role = GetNetRole()};
            },
        });

        // The PiP presents the second world's authored scene-primary camera (no bound Viewer); the
        // two worlds' spinners drift apart as each ticks on its own clock, so the corner view is
        // visibly a distinct world rather than a mirror of the primary.
        GetManagedViewports().SetViewportWorld(1, m_SecondWorld);
    }

    // Aims the joined client's local follow camera at the pawn its replicated seat now possesses
    // (Entity::Null when it possesses none). The Local-tier seat and camera were spawned by the
    // spawn rule from the same player prefab (its Server-tier pawn skipped); this only names the
    // camera's target. The pawn itself is a server-owned Remote-tier mirror the interpolation
    // system drives — nothing here simulates it.
    void OnClientPossession(Scene& world, const Entity pawn) override
    {
        // The own presentation seat is the local input seat (the one carrying SeatInput — the
        // replicated own seat mirror has none). Point its Possesses at the newly possessed pawn so the
        // ControlSystem drives that pawn's Intent, and aim its follow camera at it. The client has
        // promoted the pawn to Tier::Predicted, so movement then runs for it on the input tick.
        world.Each<Viewer, SeatInput>(
            [&](const Entity seat, const Viewer& viewer, const SeatInput&)
            {
                if (world.Has<Possesses>(seat))
                {
                    world.Get<Possesses>(seat).Pawn = pawn;
                }
                if (!viewer.Camera.IsNull() && world.IsAlive(viewer.Camera) &&
                    world.Has<CameraFollow>(viewer.Camera))
                {
                    world.Get<CameraFollow>(viewer.Camera).Target = pawn;
                }
            });
    }

    // Builds the status HUD as a scene component: spawns a HUD entity carrying a GuiOverlay that
    // names both the cooked document and the HudDriver. The engine owns the whole HUD from data —
    // load, instantiate, attach, and the per-instance driver that resolves the tick pool and sweeps
    // the highlight each frame — so the app writes no per-frame HUD code. The smoke path adds no
    // overlay, so the golden capture is the 3D scene only.
    void SetupHud(Scene& world)
    {
        AssetManager& assets = GetAssetManager();
        const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
            assets.LoadSync<Gui::UIDocument>(HudDocumentId);
        if (!recipe)
        {
            return;
        }

        const Entity hud = world.CreateEntity();
        world.Add<Name>(hud).Value = "HUD";
        auto& overlay = world.Add<GuiOverlay>(hud);
        overlay.Document = *recipe;
        overlay.Driver = GuiDriverIdOf<HudDriver>();
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

        // The deferred-restore path (HT_DEFER_RESTORE): boot suppressed the auto-restore, so the
        // startup level is what came up, and the game triggers the identical restore itself once it
        // would have opened the save its record lives in — the shape a front-end that owns the first
        // travel takes. ReleaseLocalSession is the inverse, run before opening a different save.
        if (m_DeferRestore && !m_RestoredLocalSession)
        {
            m_RestoredLocalSession = true;
            RestoreLocalSession();
        }

        // Hosting: pawn each seat that lacks one (the listen host's own and each connection's) and
        // reap a departed player's pawn (GetServerHost() is non-null only under `--server`). Runs
        // outside any scene iteration, so it may spawn/destroy freely.
        if (GetServerHost() != nullptr)
        {
            SyncPlayerPawns();
        }

        if (m_SmokeOutput)
        {
            // Smoke pins a fixed pose for golden comparison: the world is paused (no tick), so the
            // sample writes the deterministic SmokeAngle each frame and the engine pushes the
            // authored camera — byte-identical run to run.
            ManagedScene()->Each<Transform, Spinner>(
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

        // Gameplay-focus capture/release is driven by the level's GameplayFocusSystem through the
        // builtin FocusRequest seam (a click captures the cursor, Escape frees it for the debug UI),
        // so the app touches no focus state here. The seat's gameplay context is authored
        // `requiresGameplayFocus`, so the engine excludes it from resolution whenever ImGui owns the
        // cursor — every gameplay action resolves to None with no stack surgery.

        // Runtime net control through builtin request components: a system stamps a request onto the
        // world's scene and the engine drains it at its frame-safe point, so gameplay reaches the
        // start-hosting / connect / stop-net operations it cannot call directly. Here the mode keys
        // stand in for a menu's Host / Join / Leave buttons.
        PollNetModeRequests();
        PollConnectAndEnter();
        PollSwapDemo();
        PollProfileCaptureDemo();
        SyncDemoChannel();
    }

    // The consumer-side profiler hotkey recipe: the engine exposes the capture verbs, and a binding is
    // a call site over them — there is no engine hotkey registry. F5 toggles a triggered capture, F6
    // dumps the continuous ring, F7 toggles the ring on and off. The engine resolves each named
    // capture under its capture directory (no path is invented here). A capture request only ever
    // returns a Result, so a failure is reported, never fatal.
    void PollProfileCaptureDemo()
    {
        Diagnostics::Profiler& profiler = GetProfiler();

        if (GetInput().WasKeyPressed(Key::F5))
        {
            if (profiler.GetState().Status == Diagnostics::CaptureStatus::Capturing)
            {
                if (const Result<path> written = profiler.EndCapture())
                {
                    Log::Info("Capture written to {}", written.value().string());
                }
                else
                {
                    Log::Warn("EndCapture failed: {}", written.error());
                }
            }
            else if (const VoidResult begun =
                         profiler.BeginCapture(Diagnostics::ResolveCapturePath("hotkey"));
                     !begun)
            {
                Log::Warn("BeginCapture failed: {}", begun.error());
            }
        }
        if (GetInput().WasKeyPressed(Key::F6))
        {
            if (const Result<path> dumped =
                    profiler.DumpRing(Diagnostics::ResolveCapturePath("ring")))
            {
                Log::Info("Ring dumped to {}", dumped.value().string());
            }
            else
            {
                Log::Warn("DumpRing failed: {}", dumped.error());
            }
        }
        if (GetInput().WasKeyPressed(Key::F7))
        {
            const bool ringOn = profiler.GetState().Status == Diagnostics::CaptureStatus::Ring;
            profiler.SetRingEnabled(!ringOn);
        }
    }

    // Stamps the HT_ENTER connect-and-enter request once: connect to the named host and travel to
    // the regime-B world in one ConnectRequest, exercising the front door where the presenting join
    // lands in a fresh runner world (no default-key auto-join masking the presentation path).
    void PollConnectAndEnter()
    {
        if (m_EnterStamped)
        {
            return;
        }
        const char* host = std::getenv("HT_ENTER");
        Scene* const scene = ManagedScene();
        if (host == nullptr || scene == nullptr)
        {
            return;
        }
        m_EnterStamped = true;
        scene->Add(scene->CreateEntity(), ConnectRequest{.Host = host, .Join = RegimeBKey()});
    }

    // Packs a ChannelPing into an opaque message blob through the shared field serializer, naming
    // its reflected type so the receiving handler can discriminate and decode it.
    Net::Blob EncodePing(const ChannelPing& ping)
    {
        return Net::EncodeBlobRecord(ping, GetTypeRegistry());
    }

    // Decodes a demo-channel blob back into a ChannelPing; nullopt for a foreign type or bad bytes.
    optional<ChannelPing> DecodePing(const Net::Blob& blob)
    {
        return Net::DecodeBlobRecord<ChannelPing>(blob, GetTypeRegistry());
    }

    // Registers the demo message channel on whichever host is live and drives its ping/notify
    // round-trip: the joined client sends one ChannelPing after its join lands, the server's
    // handler logs it and echoes it back to the sending connection, and the client's handler logs
    // the notify. Handlers run at the engine's frame-safe delivery point (never mid-tick), so they
    // may touch scene state freely; this demo only logs. Registration re-arms when a host instance
    // changes (a runtime host/join/stop-net cycle constructs fresh hosts).
    void SyncDemoChannel()
    {
        ServerHost* const server = GetServerHost();
        if (server != m_ChannelServer)
        {
            m_ChannelServer = server;
            if (server != nullptr)
            {
                server->RegisterChannel(
                    DemoChannelId,
                    [this, server](const Net::ConnectionId from, const Net::Blob& blob)
                    {
                        const optional<ChannelPing> ping = DecodePing(blob);
                        if (!ping.has_value())
                        {
                            return;
                        }
                        Log::Info("demo channel: ping {} from connection {}", ping->Sequence, from);
                        // Echo the notify half of the round-trip back to the sender. A loopback
                        // delivery (the listen host's own player) arrives as ServerConnectionId
                        // and has no wire to echo down.
                        if (from != Net::ServerConnectionId)
                        {
                            (void)server->Send(from, DemoChannelId, EncodePing(*ping));
                        }
                    });
            }
        }

        ClientHost* const client = GetClientHost();
        if (client != m_ChannelClient)
        {
            m_ChannelClient = client;
            m_PingSent = false;
            if (client != nullptr)
            {
                client->RegisterChannel(DemoChannelId,
                                        [this](const Net::Blob& blob)
                                        {
                                            if (const optional<ChannelPing> ping = DecodePing(blob))
                                            {
                                                Log::Info("demo channel: notify {} from the server",
                                                          ping->Sequence);
                                            }
                                        });
            }
        }
        if (client != nullptr && !m_PingSent && client->IsJoined())
        {
            (void)client->Send(DemoChannelId, EncodePing(ChannelPing{.Sequence = 1}));
            m_PingSent = true;
        }
    }

    // Drives the adopt-in-place swap on a joined client: F12 hops the client between the default world
    // and regime B under one persistent scene. Make-before-break — the destination is adopted while the
    // current join stays live, and only once the destination is ready is the old join left, so the
    // scene's derived content never reloads and the client is never join-less.
    void PollSwapDemo()
    {
        const ClientHost* const client = GetClientHost();
        if (client == nullptr)
        {
            return;
        }

        // Complete a pending swap: the moment the adopted destination is ready, leave the departed join.
        if (m_SwapLeaveJoin != Net::ControlJoinId)
        {
            for (const Net::JoinId join : client->Joins())
            {
                if (join != m_SwapLeaveJoin && client->IsJoined(join))
                {
                    LeaveWorld(m_SwapLeaveJoin);
                    m_SwapLeaveJoin = Net::ControlJoinId;
                    m_CurrentRegime = m_SwapTarget;
                    break;
                }
            }
            return;
        }

        // Initiate on F12: adopt the other regime's world into the presented scene while this one stays.
        if (GetInput().WasKeyPressed(Key::F12) && client->IsJoined())
        {
            const WorldInstanceId presented = GetManagedViewportWorld(0);
            if (presented.IsValid())
            {
                m_SwapTarget =
                    m_CurrentRegime == RegimeBKey() ? Net::DefaultWorldKey : RegimeBKey();
                m_SwapLeaveJoin = client->CurrentJoinId();
                (void)JoinWorld(m_SwapTarget, presented);
            }
        }
    }

    // Stamps a net request onto the managed world when its mode key edges down. Only the key→request
    // mapping is the sample's; opening the transport and reporting the outcome is the engine's drain.
    // Skipped under a `--server` / `--join` launch (the world is already on the wire) and in headless.
    void PollNetModeRequests()
    {
        const LaunchArguments& launch = GetLaunchArguments();
        if (launch.Server || launch.Join.has_value() || launch.Headless)
        {
            return;
        }
        Scene* const scene = ManagedScene();
        if (scene == nullptr)
        {
            return;
        }

        const auto stamp = [scene](auto request)
        { scene->Add(scene->CreateEntity(), std::move(request)); };

        // F9 hosts, F10 connects to a local server, F11 returns to standalone.
        if (GetInput().WasKeyPressed(Key::F9))
        {
            stamp(HostRequest{});
        }
        if (GetInput().WasKeyPressed(Key::F10))
        {
            stamp(ConnectRequest{.Host = "127.0.0.1"});
        }
        if (GetInput().WasKeyPressed(Key::F11))
        {
            stamp(StopNetRequest{});
        }
    }

    void OnRender() override
    {
        if (GetImGuiLayer())
        {
            RenderUserInterface();
        }
    }

    // Runs before ~Application, while every engine service is still alive. Drop the MCP server first:
    // its destructor stops the listener thread and closes the socket, so no in-flight tool handler can
    // touch engine state while the rest of the app tears down. The remaining resources are members and
    // retire in declaration order.
    ~HelloTriangleApp() override { m_McpServer.reset(); }

private:
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
            .CurrentWorld = [this] { return ManagedScene(); },
            .Viewport = [this](string_view name) -> Renderer::Viewport*
            {
                if (name.empty() || name == "primary")
                {
                    return GetManagedViewports().Get(0);
                }
                return nullptr;
            },
            .ViewportNames = [] { return vector<string>{"primary"}; },
            .InjectInput = [this](Event& event) { GetInputRouter().PostInjectedEvent(event); },
            .RenderContext = [this] { return &GetRenderContext(); },
            .Profiler = [this] { return &GetProfiler(); },
        });

        m_McpServer = Mcp::McpServer::Create(info, *m_McpHost);
    }

    // Reflects the m_ShowVolume toggle onto the managed scene: builds the nebula on first enable and
    // carries it on a lazily created entity's VolumeField component while shown / a null Field while
    // hidden (the presence-driven pass drops). Runs from OnRender, after the frame's render, so the
    // scene edit lands next frame with no mid-iteration structural change.
    void SyncVolumeDemo()
    {
        Scene* scene = ManagedScene();
        if (scene == nullptr)
        {
            return;
        }
        if (m_ShowVolume && m_VolumeField == nullptr)
        {
            m_VolumeField = BuildNebula();
        }
        if (m_VolumeEntity.IsNull())
        {
            m_VolumeEntity = scene->CreateEntity();
            scene->Add<Name>(m_VolumeEntity).Value = "Nebula";
            scene->Add<VolumeField>(m_VolumeEntity);
        }
        scene->Get<VolumeField>(m_VolumeEntity).Field = m_ShowVolume ? m_VolumeField : nullptr;
    }

    // Builds the demo nebula: a 64^3 RGBA16F volume whose RGB is emission radiance density and A is
    // extinction density, from layered value noise densest near the box center and fading to the
    // padded border — generic procedural content (a glowing gas cloud), no external data. Built
    // synchronously on the render thread (OnRender), the blocking factory arm.
    Ref<Renderer::VolumeField> BuildNebula()
    {
        constexpr u32 N = 64;

        // A cheap integer hash → [0,1), and a smooth trilinear value noise + a 4-octave fbm over it.
        auto hash = [](i32 x, i32 y, i32 z) -> f32
        {
            u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u +
                    static_cast<u32>(z) * 2147483647u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
        };
        auto valueNoise = [&](vec3 p) -> f32
        {
            const vec3 i = glm::floor(p);
            const vec3 f = glm::fract(p);
            const vec3 u = f * f * (3.0f - 2.0f * f);
            const auto c = [&](i32 dx, i32 dy, i32 dz)
            {
                return hash(static_cast<i32>(i.x) + dx, static_cast<i32>(i.y) + dy,
                            static_cast<i32>(i.z) + dz);
            };
            const f32 x00 = glm::mix(c(0, 0, 0), c(1, 0, 0), u.x);
            const f32 x10 = glm::mix(c(0, 1, 0), c(1, 1, 0), u.x);
            const f32 x01 = glm::mix(c(0, 0, 1), c(1, 0, 1), u.x);
            const f32 x11 = glm::mix(c(0, 1, 1), c(1, 1, 1), u.x);
            return glm::mix(glm::mix(x00, x10, u.y), glm::mix(x01, x11, u.y), u.z);
        };
        auto fbm = [&](vec3 p) -> f32
        {
            f32 sum = 0.0f;
            f32 amp = 0.5f;
            for (u32 octave = 0; octave < 4; ++octave)
            {
                sum += amp * valueNoise(p);
                p *= 2.0f;
                amp *= 0.5f;
            }
            return sum;
        };

        std::vector<u8> voxels(static_cast<usize>(N) * N * N * 8);
        auto* halves = reinterpret_cast<u16*>(voxels.data());
        for (u32 z = 0; z < N; ++z)
        {
            for (u32 y = 0; y < N; ++y)
            {
                for (u32 x = 0; x < N; ++x)
                {
                    // Normalized [-1,1] position; a soft spherical falloff carves the cloud out of
                    // the box and modulates the noise so density fades to zero at the padded border.
                    const vec3 p = (vec3(x, y, z) / static_cast<f32>(N - 1)) * 2.0f - 1.0f;
                    const f32 radial = glm::clamp(1.0f - glm::length(p), 0.0f, 1.0f);
                    const f32 noise = fbm(vec3(x, y, z) * (6.0f / static_cast<f32>(N)));
                    const f32 density = glm::clamp(radial * (0.4f + noise), 0.0f, 1.0f);
                    const f32 d = density * density; // sharpen the core

                    // A warm-to-cool emissive gradient by height, scaled by density; extinction rises
                    // with density so the dense core silhouettes against its own glow.
                    const vec3 warm(1.4f, 0.5f, 0.9f);
                    const vec3 cool(0.3f, 0.5f, 1.3f);
                    const vec3 emission =
                        glm::mix(warm, cool, glm::clamp(p.y * 0.5f + 0.5f, 0.0f, 1.0f)) *
                        (d * 2.0f);
                    const f32 extinction = d * 2.5f;

                    const usize base =
                        (static_cast<usize>(z) * N * N + static_cast<usize>(y) * N + x) * 4;
                    halves[base + 0] = glm::packHalf1x16(emission.x);
                    halves[base + 1] = glm::packHalf1x16(emission.y);
                    halves[base + 2] = glm::packHalf1x16(emission.z);
                    halves[base + 3] = glm::packHalf1x16(extinction);
                }
            }
        }

        Renderer::VolumeFieldData data;
        data.Name = "Demo Nebula";
        data.Resolution = {N, N, N};
        data.Format = Renderer::Format::RGBA16Sfloat;
        data.Bounds = AABB{.Min = vec3(-6.0f, -1.0f, -6.0f), .Max = vec3(6.0f, 8.0f, 6.0f)};
        data.Voxels = voxels;
        VE_ASSERT(data.IsValid(), "BuildNebula: voxel span size mismatch");
        return Renderer::VolumeField::BuildSync(GetRenderContext(), data);
    }

    // Configure can recreate the viewport's output image, so the ImGui texture must be re-fetched
    // after each call. The engine's gather reads the viewport output fresh per frame as its
    // placement, so it picks up the new view with no re-pointing here. Headless (smoke) has no
    // ImGui layer, so only the topology applies — there is no scene texture to refresh.
    void ReconfigureScene()
    {
        GetManagedViewports().Get(0)->Configure(m_SceneSettings);
        if (GetImGuiLayer())
        {
            m_SceneTexture = GetImGuiLayer()->CreateTexture(
                *m_SceneSampler, *GetManagedViewports().Get(0)->GetOutput());
            m_SceneTextureGeneration = GetManagedViewports().Get(0)->GetOutputGeneration();
        }
    }

    void RenderUserInterface()
    {
        Renderer::Viewport& viewport = *GetManagedViewports().Get(0);

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
            if (UI::RenderSettingsEditor(m_SceneSettings, GetWorldViewState(GetManagedWorldId()),
                                         viewport))
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
                SetWorldPaused(GetManagedWorldId(), m_PauseSpin);
            }

            // Default-off demo of the volume-field pass: toggles a procedural emissive nebula
            // ray-marched against the scene (depth-aware), built lazily on first enable.
            if (UI::Checkbox("Volume nebula", m_ShowVolume))
            {
                SyncVolumeDemo();
            }
        }

        // The combined frame-time graph; the stateful helper samples the CPU/GPU/per-pass timers
        // itself and overlays the whole-frame GPU time with each pass on one chart.
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

    // Reconciles the server scene's pawns against its seats: pawn every seat that lacks a live pawn,
    // and reap a pawn whose owning connection has gone. Two kinds of seat get a pawn — the listen
    // host's own Local-tier control seat (the spawn rule seeded it; skipped on a dedicated headless
    // server, which has no local player) and each connection's Server-tier seat (the host spawns a
    // bare Viewer+Possesses seat per connection, no SeatInput — the remote path). Both thread their
    // owner onto the pawn's Authority::Owner and associate the pawn's prefab id so the spawn
    // replicates as an instantiation (its capsule mesh included), not per-component state. The
    // connection's wire input (or the host's local input) then fills the seat's PlayerInput and the
    // unchanged control → intent → movement pipeline drives the pawn.
    void SyncPlayerPawns()
    {
        ServerHost& host = *GetServerHost();
        // Host a second world once, so a joined client can hop into it under one scene (the swap demo).
        EnsureRegimeWorld(host);
        SyncPawnsForWorld(host, GetManagedWorldId(), *ManagedScene());
        if (const World* regime = GetWorldRunner().ResolveWorld(m_RegimeWorld))
        {
            SyncPawnsForWorld(host, m_RegimeWorld, regime->GetScene());
        }
    }

    // Opens a second hosted world spawning the same level and registers it under the regime-B key, so
    // a joined client can adopt it in place. Idempotent; retries next frame until the level is resident.
    void EnsureRegimeWorld(ServerHost& host)
    {
        if (m_RegimeRegistered)
        {
            return;
        }
        const AssetHandle<Level>& level = GetWorldLevel(GetManagedWorldId());
        if (!level.Id().IsValid())
        {
            return;
        }
        m_RegimeRegistered = true;
        m_RegimeWorld = GetWorldRunner().OpenWorld(WorldOpenInfo{
            .Source = level,
            .SimTickRate = 60,
            .StartSimulation = true,
            .MakeStartContext =
                [this]
            {
                return SystemContext{.Assets = GetAssetManager(),
                                     .Input = GetInput(),
                                     .Tasks = GetTaskSystem(),
                                     .Role = NetRole::Server};
            },
        });
        const World* regime = GetWorldRunner().ResolveWorld(m_RegimeWorld);
        host.AddWorld(
            ServerWorldInfo{.WorldId = m_RegimeWorld,
                            .Key = RegimeBKey(),
                            .World = regime->GetScene(),
                            .LevelId = level.Id(),
                            .Replication = ReplicationServer::Settings{.SnapshotInterval = 2},
                            .Interest = Net::InterestSettings{.Radius = 0.0f}});
    }

    // The per-world pawn rule: pawn every seat in @p scene that lacks a live pawn, keyed by world so a
    // reaped seat's pawn is torn down with it.
    void SyncPawnsForWorld(ServerHost& host, WorldInstanceId worldId, Scene& world)
    {
        auto& seatPawns = m_SeatPawns[worldId.Value];
        const bool headless = GetLaunchArguments().Headless;

        // Collect first: spawning a pawn is a structural change, illegal mid-iteration.
        vector<Entity> unpawned;
        world.Each<Viewer, Possesses, Authority>(
            [&](const Entity seat, const Viewer&, const Possesses& possesses,
                const Authority& authority)
            {
                if (!possesses.Pawn.IsNull() && world.IsAlive(possesses.Pawn))
                {
                    return;
                }
                // A connection's remote seat is Server-tier; the listen host's own seat is Local
                // (and pawned only windowed — a dedicated server hosts no local player).
                const bool remote = authority.Tier == Tier::Server;
                const bool localHost = authority.Tier == Tier::Local && !headless;
                if (remote || localHost)
                {
                    unpawned.push_back(seat);
                }
            });

        for (const Entity seat : unpawned)
        {
            if (!world.IsAlive(seat))
            {
                continue;
            }
            const AssetResult<AssetHandle<Prefab>> prefab =
                GetAssetManager().LoadSync<Prefab>(NetPawnPrefabId);
            if (!prefab.has_value())
            {
                continue;
            }
            const vector<Entity> roots = prefab->Get()->SpawnInto(world, GetAssetManager()).Roots;
            if (roots.empty())
            {
                continue;
            }
            const Entity pawn = roots.front();
            const Net::ConnectionId owner = world.Get<Authority>(seat).Owner;
            world.Get<Authority>(pawn).Owner = owner;
            world.Get<Possesses>(seat).Pawn = pawn;

            // Reattach arrival: a returning account's session record carries the pose captured at
            // its disconnect — decode it onto the fresh pawn so the player stands where they left.
            // A first join's record carries no pose payload, leaving the prefab's authored pose.
            if (world.Has<SeatAccount>(seat))
            {
                const Net::SessionRecord* record =
                    host.Sessions().Find(world.Get<SeatAccount>(seat).Account);
                if (record != nullptr)
                {
                    ApplySessionPose(world, pawn, record->Gameplay.Pose);
                }
            }

            // Mark the pawn for replication as a prefab spawn, so a joiner instantiates the whole
            // netpawn prefab rather than receiving its bare replicated leaves. The host associates
            // the prefab SpawnInto recorded on its next Pump.
            world.Add<NetSpawn>(pawn);
            seatPawns[seat] = pawn;

            // The host's own follow camera (the spawn rule left its Local-tier target null when the
            // pawn was skipped) aims at the just-spawned pawn — the server-side counterpart of the
            // client's possession wiring.
            const Viewer& viewer = world.Get<Viewer>(seat);
            if (world.Has<SeatInput>(seat) && !viewer.Camera.IsNull() &&
                world.IsAlive(viewer.Camera) && world.Has<CameraFollow>(viewer.Camera))
            {
                world.Get<CameraFollow>(viewer.Camera).Target = pawn;
            }
        }

        // Reap a pawn whose seat the host tore down (a disconnect or a world-leave destroys the seat;
        // the pawn is a separate entity, so the game reaps it).
        for (auto it = seatPawns.begin(); it != seatPawns.end();)
        {
            if (!world.IsAlive(it->first))
            {
                if (world.IsAlive(it->second))
                {
                    world.DestroyEntity(it->second);
                }
                it = seatPawns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void WriteSceneCapture(const char* outPath) const
    {
        const Ref<Renderer::Image> output = GetManagedViewports().Get(0)->GetOutput()->GetImage();
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

    // Whether the boot auto-restore was opted out (HT_DEFER_RESTORE), making the sample drive
    // Application::RestoreLocalSession itself, and whether it has already done so.
    bool m_DeferRestore = false;
    bool m_RestoredLocalSession = false;

    // The optional MCP server and the provider seam it captures by reference. m_McpHost holds the
    // TypeRegistry/AssetManager references and the per-frame world/viewport closures; it must
    // outlive m_McpServer, so it is declared first (destroyed last), while the destructor resets the
    // server first. Both stay empty unless HT_MCP is set.
    optional<Mcp::McpHost> m_McpHost;
    Unique<Mcp::McpServer> m_McpServer;

    // Pauses the managed world's simulation so the broadphase reads `static`; never set in smoke.
    bool m_PauseSpin = false;

    // Default-off debug demo: a procedural CPU-noise nebula built through VolumeField::BuildSync and
    // attached to the managed scene via the VolumeField component, toggled from the debug panel. Off
    // by default so the smoke fixture and golden never author a volume — the presence-driven volume
    // pass stays absent and the capture is byte-identical. The field is built lazily on first enable
    // and the entity carries it while shown / a null Field while hidden.
    bool m_ShowVolume = false;
    Ref<Renderer::VolumeField> m_VolumeField;
    Entity m_VolumeEntity = Entity::Null;

    // The second flat-peer world presented in the corner picture-in-picture viewport; opened offline
    // windowed (invalid in smoke and net modes, which run a single world). The runner owns it, so it
    // needs no explicit teardown here.
    WorldInstanceId m_SecondWorld;

    // Server-side multiplayer state, populated only under `--server`: per hosted world, the pawn
    // spawned for each seat (the listen host's own and each connection's), keyed by world then seat so a
    // reaped seat's pawn is torn down with it. A joined client's own follow camera is aimed by
    // OnClientPossession, not held here (the spawn rule owns the client's local seat + camera).
    unordered_map<u64, unordered_map<Entity, Entity>> m_SeatPawns;

    // The second hosted world the swap demo hops a joined client into under one scene (opened + hosted
    // once under `--server`; invalid until then).
    WorldInstanceId m_RegimeWorld;
    bool m_RegimeRegistered = false;

    // Whether the one-shot HT_ENTER connect-and-enter request has been stamped.
    bool m_EnterStamped = false;

    // Client swap state (the make-before-break driver): the join to leave once the adopted destination
    // is ready, the key being adopted, and the regime the client currently presents.
    Net::JoinId m_SwapLeaveJoin = Net::ControlJoinId;
    Net::WorldKey m_SwapTarget;
    Net::WorldKey m_CurrentRegime = Net::DefaultWorldKey;

    // Demo-channel state: the host instances the channel is registered on (re-armed when a runtime
    // host/join cycle constructs fresh ones) and whether this session's one ping went out.
    ServerHost* m_ChannelServer = nullptr;
    ClientHost* m_ChannelClient = nullptr;
    bool m_PingSent = false;
};

// Factory captures the headless flag so the launcher stays game-agnostic.
extern "C" void VengModuleRegister(VengModuleHost* host)
{
    // The game registers only its own component and systems; the engine's reusable systems
    // (MovementSystem, CameraRigSystem, RootMotionDriveSystem, AnimationSystem) are pre-registered
    // by the host. The level's ordered SystemId set names the run order across both.
    host->Types.Register<Spinner>();
    host->Systems.Register<SpinnerSystem>();

    // The tag the app seeds onto the settings entity to switch the spawn rule into net mode.
    host->Types.Register<MultiplayerMode>();

    // The demo message channel's reflected payload, packed/decoded through the field serializer.
    host->Types.Register<ChannelPing>();

    // The game-mode rule spawns the configured player prefab at the session's start, so the pawn
    // and seat exist before the control pipeline ticks.
    host->Systems.Register<SpawnPlayerRule>();

    // The game-specific control mapping: reads input into a PlayerInput snapshot and produces the
    // Intent the engine's MovementSystem consumes.
    host->Systems.Register<ControlSystem>();

    // Drives the cursor seat's gameplay-focus capture/release from a level system through the
    // builtin FocusRequest seam, replacing a hand-rolled focus policy in the app's OnUpdate.
    host->Systems.Register<GameplayFocusSystem>();

    // The HUD's presentation driver, named on the HUD overlay component; the engine instantiates it
    // per claimed overlay instance and drives it each frame.
    if (host->Drivers != nullptr)
    {
        host->Drivers->Register<HudDriver>();
    }

    // Smoke mode: no window or swapchain, render off-screen and dump — the display-free CI path.
    const bool smoke = std::getenv("HT_SMOKE") != nullptr;

    // The engine owns the managed viewports (each's SceneRenderer + the gather + composite tail); the
    // app pushes only a ViewState. Viewport 0 is the primary, covering the window; windowed it also
    // gets a corner picture-in-picture (viewport 1) the app binds to a second flat-peer world at
    // runtime, so the engine drives two worlds and two viewports at once. Smoke keeps the single
    // golden viewport, so the golden capture (viewport 0's output) is byte-identical.
    vector<ManagedViewportInfo> managedViewports;
    managedViewports.push_back(ManagedViewportInfo{
        // Render at the full backing extent — native resolution on a HiDPI display, not
        // supersampling. The fixed allocation cap; a lower ceiling is the knob for a fixed perf
        // budget, not the default posture.
        .MaxAllocationScale = 1.0f,
        // The windowed app opts into adaptive resolution: the per-frame sub-rect scale eases toward
        // the GPU-frame-time budget over the fixed allocation, adapting cost without reallocating.
        // The smoke capture leaves it off so the golden renders at the fixed baseline (the controller
        // is inert headless anyway — no GPU timing means the sub-rect holds at the ceiling).
        .DynamicResolution =
            smoke ? std::nullopt
                  : optional<
                        Renderer::DynamicResolutionSettings>{Renderer::DynamicResolutionSettings{}},
    });
    if (!smoke)
    {
        // The picture-in-picture: a small top-right sub-region whose normalized Layout the compositor
        // re-fits across resizes. Topology is left at the default; the app binds its world in
        // OnWorldLoaded, before which it renders a cleared target (inert).
        managedViewports.push_back(ManagedViewportInfo{
            .Layout = {.Offset = {0.71f, 0.03f}, .Extent = {0.26f, 0.26f}},
        });
    }

    host->App.RegisterApplication(
        [smoke, managedViewports = std::move(managedViewports)](TypeRegistry& types,
                                                                SystemRegistry& systems) mutable
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
                    .ManagedViewports = std::move(managedViewports),
                    // The engine bootstraps the world: it reads the cooked project, mounts its
                    // packs, loads the startup level, owns the running scene + simulation, and ticks
                    // + pushes the view each frame. The sample customizes the world in OnWorldLoaded.
                    .World =
                        GameWorldInfo{
                            .Project = "project.vengproj",
                            // Both restore postures are exercised from one sample: the default
                            // resumes the local account's saved sitting at boot, while
                            // HT_DEFER_RESTORE opts out so the startup level comes up untouched and
                            // OnUpdate drives Application::RestoreLocalSession on demand.
                            .RestoreLocalSessionOnBoot = std::getenv("HT_DEFER_RESTORE") == nullptr,
                        },
                    // Opt into networking with the zero-config defaults: setting Net only tunes the
                    // hosts the engine mounts when a net launch flag activates one, so this stays
                    // inert with no flag (the default run is offline and byte-identical) and turns
                    // `--server` / `--join <host>` into a listening or joining session. The single
                    // managed world is joined by the engine's default WorldKey over the multiplexed
                    // transport — one joined world, one JoinId — so this exercises the common path.
                    .Net = GameNetInfo{},
                },
                types, systems));
        });
}

VE_EXPORT_MODULE_ABI()

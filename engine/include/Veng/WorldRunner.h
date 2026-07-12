#pragma once

#include <Veng/Veng.h>
#include <Veng/World.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Level.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class TypeRegistry;
    class SystemRegistry;
    class WorldRunner;
}

namespace Veng::Renderer
{
    class Context;
    class SceneCapture;
}

namespace Veng
{
    /// @brief Borrowed services a WorldRunner drives its worlds through.
    ///
    /// Types and Systems are required — every world's scene is created against the type registry and
    /// its simulation built from the system registry. Assets and Context are optional: a runner given
    /// neither is device-free, driving only empty-scene worlds (no cooked-level spawn, no
    /// capture-surface discovery). All borrowed pointers must outlive the runner and every world it
    /// creates.
    struct WorldRunnerInfo
    {
        /// @brief The type registry every world's scene is created against.
        TypeRegistry* Types = nullptr;
        /// @brief The system registry a world's SceneSimulation is built from.
        SystemRegistry* Systems = nullptr;
        /// @brief The asset manager a cooked-level world spawns through; null for a device-free runner.
        AssetManager* Assets = nullptr;
        /// @brief The render context capture-surface discovery uses; null for a device-free runner.
        Renderer::Context* Context = nullptr;
    };

    /// @brief Parameters for opening a world through WorldRunner::OpenWorld.
    ///
    /// A world spawns either a cooked Level (Source resident) or an empty scene (Source empty). Only
    /// the level path needs the asset manager; the empty path is device-free. The OnLoaded hook runs
    /// once with the freshly-spawned scene before the simulation starts, and MakeStartContext supplies
    /// the per-tick context the start runs with (the runner is transport-agnostic, so the net role
    /// rides in through this caller-built context, never a field on the world).
    struct WorldOpenInfo
    {
        /// @brief The level to spawn into the world; an empty handle opens an empty scene.
        AssetHandle<Level> Source;
        /// @brief How to spawn the level's world prefab (the client-mode server-authoritative skip).
        LevelLoadInfo Load;
        /// @brief Fixed simulation ticks per second this world's clock steps its Sim phase at.
        u32 SimTickRate = 60;
        /// @brief Whether to start the world's simulation now; false defers it (the client join target).
        bool StartSimulation = true;
        /// @brief For an empty-scene world, attach and drive an all-registered SceneSimulation.
        ///
        /// Meaningful only with an empty Source: the empty scene gets a SceneSimulation built from the
        /// whole system registry so the runner ticks it. A level-spawned world always carries the
        /// simulation the level built. Ignored when Source is a level.
        bool EmptySimulation = false;
        /// @brief Invoked once with the spawned scene and its residency batch, before the sim starts.
        function<void(WorldInstanceId, Scene&, ResidencyBatch&)> OnLoaded;
        /// @brief Builds the SystemContext the simulation starts with; required when StartSimulation.
        function<SystemContext()> MakeStartContext;
    };

    /// @brief What one WorldRunner::Tick observed across all worlds this frame.
    struct WorldTickResult
    {
        /// @brief True when at least one world was started and unpaused this frame.
        bool AnyActive = false;
        /// @brief True when at least one world ran one or more fixed Sim steps this frame.
        bool AnyTicked = false;
    };

    /// @brief The per-frame hooks WorldRunner::Tick drives each world through.
    ///
    /// The scheduler owns the loop (advance each world's clock, run its Sim steps then its View pass);
    /// these hooks thread back the caller-owned concerns the runner does not know — building a
    /// scene's SystemContext (which resolves the presenting viewport), the net server/client per-step
    /// work, and the net client's sim time-scale. Every hook but BuildContext is optional.
    struct WorldTickInfo
    {
        /// @brief The wall-clock frame delta in seconds folded into every world's clock.
        f32 Delta = 0.0f;
        /// @brief When false, the View phase is skipped this frame (a dedicated server).
        bool RunViewPhase = true;
        /// @brief Builds a world's per-tick SystemContext for a Sim step or the View pass.
        ///
        /// @param world      The world being ticked.
        /// @param scene      The world's scene.
        /// @param tick       The tick number to stamp (the Sim step, or the last completed tick in View).
        /// @param alpha      The interpolation fraction (0 in Sim, the frame residual in View).
        /// @param firstStep  True on the frame's first Sim step (false in View); resets a per-frame
        ///                   accumulator (see SystemContext::FirstStepThisFrame).
        function<SystemContext(WorldInstanceId world, const Scene& scene, u64 tick, f32 alpha,
                               bool firstStep)>
            BuildContext;
        /// @brief Returns a world's sim time-scale this frame (the net client's slew); 1 by default.
        function<f32(WorldInstanceId world)> SimScale;
        /// @brief Runs before each of a world's Sim steps (net server: change-tick + seat-input feed).
        function<void(WorldInstanceId world, Scene& scene, u64 tick)> BeforeSimStep;
        /// @brief Runs after each of a world's Sim steps (net client: stamp input + record prediction).
        function<void(WorldInstanceId world, Scene& scene, u64 tick)> AfterSimStep;
    };

    /// @brief An RAII refcounted pause on one world, released when the scope drops.
    ///
    /// While any WorldPauseScope on a world is held the world is paused; the scopes nest (stacked
    /// overlays) and compose with the explicit SetWorldPaused toggle, since the pause is a refcount
    /// underneath rather than a boolean one holder can clobber. Move-only; a moved-from scope releases
    /// nothing. Resolving a closed world's scope is inert.
    class WorldPauseScope
    {
    public:
        /// @brief Constructs an inert scope holding no pause.
        WorldPauseScope() = default;

        /// @brief Releases the held pause if this scope still owns one.
        ~WorldPauseScope();

        WorldPauseScope(const WorldPauseScope&) = delete;
        WorldPauseScope& operator=(const WorldPauseScope&) = delete;

        /// @brief Moves the pause, leaving @p other inert.
        WorldPauseScope(WorldPauseScope&& other) noexcept;

        /// @brief Moves the pause, releasing any pause this scope currently holds first.
        WorldPauseScope& operator=(WorldPauseScope&& other) noexcept;

    private:
        friend class WorldRunner;

        WorldPauseScope(WorldRunner& runner, WorldInstanceId world);

        void Release();

        /// @brief The runner holding the refcount; null on an inert or moved-from scope.
        WorldRunner* m_Runner = nullptr;
        /// @brief The world this scope pauses.
        WorldInstanceId m_World;
    };

    /// @brief The single scheduler owning a flat set of first-class worlds, each named by a handle.
    ///
    /// The sim-domain registry: it owns each World (holding its Unique<Scene> and per-world clock),
    /// mints a WorldInstanceId per world from an instance counter, and ticks every world serially on
    /// the render thread in id order. Worlds are flat peers — every API is handle-keyed and no
    /// tick or authority path special-cases any world. Nothing here reaches into presentation or
    /// transport: a viewport names a world by handle and asks the runner to resolve a camera
    /// (ResolveCameraView, a pure query), and the runner holds no pointer back out. Device-free when
    /// given no asset manager or context, so empty-scene worlds can be built and driven without a GPU.
    class WorldRunner
    {
    public:
        /// @brief Constructs a runner over the borrowed services.
        /// @param info  The type/system registries (required) and optional asset manager / context.
        explicit WorldRunner(const WorldRunnerInfo& info);

        /// @brief Destroys the runner and every owned world (adopted scenes are left to their owner).
        ~WorldRunner();

        WorldRunner(const WorldRunner&) = delete;
        WorldRunner& operator=(const WorldRunner&) = delete;

        /// @brief Opens a world (spawning a level or an empty scene) and returns its handle.
        ///
        /// Mints an id, spawns the world (@p info.Source resident → the level; empty → an empty
        /// scene), builds its simulation, runs @p info.OnLoaded with the spawned scene, and starts the
        /// simulation when @p info.StartSimulation. Runtime open is first-class. Returns only the
        /// handle, never a viewport or a Scene&.
        /// @param info  How to spawn and start the world.
        /// @return The opened world's handle.
        [[nodiscard]] WorldInstanceId OpenWorld(const WorldOpenInfo& info);

        /// @brief Closes a world, stopping its simulation and dropping it; the id then resolves to nothing.
        /// @param world  The world to close; an unminted or already-closed id is a no-op.
        void CloseWorld(WorldInstanceId world);

        /// @brief Resolves a world by handle, or null for an unminted, closed, or invalid id.
        /// @param world  The handle to resolve.
        /// @return The world, or nullptr.
        [[nodiscard]] const World* ResolveWorld(WorldInstanceId world) const;

        /// @brief Resolves a world by handle (mutable), or null for an unminted, closed, or invalid id.
        /// @param world  The handle to resolve.
        /// @return The world, or nullptr.
        [[nodiscard]] World* ResolveWorld(WorldInstanceId world);

        /// @brief Resolves a seat's camera in a world, at the caller's aspect — the gameplay→render query.
        ///
        /// Presentation asks the runner to resolve a camera in a world; the runner answers a pure
        /// value and never learns which viewport asked. An Entity::Null viewer resolves the scene's
        /// primary camera.
        /// @param world   The world to resolve the camera in.
        /// @param viewer  The seat entity carrying the Viewer, or Entity::Null for the scene primary.
        /// @param aspect  Viewport width divided by height; the render target owns aspect.
        /// @return The resolved view, or nullopt when the world or camera does not resolve.
        [[nodiscard]] optional<CameraView> ResolveCameraView(WorldInstanceId world, Entity viewer,
                                                             f32 aspect) const;

        /// @brief Ticks every world's Sim phase at its own fixed rate, then its View phase, in id order.
        ///
        /// Serial on the render thread: for each started, unpaused world, folds the frame delta (times
        /// its net slew) into its clock, runs the accumulated fixed Sim steps then one View pass,
        /// driving the caller's per-step hooks. A paused or unstarted world resets its accumulator so
        /// resuming chases no backlog.
        /// @param info  The frame delta, view-phase gate, and per-world tick hooks.
        /// @return What the tick observed across all worlds (for the input edge latch).
        WorldTickResult Tick(const WorldTickInfo& info);

        /// @brief Sets a world's explicit pause toggle, composing with any held PauseScopes.
        /// @param world   The world to pause or resume.
        /// @param paused  True to pause, false to clear the explicit toggle.
        void SetWorldPaused(WorldInstanceId world, bool paused);

        /// @brief Returns whether a world is paused (a held scope or the explicit toggle); false when unminted.
        /// @param world  The world to query.
        [[nodiscard]] bool IsWorldPaused(WorldInstanceId world) const;

        /// @brief Opens an RAII refcounted pause on a world, held for the returned scope's lifetime.
        /// @param world  The world to pause while the scope lives.
        /// @return The pause scope; inert when the world is unminted.
        [[nodiscard]] WorldPauseScope PauseScope(WorldInstanceId world);

        /// @brief Wraps an externally-owned scene as a non-owning tickable world and returns its handle.
        ///
        /// The transitional seam letting a scene the runner does not own be ticked on the same
        /// schedule as owned worlds. The runner never destroys an adopted scene; the scene
        /// self-unregisters (the world is dropped) when it is destroyed.
        /// @param scene  The externally-owned scene to tick; its lifetime stays with its owner.
        /// @return The adopted world's handle.
        WorldInstanceId AdoptSimulation(Scene& scene);

        /// @brief Installs a freshly-loaded scene as an already-open world's scene, and returns it.
        ///
        /// The client-join seam: world #0 is opened as an empty join target, then the accepted level
        /// loads into a scene the runner takes ownership of here (replacing the empty placeholder), so
        /// the joined scene is a runner-owned world rather than a parallel one. The caller starts it
        /// once the install lands.
        /// @param world  The open world to install the scene into.
        /// @param scene  The loaded scene the runner takes ownership of.
        /// @return The installed scene.
        Scene& InstallScene(WorldInstanceId world, Unique<Scene> scene);

        /// @brief Discovers each world's CaptureSurface components and drives them into the compositor.
        ///
        /// Iterates every world's scene (regardless of pause — registration, not run-state, gates
        /// capture driving) for Renderer::CaptureSurface components, materializing each one's
        /// SceneCapture on first sight and registering it through @p registerCapture, then pushing this
        /// frame's capture source. Requires the runner to have been given a context and asset manager.
        /// @param registerCapture  Registers a newly-materialized capture on the compositor drive-list.
        void DriveCaptureSurfaces(const function<void(Renderer::SceneCapture&)>& registerCapture);

        /// @brief Returns the owned worlds in id order, for per-world presentation drives.
        [[nodiscard]] const vector<Unique<World>>& GetWorlds() const { return m_Worlds; }

        /// @brief Returns whether the runner holds any world.
        [[nodiscard]] bool HasWorlds() const { return !m_Worlds.empty(); }

    private:
        friend class WorldPauseScope;

        /// @brief Mints the next never-reused world id from the instance counter.
        [[nodiscard]] WorldInstanceId MintId();

        /// @brief Increments a world's pause refcount (a WorldPauseScope open).
        void AcquirePause(WorldInstanceId world);

        /// @brief Decrements a world's pause refcount (a WorldPauseScope drop).
        void ReleasePause(WorldInstanceId world);

        /// @brief Drops adopted worlds whose scene has self-unregistered (been destroyed).
        void PruneAdopted();

        /// @brief The type registry every world's scene is created against.
        TypeRegistry* m_Types = nullptr;
        /// @brief The system registry a world's simulation is built from.
        SystemRegistry* m_Systems = nullptr;
        /// @brief The asset manager cooked-level worlds spawn through; null on a device-free runner.
        AssetManager* m_Assets = nullptr;
        /// @brief The render context capture-surface discovery uses; null on a device-free runner.
        Renderer::Context* m_Context = nullptr;

        /// @brief The owned and adopted worlds, in ascending id (open) order.
        vector<Unique<World>> m_Worlds;

        /// @brief The drive-list adopted scenes self-unregister from on destruction (see AdoptSimulation).
        vector<Scene*> m_AdoptedScenes;

        /// @brief The instance counter minting world ids; never reused, so a stale id resolves to nothing.
        u64 m_NextId = 1;
    };
}

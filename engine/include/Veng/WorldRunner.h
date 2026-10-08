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
    class SceneSimulation;
    class WorldRunner;
}

namespace Veng::Renderer
{
    class Context;
    struct CaptureSurface;
    class SceneCapture;
    class SceneCapturePool;
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
    /// once with the freshly-spawned scene before the simulation starts, which starts with the context
    /// the runner's factory builds for the new world (WorldRunner::SetContextFactory).
    struct WorldOpenInfo
    {
        /// @brief The level to spawn into the world; an empty handle opens an empty scene.
        AssetHandle<Level> Source;
        /// @brief How to spawn the level's world prefab (the client-mode server-authoritative skip).
        LevelLoadInfo Load;
        /// @brief Fixed simulation ticks per second this world's clock steps its Sim phase at.
        u32 SimTickRate = 60;
        /// @brief The most Sim steps this world runs in one frame; the backlog past it is dropped.
        ///
        /// See SimClockInfo::MaxTicksPerFrame. Must be positive.
        u32 MaxTicksPerFrame = 5;
        /// @brief The wall-clock budget for this world's Sim steps in one frame, in milliseconds;
        /// unset runs every step MaxTicksPerFrame allows.
        ///
        /// See SimClockInfo::MaxSimMillisecondsPerFrame: with a budget an overloaded world dilates
        /// time at a bounded frame cost rather than spending MaxTicksPerFrame steps every frame. The
        /// time it drops is reported as the `WorldRunner/DroppedMs` profiler counter. Not applied
        /// while the frame clock is driven (Time::IsDriven), so a driven run's step count stays a
        /// function of its frame deltas.
        optional<f32> MaxSimMillisecondsPerFrame;
        /// @brief Whether to start the world's simulation now; false defers it (the client join target).
        bool StartSimulation = true;
        /// @brief For an empty-scene world, the ordered system set its SceneSimulation runs.
        ///
        /// Meaningful only with an empty Source: engaged, the empty scene gets a SceneSimulation
        /// built from exactly this ordered set — an empty vector is legal and attaches a simulation
        /// running no systems (a ticking data world populated by other means); disengaged, the world
        /// carries no simulation. An empty world runs the systems its opener names, exactly as a
        /// level world runs the systems its level names; a caller wanting every registered system
        /// enumerates SystemRegistry::Entries(). Ignored when Source is a level.
        optional<vector<SystemId>> Systems;
        /// @brief Invoked once with the spawned scene and its residency batch, before the sim starts.
        function<void(WorldInstanceId, Scene&, ResidencyBatch&)> OnLoaded;
    };

    /// @brief Which lifecycle call or tick phase a SystemContextRequest asks a context for.
    enum class SystemContextPhase : u8
    {
        /// @brief The context a world's simulation starts with (each system's OnStart).
        Start,
        /// @brief The context of one live fixed Sim step.
        Sim,
        /// @brief The context of a frame's View pass.
        View,
        /// @brief The context a world's simulation stops with (each system's OnStop).
        Stop,
        /// @brief The context of one Sim step re-run by a client reconciliation replay.
        Replay,
    };

    /// @brief What a caller knows about the SystemContext it needs: the world, the scene and the step.
    ///
    /// Everything per-call a context carries beyond the services comes from here, so the one factory
    /// that fills the services (WorldRunner::SetContextFactory) builds every context — start, tick,
    /// stop and replay — in the same shape.
    struct SystemContextRequest
    {
        /// @brief The world the context is for; invalid only for a simulation the runner does not hold.
        WorldInstanceId World;
        /// @brief The scene the context's systems run over.
        const Veng::Scene& Scene;
        /// @brief The lifecycle call or tick phase the context is for.
        SystemContextPhase Phase = SystemContextPhase::Sim;
        /// @brief The tick number to stamp: the Sim step, or the last completed tick otherwise.
        u64 Tick = 0;
        /// @brief The interpolation fraction to stamp; nonzero only in the View phase.
        f32 Alpha = 0.0f;
        /// @brief Whether this is the frame's first Sim step (see SystemContext::FirstStepThisFrame).
        bool FirstStep = false;
        /// @brief Whether this is the frame's last Sim step (see SystemContext::LastStepThisFrame).
        bool LastStep = false;
    };

    /// @brief Builds the SystemContext a request describes, over the services its installer owns.
    using SystemContextFactory = function<SystemContext(const SystemContextRequest& request)>;

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
    /// The scheduler owns the loop (advance each world's clock, run its Sim steps then its View pass)
    /// and builds each step's SystemContext through its factory; these hooks thread back the
    /// caller-owned concerns the runner does not know — the net server/client per-step work and the
    /// net client's sim time-scale. Every hook is optional.
    struct WorldTickInfo
    {
        /// @brief The wall-clock frame delta in seconds folded into every world's clock.
        f32 Delta = 0.0f;
        /// @brief When false, the View phase is skipped this frame (a dedicated server).
        bool RunViewPhase = true;
        /// @brief Returns a world's sim time-scale this frame (the net client's slew); 1 by default.
        function<f32(WorldInstanceId world)> SimScale;
        /// @brief Runs before each of a world's Sim steps (net server: change-tick + seat-input feed).
        function<void(WorldInstanceId world, Scene& scene, u64 tick)> BeforeSimStep;
        /// @brief Runs after each of a world's Sim steps (net client: stamp input + record prediction).
        function<void(WorldInstanceId world, Scene& scene, u64 tick)> AfterSimStep;
    };

    /// @brief How one WorldRunner::DriveCaptureSurfaces pass resolves presentation and registration.
    ///
    /// The runner holds no back-reference out of the sim domain, so it cannot know which of its worlds
    /// a view shows; presentation answers that through IsPresented, and the compositor drive-list is
    /// joined through Register. Both hooks are required.
    struct WorldCaptureDriveInfo
    {
        /// @brief Registers a newly-materialized capture on the compositor drive-list.
        function<void(Renderer::SceneCapture&)> Register;
        /// @brief Whether any view presents a world — the gate on driving that world's captures.
        ///
        /// A capture feeds a material sampled by a mesh drawn in some view, so a world no view shows
        /// can have no capture of its own sampled and its captures are work nobody can see. A world
        /// being rebound onto a viewport counts as presented for the whole rebind (see
        /// ManagedViewportSet::IsWorldPresented), so a make-before-break swap presents a warm probe
        /// rather than a blank one.
        function<bool(WorldInstanceId)> IsPresented;
        /// @brief The most captures this pass builds new; a surface past it waits for a later pass.
        ///
        /// Building a capture builds a whole face renderer, so a world arriving with several capture
        /// surfaces would otherwise pay for all of them in its first presented frame. A surface handed
        /// a released capture of its configuration from the runner's pool builds nothing and is not
        /// counted. The engine drives one pass per frame, so the default is one new build per frame.
        u32 MaxNewCaptures = 1;
    };

    /// @brief What one WorldRunner::DriveCaptureSurfaces pass did across the open worlds.
    struct WorldCaptureDriveResult
    {
        /// @brief Worlds whose capture surfaces were driven, because a view presents them.
        u32 WorldsDriven = 0;
        /// @brief Worlds skipped whole, because no view presents them.
        u32 WorldsSkipped = 0;
        /// @brief Capture surfaces driven across the driven worlds.
        u32 SurfacesDriven = 0;
        /// @brief Capture surfaces re-armed in skipped worlds, so a resumed one refreshes.
        u32 SurfacesReArmed = 0;
        /// @brief Captures built new this pass, at most WorldCaptureDriveInfo::MaxNewCaptures.
        u32 CapturesBuilt = 0;
        /// @brief Captures this pass took from the runner's pool of released ones instead of building.
        u32 CapturesReused = 0;
        /// @brief Surfaces left unmaterialized and undriven this pass because the build budget was spent.
        u32 SurfacesDeferred = 0;
        /// @brief Disabled surfaces in the driven worlds, whose runtime is released and left empty.
        u32 SurfacesDisabled = 0;
    };

    /// @brief An RAII refcounted pause on one world, released when the scope drops.
    ///
    /// While any WorldPauseScope on a world is held the world is paused; the scopes nest (stacked
    /// overlays) and compose with the explicit SetWorldPaused toggle, since the pause is a refcount on
    /// the world's SceneSimulation rather than a boolean one holder can clobber. The release resolves
    /// the world when it runs, so it lands on whatever simulation the world holds then (InstallScene
    /// carries the pause onto a replacement), and a scope outliving its world releases nothing.
    /// Move-only; a moved-from scope releases nothing.
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

        /// @brief Returns whether this scope holds a pause it will release.
        ///
        /// False for a default, moved-from or released scope, and for one opened on a world that was
        /// unminted or had no simulation to pause.
        [[nodiscard]] bool IsHeld() const { return m_Runner != nullptr; }

    private:
        friend class WorldRunner;

        WorldPauseScope(WorldRunner& runner, WorldInstanceId world);

        void Release();

        /// @brief The runner the pause is released through; null when this scope holds no pause.
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

        /// @brief Destroys the runner and every world it owns, running no system's OnStop.
        ///
        /// A destructor has no SystemContext to stop a simulation with, so an owner that installed a
        /// context factory calls CloseAllWorlds first, while the services the factory reads are still
        /// alive. A device-free runner, which has no factory, simply drops its worlds.
        ~WorldRunner();

        WorldRunner(const WorldRunner&) = delete;
        WorldRunner& operator=(const WorldRunner&) = delete;

        /// @brief Opens a world (spawning a level or an empty scene) and returns its handle.
        ///
        /// Mints an id, spawns the world (@p info.Source resident → the level; empty → an empty
        /// scene), builds its simulation, runs @p info.OnLoaded with the spawned scene, and starts the
        /// simulation when @p info.StartSimulation, with the factory's Start context naming the new
        /// world. Runtime open is first-class. Returns only the handle, never a viewport or a Scene&.
        ///
        /// An open issued from inside Tick — a system opening a world from its own update — is
        /// immediate: the load hook and the start run nested in the opening world's tick, so the
        /// caller holds a valid handle and a live scene the moment this returns. The new world takes
        /// its first tick next frame, since the walk runs over the world count it captured at entry.
        /// @param info  How to spawn and start the world.
        /// @return The opened world's handle.
        /// @pre A context factory is installed when @p info.StartSimulation and the world carries a
        ///      simulation.
        [[nodiscard]] WorldInstanceId OpenWorld(const WorldOpenInfo& info);

        /// @brief Starts the simulation of a world opened with WorldOpenInfo::StartSimulation false.
        ///
        /// The deferred half of an open: a world whose scene arrives or is populated after the open
        /// (a client join target once its level installs, an overlay once its policy is applied)
        /// starts here, with the factory's Start context naming the world. A world carrying no
        /// simulation starts nothing.
        /// @param world  The open, not yet started world to start.
        /// @pre A context factory is installed, @p world resolves, and its simulation is not started.
        void StartWorld(WorldInstanceId world);

        /// @brief Stops a started world's simulation and leaves the world open and unticked.
        ///
        /// Each system's OnStop runs with the factory's Stop context naming the world, under the role
        /// it ran under; the world then holds still (Tick runs no phase on an unstarted world) until a
        /// later InstallScene replaces its scene, StartWorld starts it again, or it closes. Stopping is
        /// idempotent: a world whose simulation is unstarted or already stopped, or that carries none,
        /// stops nothing, and a later close or InstallScene runs no second OnStop.
        /// @param world  The world to stop; an unminted or already-closed id is a no-op.
        /// @pre Not inside Tick. A context factory is installed when @p world's simulation is started.
        void StopWorld(WorldInstanceId world);

        /// @brief Installs the one factory every SystemContext the runner's worlds receive is built by.
        ///
        /// The runner builds through it at every lifecycle point it drives — a world's start (OpenWorld,
        /// StartWorld), each Sim step and View pass (Tick), and its stop (StopWorld, CloseWorld,
        /// CloseAllWorlds, and a started scene InstallScene replaces) — and a caller
        /// stepping a world's scene outside those (a reconciliation replay, ReplaySimStep) builds
        /// through BuildContext, so every context names its world and carries the same services.
        ///
        /// Unset — the default — is the device-free contract: CloseWorld and InstallScene drop a started
        /// scene without running OnStop rather than fabricating a context, and starting, stopping
        /// (StopWorld) or ticking a simulation asserts.
        /// @param factory  The context factory, or an empty function to clear it.
        void SetContextFactory(SystemContextFactory factory);

        /// @brief Returns whether a context factory is installed.
        [[nodiscard]] bool HasContextFactory() const { return static_cast<bool>(m_ContextFactory); }

        /// @brief Builds a SystemContext through the installed factory.
        ///
        /// The one public entry every context builder calls. @p request names the world it is for;
        /// the only caller that names no world (an invalid id) is a simulation the runner does not
        /// hold — an editor's hand-driven Play session.
        /// @param request  The world, scene, phase and step the context is for.
        /// @return The context the factory built.
        /// @pre A context factory is installed.
        [[nodiscard]] SystemContext BuildContext(const SystemContextRequest& request) const;

        /// @brief Returns the world whose live scene is @p scene, or an invalid id when none is.
        ///
        /// The reverse of ResolveWorld, for a caller handed a scene by a layer that holds scenes rather
        /// than runner handles (the net client host's replay hook).
        /// @param scene  The scene to look up.
        /// @return The holding world's id, or an invalid id.
        [[nodiscard]] WorldInstanceId FindWorld(const Scene& scene) const;

        /// @brief Re-runs one Sim step of a world's scene as a reconciliation replay.
        ///
        /// Resolves @p scene to the world holding it (FindWorld), builds its Replay context naming that
        /// world and stamping @p tick, and advances the scene's Sim phase by that world's own fixed
        /// step — so a client replaying any of several joined worlds replays it under its own id, role
        /// and tick rate. The caller feeds the step's recorded input first.
        /// @param scene  The live scene of a world this runner holds.
        /// @param tick   The tick being replayed.
        /// @pre A context factory is installed and @p scene is a world this runner holds.
        void ReplaySimStep(Scene& scene, u64 tick);

        /// @brief Sets the hook told a world's scene is about to be destroyed.
        ///
        /// Called with the scene while it is still live, once per scene the runner destroys: a world
        /// dropped by CloseWorld (after its OnStop), or a scene InstallScene replaces (likewise). A
        /// presentation layer that retains a raw scene pointer across frames uses it to drop that
        /// pointer before it dangles.
        /// @param hook  The retiring hook, or an empty function to clear it.
        void SetSceneRetiringHook(function<void(const Scene&)> hook);

        /// @brief Closes a world, stopping its simulation and dropping it; the id then resolves to nothing.
        ///
        /// Outside Tick the close is immediate. Issued from inside Tick — a system closing its own or
        /// another world from its update — it is deferred instead: the world is queued, takes no
        /// further Sim or View phase this frame, and is stopped (OnStop with its Stop context, exactly
        /// as an immediate close) and dropped once the walk finishes, in the order the closes were
        /// issued. A queued world still resolves until it drains, so the caller's scene reference
        /// stays live for the rest of its own call. Closing one world twice within a tick closes it
        /// once, and a close issued from a system's OnStop during the drain drains in its turn.
        /// @param world  The world to close; an unminted or already-closed id is a no-op.
        void CloseWorld(WorldInstanceId world);

        /// @brief Closes every open world, newest first, each exactly as CloseWorld closes one.
        ///
        /// Descending id order stops a world opened over another (an overlay) before the world it
        /// covers. A world an OnStop opens during the sweep is closed in its turn, so the runner holds
        /// no world when this returns. The owner's teardown step: a runner destroyed with worlds open
        /// runs no OnStop for them.
        /// @pre Not inside Tick.
        void CloseAllWorlds();

        /// @brief Sets the hook told a world has closed.
        ///
        /// Called once per closed world with its id, after the world is erased — so the id no longer
        /// resolves — and in close order, for a holder of state keyed by world id to drop it. A scene
        /// InstallScene replaces closes no world and fires no call.
        /// @param hook  The closed hook, or an empty function to clear it.
        void SetWorldClosedHook(function<void(WorldInstanceId)> hook);

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

        /// @brief Resolves a world's interpolation fraction from its last tick; 0 for an unresolved id.
        ///
        /// The residual accumulator the render gather and View systems blend the last two ticks by,
        /// read live for a presentation pull that needs a world's own phase (an overlay presenting a
        /// world other than the one driving the frame's alpha).
        /// @param world  The world whose interpolation fraction is read.
        /// @return The world's LastAlpha, or 0 when the id resolves to nothing.
        [[nodiscard]] f32 ResolveAlpha(WorldInstanceId world) const;

        /// @brief Ticks every world's Sim phase at its own fixed rate, then its View phase, in id order.
        ///
        /// Serial on the render thread: for each started, unpaused world, folds the frame delta (times
        /// its net slew) into its clock, runs the accumulated fixed Sim steps then one View pass,
        /// driving the caller's per-step hooks. A world's steps stop at its MaxTicksPerFrame and, when
        /// set, its MaxSimMillisecondsPerFrame; transform history is recorded only after the steps
        /// interpolation reads (SimStepInfo::RecordsHistory). A paused or unstarted world resets its
        /// accumulator so resuming chases no backlog. Any world that runs no step this frame —
        /// paused, unstarted, or short of a whole tick — has its frame action edges cleared
        /// (ResetFrameActionEdges), so per-frame code reading its PlayerInput sees no stale edge.
        ///
        /// A system may open and close worlds from its own update: an open lands at once and first
        /// ticks next frame, a close is deferred to the end of the walk (see OpenWorld and
        /// CloseWorld). Not reentrant — a tick may not be driven from inside a tick.
        /// @param info  The frame delta, view-phase gate, and per-world tick hooks.
        /// @return What the tick observed across all worlds (for the input edge latch).
        WorldTickResult Tick(const WorldTickInfo& info);

        /// @brief Returns whether the runner is inside Tick — its world walk, or the close drain after it.
        ///
        /// What decides whether a CloseWorld is deferred or immediate, exposed for a caller that must
        /// know which of the two it is about to get.
        [[nodiscard]] bool IsTicking() const { return m_Ticking; }

        /// @brief Sets a world's explicit pause toggle, composing with any held PauseScopes.
        ///
        /// Forwards to the world's live SceneSimulation::SetPaused; a no-op for an unminted world or
        /// one whose scene has no simulation, which never ticks.
        /// @param world   The world to pause or resume.
        /// @param paused  True to pause, false to clear the explicit toggle.
        void SetWorldPaused(WorldInstanceId world, bool paused);

        /// @brief Returns whether a world's live simulation is paused (a held scope or the toggle).
        /// @param world  The world to query.
        /// @return False for an unminted world or one with no simulation.
        [[nodiscard]] bool IsWorldPaused(WorldInstanceId world) const;

        /// @brief Opens an RAII refcounted pause on a world, held for the returned scope's lifetime.
        /// @param world  The world to pause while the scope lives.
        /// @return The pause scope; inert (WorldPauseScope::IsHeld false) when the world is unminted or
        ///         has no simulation.
        [[nodiscard]] WorldPauseScope PauseScope(WorldInstanceId world);

        /// @brief Installs a freshly-loaded scene as an already-open world's scene, and returns it.
        ///
        /// The client-join seam: world #0 is opened as an empty join target, then the accepted level
        /// loads into a scene the runner takes ownership of here (replacing the empty placeholder), so
        /// the joined scene is a runner-owned world rather than a parallel one. The caller starts it
        /// once the install lands. A replaced scene whose simulation is started is stopped first
        /// (OnStop with the world's Stop context, as CloseWorld stops one), then retired and dropped;
        /// the world itself stays open, so the closed hook does not fire. The replaced simulation's pause
        /// (its held refs and explicit toggle) is carried onto the installed scene's simulation, so a
        /// pause held across the replacement survives it; a side with no simulation carries nothing.
        /// @param world  The open world to install the scene into.
        /// @param scene  The loaded scene the runner takes ownership of.
        /// @return The installed scene.
        Scene& InstallScene(WorldInstanceId world, Unique<Scene> scene);

        /// @brief Discovers the presented worlds' CaptureSurface components and drives them into the compositor.
        ///
        /// Iterates every **presented** world's scene (regardless of pause — pause is not what gates
        /// capture driving) for Renderer::CaptureSurface components, materializing each one's
        /// SceneCapture on first sight and registering it through @p info.Register, then pushing this
        /// frame's capture source. Requires the runner to have been given a context and asset manager
        /// whenever a presented world holds a capture surface.
        ///
        /// Materialization is paced and pooled. A surface first takes a released capture of its
        /// configuration from the runner's pool (see GetCapturePool); failing that, one is built new,
        /// at most @p info.MaxNewCaptures per pass — a surface past the budget is left for a later
        /// pass and not driven this one. A materialized capture returns to the pool when its surface
        /// is destroyed, so a world swap that tears down and rebuilds the same captures reuses them.
        ///
        /// A world @p info.IsPresented rejects is skipped whole: with no view showing it, nothing can
        /// sample a capture rendered from it, so the face render, its scene walk, and its view slot are
        /// all waste — and several live worlds is the ordinary state of a runner holding worlds warm,
        /// so the waste multiplies straight into the frame's view budget. Each already-materialized
        /// capture in a skipped world is re-armed (CaptureSurface::MarkDirty) instead, so a world that
        /// becomes presented again rebuilds its maps over the following frames rather than resuming
        /// from content captured before it went dark.
        ///
        /// A capture binds onto the first MaterialInstance of its sibling MeshRenderer's mesh. That
        /// instance belongs to the mesh asset and is shared by every entity drawing it, so on a
        /// surface's first drive the runner installs a clone of it as the entity's
        /// MeshRenderer::InstanceMaterials and binds the capture into the clone alone; see
        /// Renderer::CaptureSurface.
        /// @param info  The registration and presentation hooks this pass drives through.
        /// @return What the pass drove, skipped, and re-armed.
        WorldCaptureDriveResult DriveCaptureSurfaces(const WorldCaptureDriveInfo& info);

        /// @brief Returns the pool released scene captures return to, and capture surfaces draw from.
        ///
        /// Null on a runner given no context and asset manager, which materializes no capture.
        [[nodiscard]] Renderer::SceneCapturePool* GetCapturePool() const
        {
            return m_CapturePool.get();
        }

        /// @brief Returns the owned worlds in id order, for per-world presentation drives.
        [[nodiscard]] const vector<Unique<World>>& GetWorlds() const { return m_Worlds; }

        /// @brief Returns whether the runner holds any world.
        [[nodiscard]] bool HasWorlds() const { return !m_Worlds.empty(); }

    private:
        friend class WorldPauseScope;

        /// @brief Mints the next never-reused world id from the instance counter.
        [[nodiscard]] WorldInstanceId MintId();

        /// @brief Stops a world's started simulation and erases it, here and now.
        /// @param world  The world to close; an unminted or already-closed id is a no-op.
        void CloseWorldNow(WorldInstanceId world);

        /// @brief Runs each system's OnStop on a world's scene when its simulation is started.
        ///
        /// Builds the Stop context through the factory; a runner with none stops nothing rather than
        /// fabricating a context. Idempotent, since an already-stopped simulation runs no OnStop.
        /// @param world  The world the scene belongs to, named by the Stop context.
        /// @param scene  The world's live scene.
        void StopScene(WorldInstanceId world, Scene& scene);

        /// @brief Closes every world a deferred close queued, in issue order, until the queue empties.
        void DrainPendingCloses();

        /// @brief Whether a world is queued for a deferred close, and so takes no further phase.
        /// @param world  The world to test.
        [[nodiscard]] bool IsCloseQueued(WorldInstanceId world) const;

        /// @brief Re-arms every already-materialized capture in a world whose captures are suppressed.
        ///
        /// A capture frozen while its world is unpresented holds the scene as it was when the world went
        /// dark, so each one that has rendered is marked dirty and rebuilds its faces once the world is
        /// presented again. A capture that never materialized has nothing to re-arm.
        /// @param world  The skipped world whose capture surfaces are re-armed.
        /// @return How many surfaces were re-armed.
        static u32 ReArmCaptureSurfaces(const World& world);

        /// @brief Gives an unmaterialized capture surface its capture, from the pool or newly built.
        ///
        /// A pooled capture of the surface's configuration is taken first and costs no build; past
        /// that, a capture is built only while @p built is under @p maxNew.
        /// @param surface  The surface to materialize; it holds no capture yet.
        /// @param maxNew   The most captures this pass may build new.
        /// @param built    The captures this pass has built so far; incremented on a build.
        /// @param result   The pass's tally, counting the build or the reuse.
        /// @return True when the surface now holds a capture; false when the build budget is spent.
        bool MaterializeCapture(const Renderer::CaptureSurface& surface, u32 maxNew, u32& built,
                                WorldCaptureDriveResult& result);

        /// @brief Returns a world's live simulation, or null when it is unminted or has none.
        /// @param world  The world to resolve.
        [[nodiscard]] SceneSimulation* ResolveSimulation(WorldInstanceId world) const;

        /// @brief Releases one held pause on a world's live simulation (a WorldPauseScope drop).
        ///
        /// A no-op when the world has closed or holds no simulation.
        /// @param world  The world the scope paused.
        void ReleasePause(WorldInstanceId world);

        /// @brief The type registry every world's scene is created against.
        TypeRegistry* m_Types = nullptr;
        /// @brief The system registry a world's simulation is built from.
        SystemRegistry* m_Systems = nullptr;
        /// @brief The asset manager cooked-level worlds spawn through; null on a device-free runner.
        AssetManager* m_Assets = nullptr;
        /// @brief The render context capture-surface discovery uses; null on a device-free runner.
        Renderer::Context* m_Context = nullptr;

        /// @brief Released scene captures held for reuse; null on a device-free runner.
        ///
        /// Declared ahead of m_Worlds so it outlives them: a capture surface destroyed with its world
        /// returns its capture here. Shared because each surface holds it weakly, so a surface that
        /// outlives the runner drops its capture instead of returning it to a dead pool.
        Ref<Renderer::SceneCapturePool> m_CapturePool;

        /// @brief The owned worlds, in ascending id (open) order.
        vector<Unique<World>> m_Worlds;

        /// @brief Builds every context the worlds receive; unset leaves a closed world's OnStop unrun.
        SystemContextFactory m_ContextFactory;

        /// @brief Told a scene is about to be destroyed; unset tells no one.
        function<void(const Scene&)> m_SceneRetiringHook;

        /// @brief Told a world has closed, after it is erased; unset tells no one.
        function<void(WorldInstanceId)> m_WorldClosedHook;

        /// @brief Worlds a close issued inside Tick queued, in issue order; drained after the walk.
        vector<WorldInstanceId> m_PendingCloses;

        /// @brief True from Tick's entry until its close drain finishes; what makes a close deferred.
        bool m_Ticking = false;

        /// @brief The instance counter minting world ids; never reused, so a stale id resolves to nothing.
        u64 m_NextId = 1;
    };
}

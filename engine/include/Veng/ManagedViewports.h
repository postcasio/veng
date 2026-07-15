#pragma once

#include <Veng/Veng.h>
#include <Veng/World.h>
#include <Veng/Renderer/DynamicResolution.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class AssetManager;
    class InputRouter;
    class WorldRunner;
}

namespace Veng::Renderer
{
    class Context;
    class ViewportCompositor;
}

namespace Veng
{
    /// @brief Configuration for one engine-owned managed viewport, naming its world and seat.
    ///
    /// An element of ApplicationInfo::ManagedViewports (or the singular ApplicationInfo::ManagedViewport)
    /// has Application construct, register, and drive one Presented viewport whose region follows its
    /// normalized Layout across resizes. The viewport presents the world named by World, resolving that
    /// world's per-seat camera (Viewer) through the WorldRunner each frame — a presentation-side pull, so
    /// the runner never learns a viewport exists. An invalid World (the default) leaves the viewport for
    /// the game to drive itself through ManagedViewportSet::Get(n)->SetViewState. The editor leaves the
    /// managed set unset.
    struct ManagedViewportInfo
    {
        /// @brief Render extent the managed viewport's SceneRenderer is sized to.
        ///
        /// Defaults to {} so the viewport tracks the window: its region follows the render-target
        /// extent (the swapchain framebuffer extent windowed, HeadlessExtent headless) and
        /// resizes with the swapchain, covering the whole window. A non-zero value pins a fixed
        /// render resolution that does not track resize.
        uvec2 Extent = {};
        /// @brief Output color format; resolved to Context::GetOutputFormat() when Undefined.
        Renderer::Format ColorFormat = Renderer::Format::Undefined;
        /// @brief Initial topology and sizing knobs for the managed viewport's SceneRenderer.
        Renderer::SceneRendererSettings Settings;
        /// @brief Initial render-resolution multiplier on the region extent (see Viewport).
        ///
        /// The managed viewport renders at its region extent times this; (0,1] renders below the
        /// window for dynamic resolution scaling, >1 supersamples. Must be > 0.
        f32 RenderScale = 1.0f;
        /// @brief Caps the managed viewport's allocation to this fraction of the window's pixels.
        ///
        /// The HiDPI supersample budget threaded into the viewport's ViewportInfo (see
        /// Renderer::ViewportInfo::MaxAllocationScale). The managed viewport tracks the full
        /// swapchain framebuffer extent — 2× the logical window on a HiDPI display — so 0.5 there
        /// brings the allocation back to logical-point resolution. 1.0 (the default) allocates at
        /// the full backing extent. Must be > 0.
        f32 MaxAllocationScale = 1.0f;
        /// @brief Enables automatic render-scale control on the managed viewport when set.
        ///
        /// The viewport eases its RenderScale toward this budget from measured GPU frame time
        /// each frame (see Viewport::SetDynamicResolution), rendering into a sub-rect of the fixed
        /// allocation; inert on a device without GPU timing. Unset leaves the scale fixed at
        /// RenderScale.
        optional<Renderer::DynamicResolutionSettings> DynamicResolution;
        /// @brief Where in the window this managed viewport sits; resolved to pixels per resize.
        ///
        /// Meaningful only when Extent tracks the window (the default empty Extent): the
        /// ViewportCompositor resolves the pixel region as round(Layout · render extent) at
        /// construction and on every swapchain resize. The default full-window Layout is
        /// byte-identical to a single viewport covering the whole window. Ignored when Extent pins a
        /// fixed render resolution.
        Renderer::ViewportLayout Layout;
        /// @brief The world this viewport presents; invalid (the default) leaves it game-driven.
        ///
        /// When valid, the per-frame camera pull resolves this world (WorldRunner::ResolveWorld) and
        /// pushes its scene through the viewport; a world closed at runtime resolves to no camera and
        /// the viewport renders a cleared target (inert, never a dangling read). When invalid, the
        /// engine pushes nothing — the game drives the viewport itself through SetViewState.
        WorldInstanceId World;
        /// @brief Optional seat in World whose camera the engine resolves and pushes into this viewport.
        ///
        /// When set (and World is valid), the per-frame camera pull resolves this seat's CameraView
        /// (WorldRunner::ResolveCameraView) in World and pushes it into this viewport, generalizing
        /// the primary-camera push to a named seat. Entity::Null (the default) takes World's scene
        /// primary camera.
        Entity Viewer = Entity::Null;
    };

    /// @brief The engine's managed-viewport policy: owns the set, resolves layouts, pulls cameras.
    ///
    /// The collaborator Application delegates its managed-viewport policy to. It owns each managed
    /// ManagedViewport (a Presented Viewport plus the ManagedViewportInfo it was built from),
    /// registers them into the ViewportCompositor (which resolves each Layout-carrying viewport's
    /// region and UI scale on swapchain resize), and applies a deferred reconfigure at a safe point.
    /// Each frame it pulls each viewport's camera from the WorldRunner by the viewport's
    /// { World, Viewer } binding and pushes it — a one-directional gameplay→render bridge; the runner
    /// holds no pointer back. Presentation-only: it never reaches into gameplay beyond the
    /// ResolveCameraView query.
    ///
    /// The viewports self-unregister from the compositor on drop, so clearing the set (or destroying
    /// it) is the whole of cleanup. It borrows the context, asset manager, compositor, and router; all
    /// must outlive it, and it must be destroyed before any of them.
    class ManagedViewportSet
    {
    public:
        /// @brief Constructs an empty set over the borrowed services.
        /// @param context     The render context Viewports are created against; must outlive the set.
        /// @param assets      The asset manager Viewports load their shaders through; must outlive the set.
        /// @param compositor  The compositor the viewports register into; must outlive the set.
        /// @param router      The input router viewport↔seat associations are made through; must outlive the set.
        ManagedViewportSet(Renderer::Context& context, AssetManager& assets,
                           Renderer::ViewportCompositor& compositor, InputRouter& router);

        /// @brief Clears the set, self-unregistering each viewport and its router association.
        ~ManagedViewportSet();

        ManagedViewportSet(const ManagedViewportSet&) = delete;
        ManagedViewportSet& operator=(const ManagedViewportSet&) = delete;

        /// @brief Returns the number of managed viewports (index 0 is the primary).
        [[nodiscard]] usize GetCount() const { return m_Viewports.size(); }

        /// @brief Returns whether the set holds no managed viewport.
        [[nodiscard]] bool Empty() const { return m_Viewports.empty(); }

        /// @brief Returns the managed viewport at an index, or null when out of range.
        ///
        /// Index 0 is the primary. A game pushes its scene and camera through the returned viewport's
        /// SetViewState each frame (or names a World/Viewer for the engine to resolve).
        /// @param index  The managed viewport index.
        /// @return The managed viewport, or nullptr when index is out of range.
        [[nodiscard]] Renderer::Viewport* Get(usize index) const;

        /// @brief Builds the set from a list of infos, registering each viewport in order.
        ///
        /// Replaces the current set: drops the prior viewports (each self-unregisters, its router
        /// association cleared), constructs one Presented viewport per info at its Layout-resolved
        /// region (index 0 the primary), applies its dynamic-resolution opt-in, registers it, and
        /// associates a bound Viewer with the router. Delegates region + UI-scale resolution to the
        /// compositor so no viewport needs a per-frame re-apply.
        /// @param infos  The managed viewport infos to build from.
        void Build(std::span<const ManagedViewportInfo> infos);

        /// @brief Records a reconfigure applied at the next ApplyPendingReconfigure (top of frame).
        ///
        /// Mirrors the SetRegion resize debounce: the rebuild constructs/drops viewports, which must
        /// not run mid-drive. Requires a managed viewport to have been built.
        /// @param infos  The new managed set; each info's Layout, World, Viewer, and render knobs apply.
        void Reconfigure(std::span<const ManagedViewportInfo> infos);

        /// @brief Applies pending reconfigure, world rebinds, and ready present-on-ready swaps.
        ///
        /// Called at the top of a frame, outside any Scene/viewport-list iteration. In order: applies a
        /// recorded reconfigure (rebuilds the set), applies each unconditional world rebind as a
        /// complete rebind (detaching the departed world's engine-driven overlay documents from the
        /// viewport and re-resolving its seat in the destination), then evaluates each present-on-ready
        /// rebind — applying it once its destination is ready, dropping it if the destination closed, or
        /// abandoning it (surfaced through GetAbandonedPresentWorld) once it exceeds the ready timeout.
        /// The runner resolves the departed and destination worlds; @p delta advances each pending
        /// present-on-ready request's wait clock.
        /// @param runner  The runner the departed/destination worlds resolve through.
        /// @param delta   The wall-clock frame delta in seconds, accruing toward the ready timeout.
        void ApplyPendingReconfigure(WorldRunner& runner, f32 delta);

        /// @brief Returns the world a managed viewport currently presents (its applied binding).
        ///
        /// The applied binding: what the per-frame camera pull resolves for this viewport now. An
        /// in-flight rebind (deferred or present-on-ready) is not reflected here until it applies — read
        /// GetPendingViewportWorld for that. An out-of-range index returns the invalid handle.
        /// @param index  The managed viewport index (0 the primary).
        /// @return The presented world's handle, or an invalid handle when index is out of range.
        [[nodiscard]] WorldInstanceId GetViewportWorld(usize index) const;

        /// @brief Returns the destination of a viewport's in-flight rebind, or nullopt when none is pending.
        ///
        /// The world a recorded rebind (deferred RebindWorld or present-on-ready RebindWorldWhenReady)
        /// will re-point this viewport to once it applies. A present-on-ready request reads here for its
        /// whole wait, so a caller treats a pending destination as presented (never reaping it in its
        /// own rebind gap); it clears when the rebind applies, is superseded, is dropped (destination
        /// closed), or is abandoned (timed out). An out-of-range index has no pending rebind.
        /// @param index  The managed viewport index (0 the primary).
        /// @return The pending destination world, or nullopt when no rebind is in flight for the index.
        [[nodiscard]] optional<WorldInstanceId> GetPendingViewportWorld(usize index) const;

        /// @brief Returns the destination a present-on-ready rebind abandoned on timeout, else invalid.
        ///
        /// The self-contained failure surface of RebindWorldWhenReady: when a present-on-ready request
        /// exceeds its ready timeout it is abandoned (the viewport keeps its current world) and its
        /// destination is recorded here, so a caller can react rather than presenting the old world
        /// forever. The record clears when a later rebind of the same index is recorded (it supersedes).
        /// @param index  The managed viewport index (0 the primary).
        /// @return The abandoned destination world, or an invalid handle when none was abandoned.
        [[nodiscard]] WorldInstanceId GetAbandonedPresentWorld(usize index) const;

        /// @brief Binds a managed viewport to the world it presents.
        ///
        /// Sets the viewport's ManagedViewportInfo::World; the per-frame pull then resolves this world
        /// for its camera. Used at bootstrap to bind managed viewport #0 to the opened world #0, and by
        /// a game rebinding a viewport to another open world at runtime. A no-op for an out-of-range index.
        /// @param index  The managed viewport index.
        /// @param world  The world the viewport presents.
        void SetViewportWorld(usize index, WorldInstanceId world);

        /// @brief Records a world rebind applied at the next ApplyPendingReconfigure (top of frame).
        ///
        /// The deferred runtime counterpart to SetViewportWorld: re-points the indexed managed viewport
        /// at a different world, applied at the same safe point a reconfigure uses (never mid-drive), so
        /// the viewport's next camera pull resolves the new world while keeping its render target and
        /// bound Viewer. A no-op for an out-of-range index (dropped at apply). A pending rebind of a
        /// viewport a pending reconfigure also replaces is applied after the reconfigure, on the rebuilt
        /// set. Recording a rebind supersedes any earlier pending rebind of the same index — the plain
        /// deferred one and a present-on-ready one alike — so the last request for an index wins.
        /// @param index  The managed viewport index (0 the primary).
        /// @param world  The world the viewport presents next.
        void RebindWorld(usize index, WorldInstanceId world);

        /// @brief Records a present-on-ready rebind: applied at the top of frame once the world is ready.
        ///
        /// The complete-scene-change counterpart to RebindWorld: the viewport keeps presenting its
        /// current world until the destination is ready (resolves, its scene is installed, its
        /// simulation started, its spawn residency batch resident, and its clock has ticked at least
        /// once), then the rebind applies in one frame — the departed world's overlays detach and the
        /// seat re-resolves atomically, with no empty-world frame between. Superseded by any later
        /// rebind of the same index (last wins), dropped if the destination closes before it is ready,
        /// and abandoned once it exceeds the ready timeout (surfaced through GetAbandonedPresentWorld),
        /// so a destination that never readies does not strand the viewport presenting the old world
        /// forever. The pending destination is observable through GetPendingViewportWorld. A no-op for an
        /// out-of-range index (dropped at apply).
        /// @param index  The managed viewport index (0 the primary).
        /// @param world  The world to present once it is ready.
        void RebindWorldWhenReady(usize index, WorldInstanceId world);

        /// @brief Registers a non-owning presented viewport bound to a world, driven beside the set.
        ///
        /// The camera pull that serves a Presented viewport opened at runtime over the indexed managed
        /// set (an overlay): the caller owns the viewport (and registers it with the compositor for
        /// render and layout tracking), and this binds it to a world so PushViews resolves and pushes
        /// its camera each frame through the identical { World, Viewer } path a managed viewport uses,
        /// pulling the world's own interpolation fraction. The set never owns the viewport — the caller
        /// unregisters the binding (UnregisterBoundViewport) before dropping the viewport. The bound
        /// viewports are not indexed and Get / Build / Reconfigure never touch them.
        /// @param viewport  The caller-owned presented viewport whose camera the set pushes.
        /// @param world     The world the viewport presents.
        /// @param viewer    The seat in @p world whose camera to resolve, or Entity::Null for the primary.
        /// @param knobs     The per-frame tone/bloom/environment knobs carried into the push.
        void RegisterBoundViewport(Renderer::Viewport& viewport, WorldInstanceId world,
                                   Entity viewer, const Renderer::ViewState& knobs);

        /// @brief Removes a bound viewport's camera-pull binding; a no-op if it is not bound.
        ///
        /// The counterpart to RegisterBoundViewport, called before the caller drops the viewport so no
        /// stale pointer lingers in the pull. Does not touch the compositor drive-list or the router.
        /// @param viewport  The bound viewport whose binding to remove.
        void UnregisterBoundViewport(const Renderer::Viewport& viewport);

        /// @brief Pulls each managed and bound viewport's camera from the runner and pushes it, once per frame.
        ///
        /// For each managed viewport naming a valid World: resolves the world through @p runner and
        /// pushes its scene (a bound Viewer resolves that seat's camera through ResolveCameraView at
        /// the viewport's aspect; Entity::Null takes the scene primary). A viewport whose World was
        /// closed pushes a null-scene ViewState (a cleared target, inert). A managed viewport with an
        /// invalid World is left untouched for the game to drive. Each registered bound viewport
        /// (RegisterBoundViewport) is pushed the same way with its own carried knobs and its world's
        /// own interpolation fraction. The runner never learns a viewport asked.
        /// @param runner  The world runner cameras are resolved through.
        /// @param knobs   The per-frame tone/bloom/environment view knobs carried into each managed push.
        /// @param delta   Frame delta in seconds, forwarded to the renderer.
        /// @param alpha   The fixed-timestep interpolation fraction for the managed viewports.
        void PushViews(WorldRunner& runner, const Renderer::ViewState& knobs, f32 delta, f32 alpha);

        /// @brief Clears the set, dropping every managed viewport and its router association.
        void Clear();

    private:
        /// @brief One engine-owned managed viewport plus the info it was built from.
        struct ManagedViewport
        {
            /// @brief The owned, registered Presented viewport.
            Unique<Renderer::Viewport> Viewport;
            /// @brief The info this viewport was constructed from (Layout, World, Viewer, render knobs).
            ManagedViewportInfo Info;
        };

        /// @brief A non-owning presented viewport bound to a world, pushed beside the indexed set.
        struct BoundViewport
        {
            /// @brief The caller-owned viewport whose camera the set pushes; never owned here.
            Renderer::Viewport* Viewport = nullptr;
            /// @brief The world this viewport presents.
            WorldInstanceId World;
            /// @brief The seat in World whose camera to resolve, or Entity::Null for the scene primary.
            Entity Viewer = Entity::Null;
            /// @brief The per-frame tone/bloom/environment knobs carried into the push.
            Renderer::ViewState Knobs;
        };

        /// @brief Pushes one viewport's resolved world source through the runner by its { World, Viewer } binding.
        /// @param viewport  The viewport to push into.
        /// @param world     The world the viewport presents.
        /// @param viewer    The seat whose camera to resolve, or Entity::Null for the scene primary.
        /// @param runner    The world runner the world and camera resolve through.
        /// @param knobs     The per-frame view knobs carried into the push.
        /// @param delta     Frame delta in seconds.
        /// @param alpha     The interpolation fraction.
        void PushViewportView(Renderer::Viewport& viewport, WorldInstanceId world, Entity viewer,
                              WorldRunner& runner, const Renderer::ViewState& knobs, f32 delta,
                              f32 alpha) const;

        /// @brief Applies a world rebind to a viewport: departed-overlay detach + seat re-resolution.
        ///
        /// The complete rebind at the apply point: detaches every engine-driven overlay of the departed
        /// world (when it still resolves and differs from the destination) from the viewport, re-points
        /// the viewport's seat association (and the cursor seat when the departed association owned it)
        /// to the destination's resolved seat, resets Info.World and Info.Viewer, and leaves focus
        /// policy untouched.
        /// @param index   The managed viewport index; out of range is a no-op.
        /// @param world   The destination world the viewport presents after the rebind.
        /// @param runner  The runner the departed/destination worlds resolve through.
        void ApplyCompleteRebind(usize index, WorldInstanceId world, WorldRunner& runner);

        /// @brief Drops any pending rebind (deferred, present-on-ready, or abandoned) for an index.
        ///
        /// The supersession sweep a newly recorded rebind runs so the last request for an index wins and
        /// no stale abandonment record lingers past it.
        /// @param index  The managed viewport index whose pending state is cleared.
        void SupersedePending(usize index);

        /// @brief The render context Viewports are created against.
        Renderer::Context& m_Context;
        /// @brief The asset manager Viewports load their shaders through.
        AssetManager& m_Assets;
        /// @brief The compositor the viewports register into and resolve their layouts through.
        Renderer::ViewportCompositor& m_Compositor;
        /// @brief The input router viewport↔seat associations are made through.
        InputRouter& m_Router;

        /// @brief The managed viewports in order; index 0 is the primary.
        vector<ManagedViewport> m_Viewports;

        /// @brief The non-owning bound viewports (overlays) pushed beside the indexed set.
        vector<BoundViewport> m_Bound;

        /// @brief A pending reconfigure, applied at the next ApplyPendingReconfigure; nullopt when none.
        optional<vector<ManagedViewportInfo>> m_PendingReconfigure;

        /// @brief One deferred world rebind: the viewport index and the world to re-point it at.
        struct PendingRebind
        {
            /// @brief The managed viewport index to rebind.
            usize Index = 0;
            /// @brief The world the viewport presents after the rebind applies.
            WorldInstanceId World;
        };

        /// @brief Pending world rebinds applied in order at the next ApplyPendingReconfigure.
        vector<PendingRebind> m_PendingRebinds;

        /// @brief One conditional present-on-ready rebind: its target index, world, and wait clock.
        struct PendingReadyRebind
        {
            /// @brief The managed viewport index to rebind once the world is ready.
            usize Index = 0;
            /// @brief The destination world, applied only once IsWorldPresentable reports it ready.
            WorldInstanceId World;
            /// @brief Seconds spent waiting for readiness, accrued toward the ready timeout.
            f32 Waited = 0.0f;
        };

        /// @brief Present-on-ready rebinds, held until their destination readies, times out, or closes.
        vector<PendingReadyRebind> m_PendingReadyRebinds;

        /// @brief One abandoned present-on-ready destination: the index it targeted and its world.
        struct AbandonedPresent
        {
            /// @brief The managed viewport index whose present-on-ready request was abandoned.
            usize Index = 0;
            /// @brief The destination world the request timed out waiting to present.
            WorldInstanceId World;
        };

        /// @brief Present-on-ready destinations abandoned on timeout, cleared when the index is superseded.
        vector<AbandonedPresent> m_AbandonedPresents;

        /// @brief Seconds a present-on-ready rebind waits for readiness before it is abandoned.
        static constexpr f32 PresentReadyTimeoutSeconds = 15.0f;
    };
}

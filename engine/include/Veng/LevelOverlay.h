#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

namespace Veng
{
    class Application;
    class Scene;
    class SeatFocusScope;
    class InputMappingContext;
}

namespace Veng::Renderer
{
    class Viewport;
}

namespace Veng
{
    /// @brief Parameters for opening a level as a secondary, simulated overlay.
    ///
    /// Passed to LevelOverlay::Open. Only Source is required; every other field has a
    /// self-describing default that yields a full-window, input-taking, non-pausing overlay
    /// with no populate step.
    struct LevelOverlayInfo
    {
        /// @brief The level to open as an overlay; must be resident (see WaitForResidency).
        AssetHandle<Level> Source;
        /// @brief The viewport region the overlay renders into; a zero extent means the full window.
        ///
        /// A full-window overlay carries a Layout, so the compositor re-fits its region across
        /// swapchain resizes; a fixed sub-region (a picture-in-picture) is placed as given and does
        /// not track.
        Renderer::ViewportRegion Region;
        /// @brief The seat whose input the overlay suspends beneath it.
        ///
        /// Entity::Null (the default) resolves to the router's current cursor seat at open time —
        /// the covered world's seat, or a lower overlay whose own Open reassigned it, so a stack of
        /// overlays each suspends the one beneath.
        Entity SuspendSeat = Entity::Null;
        /// @brief The world the overlay covers and pauses for its lifetime; invalid pauses nothing.
        ///
        /// When valid, the overlay holds a WorldRunner::PauseScope on this world while it lives, so
        /// the covered world stops simulating beneath the modal. Because the pause is a refcount,
        /// stacked overlays over one world nest correctly and an explicit game pause is not clobbered.
        /// Invalid (the default) leaves every world simulating — input focus and simulation pause are
        /// separate knobs, and taking the overlay's seat suspends the covered seat's input regardless.
        WorldInstanceId CoveredWorld;
        /// @brief When set, Open loads the source to residency and blocks until its spawn is resident.
        ///
        /// The convenience path, accepting the first-open hitch. Unset (the default), the caller is
        /// responsible for holding the source (and its dependencies) resident before opening.
        bool WaitForResidency = false;
        /// @brief Invoked once with the fresh overlay scene, after load and before the simulation starts.
        ///
        /// The one seam through which host state enters the overlay: the engine calls it exactly
        /// once with the loaded-but-not-started scene and does not inspect what it attaches. Empty
        /// (the default) opens the scene with only its authored content.
        function<void(Scene&)> Populate;
    };

    /// @brief An RAII handle that opens a whole Level as a secondary, simulated overlay.
    ///
    /// A thin preset over WorldRunner::OpenWorld: opening an overlay opens an owned world (its own
    /// scene, systems, and HUD, ticked by the runner like any world) and applies an overlay policy —
    /// register a Presented viewport on top (composited over the covered world, its camera pulled by
    /// the managed-viewport presentation path and its region re-fit on resize by the compositor),
    /// hand the cursor seat and the covered seat's focus off to the overlay's own seat, and hold a
    /// WorldRunner::PauseScope on the caller-named covered world. The runner ticks the overlay's
    /// simulation and the engine pushes its camera each frame, so there is no per-frame game call.
    ///
    /// The handle is move-only. Dropping it (or calling Close) unwinds the policy LIFO — releases the
    /// pause scope, pops the focus scope, restores the cursor seat and clears the pointer association,
    /// unregisters and drops the viewport, and closes the world — restoring every router / cursor-seat
    /// value to the state it captured at open. Overlays stack: a second opened over the first nests
    /// through the cursor-seat handoff and the focus stack, and the handles must be dropped in reverse
    /// open order (LIFO), the discipline any scope stack requires. The pause refcount keeps the
    /// covered world paused until the last overlay over it closes.
    class LevelOverlay
    {
    public:
        /// @brief Opens @p info's level as a secondary overlay over @p app's running frame.
        ///
        /// Sequences: open an owned world through the runner (spawning the source, running
        /// info.Populate against the fresh scene, not started) → create and register a Presented
        /// viewport for the region and bind it to the world for the per-frame camera pull → route
        /// input (pointer association to the overlay seat, cursor-seat handoff, and a viewport-less
        /// focus scope suspending info.SuspendSeat) → hold a PauseScope on info.CoveredWorld when
        /// valid → start the world's simulation. The returned handle is move-only.
        /// @param app   The running application whose services (assets, systems, router, runner,
        ///              compositor, managed set) the overlay composes.
        /// @param info  The overlay parameters; info.Source must be resident unless
        ///              info.WaitForResidency is set.
        /// @return The opened overlay handle.
        /// @pre info.Source is resident, or info.WaitForResidency is set.
        [[nodiscard]] static LevelOverlay Open(Application& app, const LevelOverlayInfo& info);

        /// @brief Tears the overlay down if still open (see Close).
        ~LevelOverlay();

        LevelOverlay(const LevelOverlay&) = delete;
        LevelOverlay& operator=(const LevelOverlay&) = delete;

        /// @brief Moves the overlay, leaving @p other inert (its drop tears nothing down).
        LevelOverlay(LevelOverlay&& other) noexcept;

        /// @brief Moves the overlay, tearing down any overlay this handle currently holds first.
        LevelOverlay& operator=(LevelOverlay&& other) noexcept;

        /// @brief Closes the overlay now, restoring the captured router / cursor-seat / pause state.
        ///
        /// Reverses the open in lifetime order: leaves the overlay stack, releases the covered-world
        /// pause scope, pops the focus scope, restores the cursor seat and clears the viewport's
        /// pointer association (while the viewport is still alive), unregisters and drops the
        /// viewport, then closes the world (stopping its simulation and dropping its scene).
        /// Idempotent — a second call, or a call on a moved-from handle, does nothing.
        void Close();

        /// @brief Returns whether this handle currently holds an open overlay.
        [[nodiscard]] bool IsOpen() const { return m_App != nullptr; }

        /// @brief Returns the overlay's scene, for reading results back before close.
        /// @pre IsOpen().
        [[nodiscard]] Scene& GetScene() const;

        /// @brief Returns the overlay's Presented viewport.
        /// @pre IsOpen().
        [[nodiscard]] Renderer::Viewport& GetViewport() const;

        /// @brief Returns the overlay's world handle, for resolving it through the runner.
        [[nodiscard]] WorldInstanceId GetWorld() const { return m_World; }

        /// @brief Returns the overlay's per-frame view knobs, seeded from the level at open.
        ///
        /// The photometric half of the overlay's ViewState (exposure, bloom, SSR, …), mapped from the
        /// level's LevelRenderSettings at open. The engine carries the knobs captured at open into
        /// each per-frame push, so this getter reads the seed; retuning them takes a re-open.
        [[nodiscard]] const Renderer::ViewState& GetViewState() const { return m_ViewKnobs; }

        /// @brief Returns the overlay's own input seat (its Viewer entity), or Entity::Null if none.
        [[nodiscard]] Entity GetSeat() const { return m_OverlaySeat; }

    private:
        LevelOverlay() = default;

        /// @brief The application the overlay composes; null on a moved-from or closed handle.
        Application* m_App = nullptr;
        /// @brief The owned overlay world opened through the runner; closed in teardown.
        WorldInstanceId m_World;
        /// @brief The Presented viewport the overlay renders into; dropped after its bindings clear.
        Unique<Renderer::Viewport> m_Viewport;
        /// @brief The focus scope suspending the layer beneath; popped before the cursor-seat restore.
        Unique<SeatFocusScope> m_Suspend;
        /// @brief The engine-owned empty context the focus scope swaps the suspended seat's input to.
        AssetHandle<InputMappingContext> m_SuspendContext;
        /// @brief The refcounted pause held on the covered world; inert when none was named.
        WorldPauseScope m_PauseScope;
        /// @brief The overlay level's render knobs, seeding the topology and the view knobs at open.
        LevelRenderSettings m_Render;
        /// @brief The per-frame view knobs the engine carries into each push, seeded at open.
        Renderer::ViewState m_ViewKnobs;
        /// @brief The overlay's own seat, taken as the cursor seat and the pointer-routing target.
        Entity m_OverlaySeat = Entity::Null;
        /// @brief The cursor seat observed at open, restored on close.
        Entity m_PriorCursorSeat = Entity::Null;
    };
}

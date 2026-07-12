#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/World.h>

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
        /// A full-window overlay tracks the framebuffer extent across resizes (Update re-applies
        /// it); a fixed sub-region (a picture-in-picture) is placed as given and does not track.
        Renderer::ViewportRegion Region;
        /// @brief The seat whose input the overlay suspends beneath it.
        ///
        /// Entity::Null (the default) resolves to the router's current cursor seat at open time —
        /// the primary seat, or a lower overlay whose own Open reassigned it, so a stack of overlays
        /// each suspends the one beneath.
        Entity SuspendSeat = Entity::Null;
        /// @brief When set, also freezes the primary world's simulation for the overlay's lifetime.
        ///
        /// Input focus and simulation pause are separate knobs: taking the overlay's seat always
        /// suspends the primary world's input, but the primary keeps simulating unless this is set.
        /// The pre-open pause state is captured and restored on close, so stacking and a
        /// game-paused primary both survive.
        bool PausePrimarySim = false;
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
    /// Composes the delivered Level / Viewport / InputRouter seams into "open a level over the
    /// running one — its own scene, systems, HUD, and input seat, simulated concurrently, dismissed
    /// on drop". Open loads the level into a fresh scene, runs the populate hook, creates a Presented
    /// viewport (registered last, so it composites on top), routes input to the overlay's own seat
    /// (pointer association, cursor-seat handoff, and a focus-scope suspension of the layer beneath),
    /// optionally freezes the primary simulation, registers the scene as an engine-ticked simulation,
    /// and starts it. The engine ticks the overlay's simulation and scopes its pointer to the
    /// overlay's viewport; Update re-applies the region and pushes the view each frame; the overlay's
    /// own viewport drives its scene's GuiOverlay HUD.
    ///
    /// The handle is move-only. Dropping it (or calling Close) tears the overlay down in lifetime
    /// order and restores every router / cursor-seat / world-pause / drive-list value to the state
    /// it captured at open, leaving them byte-restored. Overlays stack: a second opened over the
    /// first nests through the cursor-seat handoff and the focus stack, and the handles must be
    /// dropped in reverse open order (LIFO), the discipline any scope stack requires.
    class LevelOverlay
    {
    public:
        /// @brief Opens @p info's level as a secondary overlay over @p app's running frame.
        ///
        /// Sequences: load the source into a fresh scene and simulation (not started) → run
        /// info.Populate against the scene → create and register a Presented viewport for the region →
        /// register the scene as an engine-ticked simulation and push its initial view → route input
        /// (pointer association to the overlay seat, cursor-seat handoff, and a viewport-less focus
        /// scope suspending info.SuspendSeat) → optionally freeze the primary simulation → start the
        /// scene's simulation. The returned handle is safe to Update.
        /// @param app   The running application whose services (assets, systems, router, context,
        ///              drive-list, world-pause) the overlay composes.
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

        /// @brief Re-applies the region and pushes the view for one frame.
        ///
        /// Pushes the overlay's resolved camera into the viewport, re-applying the region against the
        /// current framebuffer extent so a full-window overlay tracks resizes. The engine owns the
        /// simulation tick (the overlay is a registered sim), so this no longer ticks it. The
        /// viewport's own render drives the scene's GuiOverlay HUD; no explicit UI step is needed. A
        /// no-op on a moved-from or closed handle.
        /// @param delta  Frame delta in seconds.
        void Update(f32 delta);

        /// @brief Closes the overlay now, restoring the captured router / pause / drive-list state.
        ///
        /// Reverses the open in lifetime order: stop the simulation, restore the observed world-pause
        /// value (if it was paused), pop the focus scope, restore the cursor seat and clear the
        /// viewport's pointer association (while the viewport is still alive), then drop the viewport.
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

        /// @brief Returns the overlay's persistent per-frame view knobs, editable in place.
        ///
        /// The photometric half of the overlay's `ViewState` (exposure, bloom, SSR, …), seeded from
        /// the level's `LevelRenderSettings` at open and pushed each `Update` — the overlay's analogue
        /// of `Application::GetWorldViewState()`. Edit it to retune the overlay's view (a debug
        /// settings panel), and the change persists because `Update` pushes this same instance rather
        /// than re-deriving from the level settings each frame. `Update` overrides only the frame's
        /// world / camera / delta / interpolation alpha on top of these knobs.
        [[nodiscard]] Renderer::ViewState& GetViewState() { return m_ViewKnobs; }

        /// @brief Returns the overlay's persistent per-frame view knobs (read-only).
        [[nodiscard]] const Renderer::ViewState& GetViewState() const { return m_ViewKnobs; }

        /// @brief Returns the overlay's own input seat (its Viewer entity), or Entity::Null if none.
        [[nodiscard]] Entity GetSeat() const { return m_OverlaySeat; }

    private:
        LevelOverlay() = default;

        /// @brief The application the overlay composes; null on a moved-from or closed handle.
        Application* m_App = nullptr;
        /// @brief The loaded overlay scene and its (waited-on) residency batch.
        LevelInstance m_Instance;
        /// @brief The Presented viewport the overlay renders into; dropped last in teardown.
        Unique<Renderer::Viewport> m_Viewport;
        /// @brief The focus scope suspending the layer beneath; popped before the cursor-seat restore.
        Unique<SeatFocusScope> m_Suspend;
        /// @brief The engine-owned empty context the focus scope swaps the suspended seat's input to.
        AssetHandle<InputMappingContext> m_SuspendContext;
        /// @brief The overlay level's render knobs, seeding the topology and the view knobs at open.
        LevelRenderSettings m_Render;
        /// @brief The persistent per-frame view knobs pushed each Update, editable via GetViewState.
        Renderer::ViewState m_ViewKnobs;
        /// @brief The current viewport region; recomputed from the framebuffer extent when tracking.
        Renderer::ViewportRegion m_Region;
        /// @brief The overlay's own seat, taken as the cursor seat and the pointer-routing target.
        Entity m_OverlaySeat = Entity::Null;
        /// @brief The cursor seat observed at open, restored on close.
        Entity m_PriorCursorSeat = Entity::Null;
        /// @brief Whether this overlay froze the primary simulation (so close restores the pause).
        bool m_PausePrimarySim = false;
        /// @brief The world-pause value observed at open, restored on close when PausePrimarySim.
        bool m_PriorPaused = false;
        /// @brief The world PausePrimarySim froze (the runner's base world), refrozen/restored by handle.
        WorldInstanceId m_PausedWorld;
        /// @brief Whether the region tracks the framebuffer extent (a full-window overlay).
        bool m_TrackWindow = false;
        /// @brief Whether the simulation was started (so close stops exactly what it started).
        bool m_Started = false;
    };
}

#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Level.h>
#include <Veng/Input/SeatRef.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Entity.h>
#include <Veng/WorldInstanceId.h>

// Veng/LevelOverlay.h — a whole Level opened over the running world as a secondary, simulated
// overlay, requested by a component.
//
// An overlay is state in the scene that opens it: put a LevelOverlay on an entity and the engine
// opens the level as a world of its own — its own scene, systems and HUD, ticked by the runner like
// any world — and presents it over this one; remove the component, destroy its entity or close its
// world, and the overlay closes. So a system can open one as readily as application code, and the
// overlay's lifetime follows the world that asked for it rather than whoever happened to hold it.
//
// The engine reconciles the components once per frame, at the frame-top request drain (after the
// request components drain, before the worlds tick). Opening one applies the overlay policy:
//   - a Presented viewport over the opener's, placed by Layout and re-fit on every window resize,
//     bound to the overlay world through the overlay's own seat (so that seat's camera is pulled,
//     its pawn marked LocalControl, and the viewport's documents read that seat's roles);
//   - the cursor seat handed to the overlay's seat, and the seat beneath (SuspendSeat) suspended —
//     its input contexts swapped for an empty one — until the overlay closes;
//   - a pause held on the opener's world when PauseOpener is set, and every viewport presenting the
//     opener's world disabled when Opaque is set;
//   - the overlay world's ExitRequest closing the overlay rather than the application.
// Closing unwinds all of it, restoring the cursor seat to whatever it was at open. Overlays stack:
// one opened while another is open suspends that one's seat, and the closes of one frame unwind in
// reverse open order.
//
// Data crosses into the overlay at open through Seed (reflected components) and
// Application::OnOverlayLoaded (anything else). While it is open the opener reads or writes the
// overlay scene through LevelOverlayState::World and the runner; the overlay never reaches back
// into the opener's scene.

namespace Veng
{
    /// @brief Opens a Level as a secondary, simulated overlay presented over this entity's world.
    ///
    /// Local-only: an overlay is a presentation of this peer, so the component never replicates. The
    /// engine opens the overlay at the next frame-top reconcile after the component appears, and closes
    /// it when the component is removed, its entity is destroyed, its world closes or its scene is
    /// replaced, or the overlay world closes — by its own ExitRequest or any other close — in which
    /// case the engine removes this component too, so it does not reopen. The fields are read once,
    /// at open: editing a live component does nothing until it is removed and added again, since
    /// reopening on an edit would rebuild a world under its readers. A world draining its requests
    /// under a Sandboxed policy (a tool's play session) opens none: an overlay reaches the window
    /// and the cursor, which such a world may not.
    struct LevelOverlay
    {
        /// @brief The level opened as the overlay.
        ///
        /// A handle not yet resident is loaded first and the open retried each frame until it lands;
        /// a level that fails to load is logged and the component removed.
        AssetHandle<Level> Source;
        /// @brief Where the overlay's viewport sits, as fractions of the window; the whole window by default.
        Renderer::ViewportLayout Layout;
        /// @brief Hold a pause on this entity's world while the overlay is open.
        ///
        /// The pause is one more holder of the world's pause refcount, so stacked overlays nest and a
        /// pause the game holds itself is never released by an overlay closing. Unset, the world
        /// keeps simulating beneath the overlay; its seat's input is suspended either way.
        bool PauseOpener = false;
        /// @brief The seat in this world whose input the overlay suspends; null takes the cursor seat.
        ///
        /// The cursor seat at open is this world's seat, or a lower overlay's when one is already
        /// open, so a stack of overlays each suspends the one beneath.
        Entity SuspendSeat = Entity::Null;
        /// @brief The overlay covers everything beneath it: disable every viewport presenting this world while it is open.
        ///
        /// A full-window modal makes the render beneath it invisible work. The viewports keep
        /// presenting the world — its systems still see their View, its sound still plays — and only
        /// stop rendering; a viewport that comes to present the world while the overlay is open is
        /// disabled too, and each is re-enabled when no open overlay covers it.
        bool Opaque = false;
        /// @brief Block the open until the level and the overlay world's spawn are resident.
        ///
        /// The convenience path, accepting the open's hitch. Unset, the overlay opens on the first
        /// frame its level is resident and its world's assets stream in afterwards.
        bool WaitForResidency = false;
        /// @brief An entity in this world whose components seed one new entity of the overlay scene.
        ///
        /// Every component of the seed carrying reflected fields is copied — its reflected fields
        /// only — into one fresh entity of the overlay scene before the overlay's simulation starts,
        /// so its systems' OnStart see the copy. Entity references are cleared, since they name this
        /// scene; the hierarchy and the seed's own LevelOverlay are not copied. The seed may be this
        /// entity. Null seeds nothing. State with no reflected form is handed across in
        /// Application::OnOverlayLoaded instead.
        Entity Seed = Entity::Null;
    };

    /// @brief The open overlay a LevelOverlay requested; added by the engine beside it while it is open.
    ///
    /// Runtime-only: it carries no reflected field, so it neither serializes nor rides the wire. A
    /// system, a driver or the application reads it to reach the overlay scene through the runner
    /// (WorldRunner::ResolveWorld) — to mirror state in or read a result out — and its absence means
    /// the overlay is not open yet, or has closed.
    struct LevelOverlayState
    {
        /// @brief The overlay's world.
        WorldInstanceId World;
        /// @brief The overlay's own seat, or a null-Viewer ref when its level seats none.
        SeatRef Seat;
    };
}

/// @cond DOXYGEN_EXCLUDE
VE_REFLECT(::Veng::LevelOverlay, 0xA145CCCCE39E463BULL)
VE_FIELD(Source, .DisplayName = "Source")
VE_FIELD(Layout, .DisplayName = "Layout")
VE_FIELD(PauseOpener, .DisplayName = "Pause Opener")
VE_FIELD(SuspendSeat, .DisplayName = "Suspend Seat")
VE_FIELD(Opaque, .DisplayName = "Opaque")
VE_FIELD(WaitForResidency, .DisplayName = "Wait For Residency")
VE_FIELD(Seed, .DisplayName = "Seed")
VE_REFLECT_END();

VE_TYPE(::Veng::LevelOverlayState, 0xDDA023EE985FA915ULL);
/// @endcond

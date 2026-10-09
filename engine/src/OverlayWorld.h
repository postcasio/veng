#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Input/SeatRef.h>
#include <Veng/Renderer/ViewportId.h>
#include <Veng/Scene/Entity.h>
#include <Veng/WorldInstanceId.h>
#include <Veng/WorldRunner.h>

namespace Veng
{
    class AssetManager;
    class InputRouter;
    class ManagedViewportSet;
    class Scene;
    class SeatFocusScope;
    class TaskSystem;
    struct LevelOverlay;
    struct WorldRequestPolicy;

    namespace Renderer
    {
        class Context;
        class Viewport;
        class ViewportCompositor;
    }

    /// @brief The services an application lends the overlay reconcile, and the hooks it calls back.
    struct OverlayServices
    {
        /// @brief Loads an overlay's level when its handle is not resident.
        AssetManager& Assets;
        /// @brief Waits a WaitForResidency open's spawn resident.
        TaskSystem& Tasks;
        /// @brief Opens, starts, pauses and closes the overlay worlds.
        WorldRunner& Runner;
        /// @brief Takes the cursor seat and the suspended seat's focus.
        InputRouter& Router;
        /// @brief Binds each overlay viewport to its world, and names the viewports an opaque one covers.
        ManagedViewportSet& Managed;
        /// @brief Creates the overlay viewports.
        Renderer::Context& Context;
        /// @brief Drives the overlay viewports and resolves their layouts.
        Renderer::ViewportCompositor& Compositor;
        /// @brief Returns the request policy a world drains under, or null for the Full default.
        function<const WorldRequestPolicy*(WorldInstanceId)> Policy;
        /// @brief Sets a world's request policy.
        function<void(WorldInstanceId, WorldRequestPolicy)> SetPolicy;
        /// @brief Called once per open with the loaded and seeded overlay scene, before it starts.
        function<void(WorldInstanceId opener, Entity entity, WorldInstanceId overlay, Scene& scene)>
            Loaded;
    };

    /// @brief One open overlay: the world it opened and everything its open applied, unwound on close.
    struct OverlayWorld
    {
        /// @brief The world whose LevelOverlay requested this overlay.
        WorldInstanceId OpenerWorld;
        /// @brief The entity carrying the request.
        Entity Opener = Entity::Null;
        /// @brief The opener world's scene at open; a replaced scene closes the overlay.
        const Scene* OpenerScene = nullptr;
        /// @brief The overlay's own world.
        WorldInstanceId World;
        /// @brief The Presented viewport the overlay renders into.
        Unique<Renderer::Viewport> Viewport;
        /// @brief The overlay world's own seat, taken as the cursor seat.
        SeatRef Seat;
        /// @brief The cursor seat to restore on close; inherited from the overlay beneath when that one closes first.
        SeatRef PriorCursorSeat;
        /// @brief The seat whose input this overlay suspends.
        SeatRef SuspendedSeat;
        /// @brief The focus scope suspending SuspendedSeat.
        Unique<SeatFocusScope> Suspend;
        /// @brief The empty input context the suspend scope swaps in.
        AssetHandle<InputMappingContext> SuspendContext;
        /// @brief The pause held on the opener's world; inert unless PauseOpener was set.
        WorldPauseScope Pause;
        /// @brief Whether the viewports presenting the opener's world are disabled while this is open.
        bool Opaque = false;

        /// @brief Drops what is left; OverlayWorlds unwinds the policy in order before that.
        ~OverlayWorld();
    };

    /// @brief The open overlays of one application, reconciled against the scenes' LevelOverlay components.
    ///
    /// The implementation behind Veng/LevelOverlay.h. Holds the open overlays in open order and,
    /// once per frame, closes the ones whose request went (newest first), opens the ones newly
    /// requested, and disables the viewports opaque ones cover. The world-closed hook reaches it
    /// too, so a world closing closes the overlays it opened, and an overlay world closing unwinds
    /// its overlay and removes the request, at once.
    class OverlayWorlds
    {
    public:
        /// @brief Constructs the set over the application's services.
        /// @param services  The services and hooks; each borrowed service outlives the set.
        explicit OverlayWorlds(OverlayServices services);

        /// @brief Unwinds any overlay still open, closing its world.
        ~OverlayWorlds();

        OverlayWorlds(const OverlayWorlds&) = delete;
        OverlayWorlds& operator=(const OverlayWorlds&) = delete;

        /// @brief Closes, opens and covers for this frame; called once, at the frame-top request drain.
        void Reconcile();

        /// @brief Closes the overlays a closed world opened and unwinds the overlay it was.
        ///
        /// Called from the runner's world-closed hook, after the world is erased.
        /// @param world  The world that closed.
        void OnWorldClosed(WorldInstanceId world);

        /// @brief Returns the viewport an open overlay renders into, or null when no open overlay is @p world.
        /// @param world  The overlay's world.
        [[nodiscard]] Renderer::Viewport* FindViewport(WorldInstanceId world) const;

    private:
        /// @brief Opens the overlay @p entity of @p world requests, or leaves it for a later frame.
        /// @param world   The opener world.
        /// @param entity  The entity carrying the LevelOverlay.
        void TryOpen(WorldInstanceId world, Entity entity);

        /// @brief Opens the overlay once its level is resident.
        /// @param world    The opener world.
        /// @param scene    The opener world's scene.
        /// @param entity   The entity carrying the LevelOverlay.
        /// @param request  A copy of the request, read once.
        void Open(WorldInstanceId world, Scene& scene, Entity entity, const LevelOverlay& request);

        /// @brief Closes an open overlay, the overlays its world opened first.
        /// @param overlay        The overlay to close; owned by m_Open on entry.
        /// @param closeWorld     Whether its world is closed too (false when it already has).
        /// @param removeRequest  Whether the opener's LevelOverlay is removed, so it does not reopen.
        void Close(OverlayWorld* overlay, bool closeWorld, bool removeRequest);

        /// @brief Re-links the overlays above @p overlay onto what it covered, before it unwinds.
        /// @param overlay  The overlay about to close, already out of m_Open.
        /// @return False when an overlay above inherited its cursor-seat restore, so it leaves the
        ///         cursor seat alone.
        bool Unlink(OverlayWorld& overlay);

        /// @brief Unwinds an overlay's policy in reverse open order, closing its world when asked.
        /// @param overlay        The overlay to unwind, already out of m_Open.
        /// @param closeWorld     Whether to close its world.
        /// @param restoreCursor  Whether to restore the cursor seat it took.
        void Unwind(OverlayWorld& overlay, bool closeWorld, bool restoreCursor);

        /// @brief Disables each viewport an open opaque overlay covers and re-enables the rest it disabled.
        void ApplyOpaque();

        /// @brief Returns the opener scene of @p overlay while it is still the opener world's scene, else null.
        /// @param overlay  The overlay whose opener to resolve.
        [[nodiscard]] Scene* ResolveOpenerScene(const OverlayWorld& overlay) const;

        /// @brief The borrowed services and the application hooks.
        OverlayServices m_Services;
        /// @brief The open overlays, in open order.
        vector<Unique<OverlayWorld>> m_Open;
        /// @brief The viewports an opaque overlay disabled, re-enabled once none covers them.
        vector<Renderer::ViewportId> m_Disabled;
        /// @brief Scratch the opaque pass collects each opener world's viewports into.
        vector<Renderer::Viewport*> m_Scratch;
    };
}

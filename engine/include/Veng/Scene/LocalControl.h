#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Scene;

    /// @brief One marker move a reconcile made: the presenting seat and the pawn it now controls.
    ///
    /// Reported so a caller can raise the marker's event form (Application::OnClientPossession)
    /// exactly when the marker moves, rather than keeping a second, independently-derived answer
    /// to the same question. Pawn is Entity::Null when the seat stopped controlling anything.
    struct LocalControlChange
    {
        /// @brief The presenting viewport's seat whose controlled pawn changed.
        Entity Seat = Entity::Null;
        /// @brief The pawn that seat now controls, or Entity::Null when it controls none.
        Entity Pawn = Entity::Null;
    };

    /// @brief Returns the pawn a presenting viewport's seat controls in a scene, or Entity::Null.
    ///
    /// The per-viewport read a consumer uses: resolve the viewport's bound seat, then ask this. It
    /// answers from the engine-maintained LocalControl markers, so under split-screen each viewport
    /// gets its own pawn and never another viewport's — which a bare "is this entity marked?" test
    /// cannot distinguish. A null, dead, or non-presenting seat controls nothing.
    /// @param scene  The scene to read; never modified.
    /// @param seat   The presenting viewport's bound seat entity.
    /// @return The marked pawn for that seat, or Entity::Null when the seat controls none.
    [[nodiscard]] VE_API Entity ResolveLocalControlledPawn(const Scene& scene, Entity seat);

    /// @brief Reconciles a scene's LocalControl markers against the seats presenting it.
    ///
    /// The engine's whole marker lifecycle in one pass, so no consumer re-derives it: for each seat
    /// in @p presentingSeats the pawn is that seat's Possesses target (nothing when the seat is
    /// dead, carries no Possesses, or names a pawn that is not alive), and the marker is stamped
    /// onto it carrying the seat. Every existing marker naming a seat that is not presenting, or
    /// naming a pawn its seat no longer controls, is removed — so a possession transfer, a seat
    /// teardown, a viewport unbinding, and a world ceasing to be presented all leave no stale
    /// marker. Passing an empty span clears the scene's markers outright, which is the teardown
    /// form.
    ///
    /// The cost is bounded by the presenting seat count plus the live marker count — one marker per
    /// presenting viewport — never by the scene's entity count. A pawn two presenting seats both
    /// possess keeps the marker of whichever seat holds it, since the marker is singular per pawn.
    /// @param scene            The scene whose markers are reconciled.
    /// @param presentingSeats  The seats bound to the viewports presenting this scene.
    /// @param changed          Optional sink appended with one entry per presenting seat whose
    ///                         controlled pawn moved; may be null when the caller raises no event.
    /// @pre No Scene iteration is in flight — the pass adds and removes components.
    VE_API void ReconcileLocalControl(Scene& scene, std::span<const Entity> presentingSeats,
                                      vector<LocalControlChange>* changed = nullptr);
}

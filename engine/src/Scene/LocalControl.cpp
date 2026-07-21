#include <Veng/Scene/LocalControl.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        // The pawn a seat controls right now, or Entity::Null. A seat that is dead, carries no
        // Possesses, or names a pawn that has not spawned locally (or has died) controls nothing.
        [[nodiscard]] Entity ControlledPawn(const Scene& scene, const Entity seat)
        {
            if (seat.IsNull() || !scene.IsAlive(seat))
            {
                return Entity::Null;
            }
            const auto* const possesses = scene.TryGet<Possesses>(seat);
            if (possesses == nullptr || possesses->Pawn.IsNull() || !scene.IsAlive(possesses->Pawn))
            {
                return Entity::Null;
            }
            return possesses->Pawn;
        }

        [[nodiscard]] bool Contains(const std::span<const Entity> seats, const Entity seat)
        {
            return std::ranges::find(seats, seat) != seats.end();
        }
    }

    Entity ResolveLocalControlledPawn(const Scene& scene, const Entity seat)
    {
        if (seat.IsNull())
        {
            return Entity::Null;
        }
        for (auto [entity, control] : scene.View<const LocalControl>())
        {
            if (control.Seat == seat)
            {
                return entity;
            }
        }
        return Entity::Null;
    }

    void ReconcileLocalControl(Scene& scene, const std::span<const Entity> presentingSeats,
                               vector<LocalControlChange>* const changed)
    {
        // The pawn each presenting seat was marked with on entry, so the pass reports a move by
        // comparing against what it leaves rather than by tracking state across frames.
        vector<Entity> previous;
        if (changed != nullptr)
        {
            previous.reserve(presentingSeats.size());
            for (const Entity seat : presentingSeats)
            {
                previous.push_back(ResolveLocalControlledPawn(scene, seat));
            }
        }

        // Collect before mutating: adding or removing a component mid-View is a structural change
        // during iteration, which the ECS treats as API misuse.
        vector<Entity> stale;
        for (auto [entity, control] : scene.View<const LocalControl>())
        {
            if (!Contains(presentingSeats, control.Seat) ||
                ControlledPawn(scene, control.Seat) != entity)
            {
                stale.push_back(entity);
            }
        }
        for (const Entity entity : stale)
        {
            scene.Remove<LocalControl>(entity);
        }

        for (const Entity seat : presentingSeats)
        {
            const Entity pawn = ControlledPawn(scene, seat);
            // A pawn already marked for another presenting seat keeps that seat's marker: the
            // marker is singular per pawn, and two viewports possessing one pawn is degenerate.
            if (!pawn.IsNull() && !scene.Has<LocalControl>(pawn))
            {
                scene.Add<LocalControl>(pawn, LocalControl{.Seat = seat});
            }
        }

        if (changed == nullptr)
        {
            return;
        }
        for (usize index = 0; index < presentingSeats.size(); ++index)
        {
            const Entity seat = presentingSeats[index];
            if (const Entity pawn = ResolveLocalControlledPawn(scene, seat);
                pawn != previous[index])
            {
                changed->push_back({.Seat = seat, .Pawn = pawn});
            }
        }
    }
}

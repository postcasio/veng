#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Builtin Sim system that seats and unseats characters as InteractRequests land on vehicles.
    ///
    /// It drains an InteractRequest stamped on any entity carrying a Vehicle, following the request
    /// idiom (handled → removed, unhandleable → left Pending, failed → marked Failed and held a frame).
    /// The request's Interactor decides the direction: if it already occupies a seat of the vehicle the
    /// interaction leaves it, otherwise it boards the first free seat in preference order.
    ///
    /// Entering disables the character's CharacterController and removes its capsule, parents the
    /// character to the vehicle at the seat's mesh socket, sets the seat's Occupant, and — for a driver
    /// seat — re-points the controlling seat's Possesses at the vehicle and swaps its input context.
    /// LocalControl follows for free: the engine's per-frame reconcile sees the new Possesses and moves
    /// the marker, so no vehicle-specific code runs in the marker path.
    ///
    /// Exiting is the exact inverse in reverse order: it restores the input context and possession,
    /// clears the Occupant, places the character at the seat's exit socket, and re-enables its
    /// controller seeded with the vehicle's current velocity so leaving a moving vehicle introduces no
    /// discontinuity. Exit is validated before it is performed — the exit socket is overlap-tested
    /// against solid geometry and a blocked exit fails and reports rather than placing a character
    /// inside a wall.
    class VE_API VehicleSystem final : public SceneSystem
    {
    public:
        /// @brief Drains each vehicle's InteractRequest, performing the enter or exit it names.
        /// @param scene    The scene whose vehicles are driven.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services (unused).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::VehicleSystem, 0xD6E9B52D8EB46D65ULL, "Vehicle");

#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;

    /// @brief Builtin Sim system that auto-assigns connected pads to opted-in seats.
    ///
    /// Reconciles each seat's SeatInput::Gamepad against the connected-pad set on Veng::Input:
    /// a pad currently connected but held by no seat is assigned to the first seat with
    /// WantsGamepad and a None slot (deterministic seat-iteration order); a seat whose assigned
    /// slot is no longer connected is cleared back to None. A level-authored slot is respected —
    /// the policy fills only None slots and only for WantsGamepad seats. Registered immediately
    /// before InputMappingSystem so a pad connected this tick is assigned before this tick's
    /// resolve reads it. It is a Sim system, not an InputRouter responsibility, because it needs
    /// Scene access to walk the seats and mutate their SeatInput; in headless the connected set is
    /// empty, so it is a no-op.
    class DeviceAssignmentSystem final : public SceneSystem
    {
    public:
        /// @brief Reconciles every seat's SeatInput::Gamepad against the connected pads this tick.
        /// @param scene    The scene whose seats are reassigned.
        /// @param delta    Time in seconds since the previous tick (unused).
        /// @param context  Per-tick services; the connected-pad set is read from the input snapshot.
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;
    };
}

VE_SYSTEM(::Veng::DeviceAssignmentSystem, 0x6C37AE233D0E890AULL, "Device Assignment");

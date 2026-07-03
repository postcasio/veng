#include <Veng/Scene/DeviceAssignmentSystem.h>

#include <algorithm>

#include <Veng/Input.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void DeviceAssignmentSystem::OnUpdate(Scene& scene, const f32, const SystemContext& context)
    {
        const std::span<const GamepadId> connected = context.Input.ConnectedGamepads();

        // Clear any seat whose assigned pad is no longer connected, so a disconnected slot never
        // stays bound and its later reuse never silently re-points the seat at a new pad.
        scene.Each<SeatInput>(
            [&](Entity, SeatInput& devices)
            {
                if (devices.Gamepad != GamepadId::None &&
                    std::ranges::find(connected, devices.Gamepad) == connected.end())
                {
                    devices.Gamepad = GamepadId::None;
                }
            });

        // A connected slot already held by some seat is not up for assignment.
        const auto slotHeld = [&](const GamepadId slot)
        {
            bool held = false;
            scene.Each<SeatInput>([&](Entity, SeatInput& devices)
                                  { held = held || devices.Gamepad == slot; });
            return held;
        };

        // Assign each unheld connected slot to the first WantsGamepad seat still lacking a pad —
        // deterministic seat-iteration order, so the assignment is stable run to run.
        for (const GamepadId slot : connected)
        {
            if (slotHeld(slot))
            {
                continue;
            }

            bool assigned = false;
            scene.Each<SeatInput>(
                [&](Entity, SeatInput& devices)
                {
                    if (!assigned && devices.WantsGamepad && devices.Gamepad == GamepadId::None)
                    {
                        devices.Gamepad = slot;
                        assigned = true;
                    }
                });
        }
    }
}

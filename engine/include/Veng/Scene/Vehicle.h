#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    /// @brief A thing a character can be inside and control — the possession-and-seating half.
    ///
    /// Deliberately movement-agnostic: a Vehicle here is not wheeled dynamics — no wheels, no
    /// suspension, no engine, no constraint. How it moves is entirely its own business; a consumer
    /// attaches whatever movement system it likes to the vehicle pawn and this system neither knows
    /// nor cares. That separation is what lets one feature serve a car, a boat, or a lift.
    ///
    /// The entity carrying a Vehicle is the one a character parents to on entry, so it is expected to
    /// draw a mesh whose sockets name the seat and exit positions (see VehicleSeat).
    struct Vehicle
    {
        /// @brief The seat entities, each carrying a VehicleSeat, in boarding preference order.
        ///
        /// Entering picks the first seat in this order whose Occupant is Null. The references remap on
        /// prefab spawn like any intra-prefab Entity reference.
        vector<Entity> Seats;
    };

    /// @brief One seat in a Vehicle: where an occupant sits, whether they drive, and where they leave.
    ///
    /// Seat placement is entirely mesh sockets, so where a pilot sits and where they climb out are
    /// facts about the model rather than numbers in a prefab that drift from it. Socket and ExitSocket
    /// name sockets on the Vehicle entity's mesh.
    struct VehicleSeat
    {
        /// @brief Mesh socket on the vehicle naming this seat's position and facing.
        string Socket;
        /// @brief The character occupying the seat, or Entity::Null when empty.
        Entity Occupant = Entity::Null;
        /// @brief Whether occupying this seat re-points the controlling seat's possession at the vehicle.
        bool IsDriver = false;
        /// @brief Mesh socket on the vehicle where an occupant is placed on leaving.
        string ExitSocket;
        /// @brief The input context pushed onto the controlling seat while a driver occupies this seat.
        ///
        /// Read only for a driver seat: on entry the controlling seat's top input context is popped and
        /// this one pushed, and the whole prior stack is restored on exit. Empty pushes nothing, so a
        /// seat whose scheme is unchanged authors none.
        AssetHandle<InputMappingContext> Context;
    };

    /// @brief Runtime bookkeeping an occupant carries while seated, so exit can undo entry exactly.
    ///
    /// Added to a character on entry and removed on exit. It records the vehicle and seat it is in, the
    /// controlling seat whose possession was re-pointed (for a driver seat, else Null), and a snapshot
    /// of that seat's input-context stack taken before the swap, so leaving restores it verbatim.
    ///
    /// Runtime-only derived state: it carries no reflected field, so it never serializes and never
    /// rides the wire.
    struct Seated
    {
        /// @brief The vehicle the character is inside.
        Entity Vehicle = Entity::Null;
        /// @brief The seat entity the character occupies.
        Entity Seat = Entity::Null;
        /// @brief The controlling seat whose Possesses was re-pointed at the vehicle, or Null.
        Entity ControllingSeat = Entity::Null;
        /// @brief The controlling seat's input-context stack as it was before entry, restored on exit.
        vector<AssetHandle<InputMappingContext>> StashedContexts;
    };
}

VE_REFLECT(::Veng::Vehicle, 0xCB34CE0DC9FBBA05ULL)
VE_ARRAY_FIELD(Seats, .DisplayName = "Seats")
VE_REFLECT_END();

VE_REFLECT(::Veng::VehicleSeat, 0x9CC53AF89AC9581EULL)
VE_FIELD(Socket, .DisplayName = "Socket", .Tooltip = "Mesh socket naming this seat's position")
VE_FIELD(Occupant, .DisplayName = "Occupant", .ReadOnly = true)
VE_FIELD(IsDriver, .DisplayName = "Is Driver")
VE_FIELD(ExitSocket, .DisplayName = "Exit Socket",
         .Tooltip = "Mesh socket where an occupant is placed on leaving")
VE_FIELD(Context, .DisplayName = "Context")
VE_REFLECT_END();

VE_TYPE(::Veng::Seated, 0xDD3C694C2ACBCF4EULL);

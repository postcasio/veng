#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Requests.h>

namespace Veng
{
    /// @brief A thing an Interactor can focus and act on — the passive half of interaction.
    ///
    /// Authored on any entity a character can use: a door, a switch, a vehicle. The engine's
    /// InteractionSystem resolves which Interactable an Interactor is looking at and publishes it as
    /// the Interactor's Focused; a prompt is then a UI concern reading Focused and Verb. What happens
    /// when the interaction fires is not a callback on this component — a system that owns the kind of
    /// interactable drains an InteractRequest stamped on the entity, so no game code runs inside the
    /// resolve query.
    struct Interactable
    {
        /// @brief What a prompt displays for the action — "Enter", "Open"; presentation reads it.
        string Verb;
        /// @brief Largest distance from the interactor's origin at which this is offered, in metres.
        f32 Range = 2.0f;
        /// @brief Whether this interactable is currently offered; a disabled one is never focused.
        bool Enabled = true;
    };

    /// @brief The active half of interaction — a pawn that looks for an Interactable to use.
    ///
    /// Authored on the acting entity (a character). Each tick the InteractionSystem sweeps an Overlap
    /// within Reach, keeps the Interactable entities inside the view cone and within their own Range,
    /// picks the best by angle then distance, and writes it to Focused. Forward is the entity's local
    /// -Z in world space, matching the socket and camera convention. The resolution is published;
    /// drawing a prompt from it, and deciding when to fire, are the consumer's.
    struct Interactor
    {
        /// @brief How far the overlap sweep reaches for candidates, in metres.
        f32 Reach = 3.0f;
        /// @brief Half-angle of the view cone a candidate must fall within, in radians.
        f32 ConeAngle = 0.7853982f;
        /// @brief The best Interactable this tick, or Entity::Null when none qualifies. View output.
        Entity Focused = Entity::Null;
    };

    /// @brief Fires an interaction against the entity it is stamped on — the request, not a callback.
    ///
    /// A control system stamps this on an Interactor's Focused entity when the interact input fires;
    /// the system that owns that kind of interactable drains it in its own phase (the builtin
    /// VehicleSystem drains it on a Vehicle), so nothing runs inside the resolve query. It follows the
    /// same consumption idiom as the builtin application requests (Veng/Scene/Requests.h): a handled
    /// request is removed, an unhandleable one is left Pending to retry, and a failed one is marked
    /// Failed with a reason and held one frame so the stamper can read the outcome before re-stamping.
    /// Local-only — it carries no VE_REPLICATED and never rides a snapshot.
    struct InteractRequest
    {
        /// @brief The entity performing the interaction — the Interactor's owner.
        Entity Interactor = Entity::Null;
        /// @brief The engine-reported outcome; starts Pending.
        RequestStatus Status = RequestStatus::Pending;
        /// @brief The failure reason, set when Status is Failed.
        string Error;
    };
}

VE_REFLECT(::Veng::Interactable, 0x2C5E8952E4CFD100ULL)
VE_FIELD(Verb, .DisplayName = "Verb", .Tooltip = "What a prompt displays for the action")
VE_FIELD(Range, .DisplayName = "Range", .Tooltip = "Metres from the interactor's origin",
         .Display = {.Min = 0.0})
VE_FIELD(Enabled, .DisplayName = "Enabled")
VE_REFLECT_END();

VE_REFLECT(::Veng::Interactor, 0x9A5904B9F54B411EULL)
VE_FIELD(Reach, .DisplayName = "Reach", .Tooltip = "Overlap query distance in metres",
         .Display = {.Min = 0.0})
VE_FIELD(ConeAngle, .DisplayName = "Cone Angle",
         .Tooltip = "Radians; how far off-centre a candidate may be", .Display = {.Min = 0.0})
VE_FIELD(Focused, .DisplayName = "Focused", .ReadOnly = true)
VE_REFLECT_END();

VE_REFLECT(::Veng::InteractRequest, 0x8EB53777366CEE94ULL)
VE_FIELD(Interactor, .DisplayName = "Interactor")
VE_FIELD(Status, .DisplayName = "Status", .ReadOnly = true)
VE_FIELD(Error, .DisplayName = "Error", .ReadOnly = true)
VE_REFLECT_END();

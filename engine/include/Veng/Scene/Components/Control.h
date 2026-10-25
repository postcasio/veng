#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class InputMappingContext;

    /// @brief Per-seat resolved input for this tick — the serializable action snapshot.
    ///
    /// The resolved values of the seat's active actions this tick (see Veng/Input/Actions.h),
    /// produced by InputMappingSystem from the seat's InputContextStack. It is the control
    /// chokepoint a net layer replicates and the uniform surface a control system reads by
    /// action id — filled locally for an owned seat, from the wire for a remote one. It
    /// serializes through the reflection serializer's name-keyed FieldClass::Array encoding
    /// for its ActionState, each sample self-describing by its ActionId; there is no bespoke
    /// wire format. The Get*/Was* methods delegate to State, a read-through convenience so a
    /// control system reads input.WasTriggered(Actions::Jump) directly.
    struct PlayerInput
    {
        /// @brief The resolved action set for the seat this tick.
        ActionState State;

        /// @brief Resolved value of an action, or zero when absent.
        /// @param id  The action to look up.
        /// @return The action's Value, or a zero vector when absent.
        [[nodiscard]] vec2 GetValue(ActionId id) const { return State.GetValue(id); }

        /// @brief Resolved x component of an action, the 1D-axis convenience.
        /// @param id  The action to look up.
        /// @return The action's Value.x, or zero when absent.
        [[nodiscard]] f32 GetAxis(ActionId id) const { return State.GetAxis(id); }

        /// @brief Whether an action is currently active (Started or Ongoing).
        /// @param id  The action to look up.
        /// @return True while the action is held.
        [[nodiscard]] bool IsHeld(ActionId id) const { return State.IsHeld(id); }

        /// @brief Whether an action became active this tick (Started).
        /// @param id  The action to look up.
        /// @return True on the tick the action activated.
        [[nodiscard]] bool WasTriggered(ActionId id) const { return State.WasTriggered(id); }

        /// @brief Whether an action was released this tick (Completed).
        /// @param id  The action to look up.
        /// @return True on the tick the action released.
        [[nodiscard]] bool WasReleased(ActionId id) const { return State.WasReleased(id); }

        /// @brief Whether an action started on any Sim tick since this frame began.
        ///
        /// The query a once-per-frame reader (a View system) uses so a Started edge on a non-final
        /// tick of a multi-tick frame is not lost; a per-tick Sim system uses WasTriggered.
        /// @param id  The action to look up.
        /// @return True if the action activated on any tick this frame.
        [[nodiscard]] bool WasTriggeredThisFrame(ActionId id) const
        {
            return State.WasTriggeredThisFrame(id);
        }

        /// @brief Whether an action released on any Sim tick since this frame began.
        ///
        /// The once-per-frame release query; the companion to WasTriggeredThisFrame.
        /// @param id  The action to look up.
        /// @return True if the action released on any tick this frame.
        [[nodiscard]] bool WasReleasedThisFrame(ActionId id) const
        {
            return State.WasReleasedThisFrame(id);
        }
    };

    /// @brief The ordered active input contexts for a seat, highest priority last.
    ///
    /// InputMappingSystem resolves these against the raw snapshot into the seat's PlayerInput.
    /// Gameplay systems push/pop entries to switch schemes (enter a vehicle, open a modal). The
    /// fine-grained, per-seat sibling of the InputRouter's coarse focus stack.
    ///
    /// Each entry is a cooked InputMappingContext referenced by AssetId — authored on a prefab
    /// and resolved as an ordinary load-time dependency, so a seat's base scheme is data. A
    /// not-yet-resident handle resolves to empty actions until it streams in.
    struct InputContextStack
    {
        /// @brief The active contexts, lowest priority first; resolved as a stack each tick.
        vector<AssetHandle<InputMappingContext>> Active;

        /// @brief Whether Active is the seat's whole UI scheme, with no default UI context beneath it.
        ///
        /// The engine's per-frame role resolution otherwise appends the application's default UI
        /// context below Active (Application::SetDefaultUiContext). A SeatFocusScope that swaps in a
        /// context sets this for its lifetime, so a seat suspended under an empty context navigates
        /// nothing. Runtime state, not reflected.
        bool Exclusive = false;
    };

    /// @brief Abstract, source-agnostic command for what a pawn wants to do this tick.
    ///
    /// The interface between "who decides" (player, AI, remote) and "what happens"
    /// (movement and gameplay systems). A control or AI system writes it; the movement
    /// system consumes it. It is overwritten each tick by its producer, so a zero Intent
    /// is a pawn at rest. Move is expressed in the pawn's local frame.
    struct Intent
    {
        /// @brief Desired move direction in the pawn's local frame: X right, Y up, Z forward.
        vec3 Move{0.0f};
        /// @brief Desired rotational command this tick: X yaw, Y pitch, Z roll, in radians-scaling units.
        vec3 Look{0.0f};
        /// @brief Action-flag bitset (jump/fire/...); bit meanings are game policy.
        u32 Actions = 0;
    };

    /// @brief Seat-to-pawn link: names the pawn entity a player/seat controls.
    ///
    /// Possession is just this reference — nothing inherits or owns through it, and it
    /// is independent of the seat's Viewer.Camera (a spectator views without possessing;
    /// a cutscene retargets the camera without un-possessing). The Pawn field is a
    /// reflected Entity reference, so it remaps on prefab spawn like any intra-prefab
    /// reference.
    struct Possesses
    {
        /// @brief The pawn entity this seat controls.
        Entity Pawn = Entity::Null;
    };

    /// @brief The physical devices that feed one local seat's input this session.
    ///
    /// Lives on the Viewer entity beside Possesses and InputContextStack. InputMappingSystem
    /// builds a per-seat filtered view (SeatInputView) from it — gamepad arms scoped to Gamepad,
    /// keyboard/mouse arms present only when UsesKeyboardMouse — so two seats with different
    /// assignments resolve to distinct PlayerInputs. Authorable in a level and serialized through
    /// the reflection path. A seat with no SeatInput takes no local device input:
    /// InputMappingSystem skips it, so its PlayerInput is synthesized or replicated (the AI/remote
    /// path). A seat whose world no viewport presents reads neutral input (NeutralActionState) and
    /// raises no navigation or focus-release press unless it sets ReceivesInputWhileUnpresented.
    struct SeatInput
    {
        /// @brief Whether this seat reads the keyboard (and, region-gated elsewhere, the pointer).
        bool UsesKeyboardMouse = true;
        /// @brief The pad slot assigned to this seat, or GamepadId::None for no pad.
        GamepadId Gamepad = GamepadId::None;
        /// @brief Whether DeviceAssignmentSystem auto-fills the Gamepad slot when a pad connects.
        ///
        /// Independent of UsesKeyboardMouse: the ordinary single-player seat opts into both a
        /// keyboard and an auto-assigned pad. The flag fills a None Gamepad slot on connect; a
        /// level-authored slot is left untouched.
        bool WantsGamepad = false;
        /// @brief Whether the seat keeps reading its devices while no viewport presents its world.
        ///
        /// Off by default: a seat takes input only while its world is on screen, so a world ticking
        /// unseen — a destination loading behind the one presented, a world a hidden tab shows — is
        /// never driven by controls the player is aiming at another. Set it for a seat that is
        /// meant to be steered unseen, such as a remotely piloted drone or a picture-in-picture view
        /// whose world is not registered as presented. It governs the seat's actions and its
        /// navigation alike: the per-frame role presses (navigation, ReleaseFocus) follow it too.
        bool ReceivesInputWhileUnpresented = false;
    };

    /// @brief Per-pawn movement tuning the movement system scales its integration by.
    ///
    /// Authored data so a pawn's feel is tunable. A pawn without a Mover moves at the
    /// component's default speeds.
    struct Mover
    {
        /// @brief Local-space move speed in units per second.
        f32 MoveSpeed = 4.0f;
        /// @brief Look/turn speed scaling the Intent's look delta, in radians per unit.
        f32 TurnSpeed = 2.0f;
    };

    /// @brief Frame a ConstantMotion's velocities are applied in.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class MotionSpace : u32
    {
        /// @brief Apply in the entity's own frame: linear velocity along its local axes,
        ///        rotation about its local axis (post-multiplied onto the orientation).
        Local = 0,
        /// @brief Apply in the parent frame: linear velocity along the parent axes,
        ///        rotation about the parent axis (pre-multiplied onto the orientation).
        World = 1,
    };

    /// @brief Constant per-tick transform velocity: drifts and/or spins an entity at a fixed rate.
    ///
    /// Authored data the ConstantMotionSystem integrates each tick — a rate of change of the
    /// Transform, not a curve. Both velocities are zero by default, so a drift-only or
    /// spin-only entity sets just the one it needs. AngularVelocity is an axis-angle vector:
    /// its direction is the spin axis and its magnitude is the angular speed in radians per
    /// second (a zero vector does not spin). Space selects whether the velocities are applied
    /// in the entity's local frame or its parent frame.
    struct ConstantMotion
    {
        /// @brief Linear velocity in units per second.
        vec3 LinearVelocity = vec3(0.0f);
        /// @brief Angular velocity as an axis-angle vector: direction is the spin axis,
        ///        magnitude is radians per second.
        vec3 AngularVelocity = vec3(0.0f);
        /// @brief Frame the velocities are applied in.
        MotionSpace Space = MotionSpace::World;
    };
}

VE_REFLECT(::Veng::PlayerInput, 0x5401D36B1EF55045ULL)
VE_FIELD(State, .DisplayName = "State")
VE_REFLECT_END();
// Not VE_REPLICATED: PlayerInput flows client→server on the dedicated input channel (last-N-ticks
// redundant, unreliable), not in the server→client state snapshot — a different direction and
// discipline from replicated state.

VE_REFLECT(::Veng::InputContextStack, 0x89B0625016A01BE2ULL)
VE_ARRAY_FIELD(Active, .DisplayName = "Active")
VE_REFLECT_END();

VE_REFLECT(::Veng::Intent, 0x27F416122B525965ULL)
VE_FIELD(Move, .DisplayName = "Move")
VE_FIELD(Look, .DisplayName = "Look")
VE_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();
// Not VE_REPLICATED: Intent is re-derived per tick server-side from the seat's input and is never
// sent — the client sees only its effect (the moved Transform), never the command.

VE_REFLECT(::Veng::Possesses, 0xC7D4144C7DF95B9BULL)
VE_FIELD(Pawn, .DisplayName = "Pawn")
VE_REFLECT_END();
// A seat's possession is server-authoritative; the Pawn reference replicates as its target's NetId.
VE_REPLICATED(::Veng::Possesses);

VE_REFLECT(::Veng::SeatInput, 0x2D178569EDDBC215ULL)
VE_FIELD(UsesKeyboardMouse, .DisplayName = "Uses Keyboard/Mouse")
VE_FIELD(Gamepad, .DisplayName = "Gamepad")
VE_FIELD(WantsGamepad, .DisplayName = "Wants Gamepad")
VE_FIELD(ReceivesInputWhileUnpresented, .DisplayName = "Receives Input While Unpresented")
VE_REFLECT_END();
// Not VE_REPLICATED: SeatInput is a client-local device assignment (which keyboard/pad feeds a
// seat) — meaningless on another peer, never sent.

VE_REFLECT(::Veng::Mover, 0x7774F1C2B00DE07EULL)
VE_FIELD(MoveSpeed, .DisplayName = "Move Speed", .Display = {.Min = 0.0})
VE_FIELD(TurnSpeed, .DisplayName = "Turn Speed", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_ENUM(::Veng::MotionSpace, 0x46914AC0C743D776ULL)
VE_ENUMERATOR(Local)
VE_ENUMERATOR(World)
VE_ENUM_END();

VE_REFLECT(::Veng::ConstantMotion, 0xEBB74CB78D872F9FULL)
VE_FIELD(LinearVelocity, .DisplayName = "Linear Velocity", .Tooltip = "Units per second")
VE_FIELD(AngularVelocity, .DisplayName = "Angular Velocity",
         .Tooltip = "Axis-angle vector: direction is the spin axis, magnitude is radians/sec")
VE_FIELD(Space, .DisplayName = "Space", .Tooltip = "Local (own frame) or World (parent frame)")
VE_REFLECT_END();

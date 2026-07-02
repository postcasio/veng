#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <span>

namespace Veng
{
    /// @brief Stable identity of a named input action, authored like an AssetId.
    ///
    /// A game mints one id per action (a C++ constant a control system references and the
    /// binding JSON names). There is no registry — an action "exists" by being declared in a
    /// context's InputAction list. Null is the reserved empty id.
    enum class ActionId : u64
    {
        /// @brief The empty id, distinct from every minted action id.
        Null = 0
    };

    /// @brief The value shape a resolved action carries.
    enum class ActionKind : u32
    {
        /// @brief A digital action: value x is 0 or 1.
        Button,
        /// @brief A one-dimensional analog action: value x.
        Axis1D,
        /// @brief A two-dimensional analog action: value xy.
        Axis2D
    };

    /// @brief One action a context declares: its id, display name, and value shape.
    struct InputAction
    {
        /// @brief The action's stable identity, referenced by bindings and control code.
        ActionId Id = ActionId::Null;

        /// @brief Display/authoring label; on-disk identity is Id, not this name.
        string Name;

        /// @brief The value shape this action resolves to.
        ActionKind Kind = ActionKind::Button;
    };

    /// @brief Which raw device a binding reads.
    ///
    /// The Gamepad* arms are forward vocabulary: Veng::Input carries no gamepad state yet, so
    /// the resolver reads them as neutral until the device layer lands. Keyboard and mouse are
    /// live now.
    enum class InputDeviceType : u32
    {
        /// @brief A keyboard key (Control is a key code).
        Keyboard,
        /// @brief A mouse button (Control is a button index).
        MouseButton,
        /// @brief A mouse motion axis (Control selects the axis).
        MouseAxis,
        /// @brief A gamepad button (forward vocabulary; read as neutral).
        GamepadButton,
        /// @brief A gamepad analog axis (forward vocabulary; read as neutral).
        GamepadAxis
    };

    /// @brief A raw control reference: a device kind plus a control code interpreted per device.
    struct InputSource
    {
        /// @brief The device kind the Control code is interpreted against.
        InputDeviceType Device = InputDeviceType::Keyboard;

        /// @brief Key code / mouse-button / mouse-axis / gamepad button or axis index.
        u32 Control = 0;
    };

    /// @brief Which component of a vector action a scalar source drives.
    enum class AxisComponent : u32
    {
        /// @brief A native axis drives the action value directly.
        Whole,
        /// @brief The source contributes to the action's x component.
        X,
        /// @brief The source contributes to the action's y component.
        Y
    };

    /// @brief One raw-source → action mapping with the minimal modifiers.
    ///
    /// A scalar source contributes to one action component with a signed scale (a negative
    /// Scale inverts, so there is no separate invert flag); a native axis (AxisComponent::Whole)
    /// drives its action directly. This covers WASD → a 2D Move action and a stick → the same
    /// action.
    struct Binding
    {
        /// @brief The raw control this binding reads.
        InputSource Source;

        /// @brief The action this binding contributes to.
        ActionId Action = ActionId::Null;

        /// @brief Which action component the source drives (Whole for a native axis).
        AxisComponent Axis = AxisComponent::Whole;

        /// @brief Signed scale applied to the source value before accumulation.
        f32 Scale = 1.0f;
    };

    /// @brief How an action's activation changed this tick.
    enum class ActionPhase : u32
    {
        /// @brief Inactive this tick and last tick.
        None,
        /// @brief Became active this tick.
        Started,
        /// @brief Still active, having been active last tick.
        Ongoing,
        /// @brief Released this tick, having been active last tick.
        Completed
    };

    /// @brief One resolved action this tick.
    struct ActionSample
    {
        /// @brief The resolved action's identity.
        ActionId Id = ActionId::Null;

        /// @brief The resolved value: button → x in {0,1}; Axis1D → x; Axis2D → xy.
        vec2 Value{0.0f};

        /// @brief How the action's activation changed this tick.
        ActionPhase Phase = ActionPhase::None;
    };

    /// @brief The resolved action set for one seat this tick.
    ///
    /// Carries one sample per action declared across the active contexts, in the deterministic
    /// stack-declared order the resolution contract defines, so the same active stack yields the
    /// same layout run to run. The Get*/Was* helpers are the surface a control system reads.
    struct ActionState
    {
        /// @brief The resolved samples, one per declared action, in stack-declared order.
        vector<ActionSample> Actions;

        /// @brief Resolved value of an action, or zero if the action is not present.
        /// @param id  The action to look up.
        /// @return The action's Value, or a zero vector when absent.
        [[nodiscard]] vec2 GetValue(ActionId id) const;

        /// @brief Resolved x component of an action, the 1D-axis convenience.
        /// @param id  The action to look up.
        /// @return The action's Value.x, or zero when absent.
        [[nodiscard]] f32 GetAxis(ActionId id) const;

        /// @brief Whether an action is currently active (Started or Ongoing).
        /// @param id  The action to look up.
        /// @return True while the action is held.
        [[nodiscard]] bool IsHeld(ActionId id) const;

        /// @brief Whether an action became active this tick (Started).
        /// @param id  The action to look up.
        /// @return True on the tick the action activated.
        [[nodiscard]] bool WasTriggered(ActionId id) const;

        /// @brief Whether an action was released this tick (Completed).
        /// @param id  The action to look up.
        /// @return True on the tick the action released.
        [[nodiscard]] bool WasReleased(ActionId id) const;
    };

    /// @brief The in-memory, load-resolved form of an InputMappingContext.
    ///
    /// The declared InputActions plus the Bindings already validated against them. The cooked
    /// asset produces one; a test hand-builds one. ResolveActions reads a stack of these.
    struct ResolvedContext
    {
        /// @brief The actions this context declares, in declaration order.
        vector<InputAction> Actions;

        /// @brief The raw-source → action bindings this context contributes.
        vector<Binding> Bindings;
    };

    /// @brief The raw-input read surface the resolver needs, satisfied by Veng::Input.
    ///
    /// A read-only interface so the resolver is testable with a fake and never links the
    /// windowing snapshot. It reports only this tick's state — phase comes from the previous
    /// ActionState threaded into ResolveActions, not from a previous-raw query — so there is no
    /// *Prev accessor.
    struct RawInputView
    {
        /// @brief Virtual destructor for the abstract read surface.
        virtual ~RawInputView() = default;

        /// @brief Whether a keyboard key is down this tick.
        /// @param code  The key code.
        /// @return True while the key is held.
        [[nodiscard]] virtual bool IsKeyDown(u32 code) const = 0;

        /// @brief Whether a device button is down this tick.
        /// @param device  The device the button belongs to.
        /// @param code    The button index.
        /// @return True while the button is held.
        [[nodiscard]] virtual bool IsButtonDown(InputDeviceType device, u32 code) const = 0;

        /// @brief The value of a device axis this tick.
        /// @param device  The device the axis belongs to.
        /// @param code    The axis index.
        /// @return The axis value (zero for a neutral or unsupported source).
        [[nodiscard]] virtual f32 GetAxis(InputDeviceType device, u32 code) const = 0;
    };

    /// @brief Resolve active bindings against a raw snapshot into the seat's action state.
    ///
    /// Pure: same inputs → same output, no device/GPU/scene access. The result carries one
    /// sample per action declared across the active contexts, in stack-declared order (each
    /// context's actions in declaration order, an action declared in more than one context
    /// keeping its first position). A higher-priority context (later in active) that binds an
    /// action shadows a lower context's bindings of that same action entirely. Combines a 2D
    /// action's component bindings, applies each binding's signed scale, and derives each
    /// action's phase by comparing this tick's activation against previous — the seat's
    /// ActionState from last tick — so phase needs no stateful adapter and works for axis
    /// actions.
    /// @param active    The active context stack, lowest priority first.
    /// @param raw       This tick's raw input read surface.
    /// @param previous  The seat's resolved ActionState from last tick, for phase derivation.
    /// @return The resolved ActionState this tick.
    [[nodiscard]] ActionState ResolveActions(std::span<const ResolvedContext> active,
                                             const RawInputView& raw, const ActionState& previous);
}

VE_LEAF(::Veng::ActionId, 0x5ED22C7AFCDD1E13ULL, ::Veng::FieldClass::Scalar);

VE_ENUM(::Veng::ActionKind, 0x03FEB372C7B46A10ULL)
VE_ENUMERATOR(Button)
VE_ENUMERATOR(Axis1D)
VE_ENUMERATOR(Axis2D)
VE_ENUM_END();

VE_ENUM(::Veng::InputDeviceType, 0x8545AEF8E03B743CULL)
VE_ENUMERATOR(Keyboard)
VE_ENUMERATOR(MouseButton)
VE_ENUMERATOR(MouseAxis)
VE_ENUMERATOR(GamepadButton)
VE_ENUMERATOR(GamepadAxis)
VE_ENUM_END();

VE_ENUM(::Veng::AxisComponent, 0xFA84EF435C864686ULL)
VE_ENUMERATOR(Whole)
VE_ENUMERATOR(X)
VE_ENUMERATOR(Y)
VE_ENUM_END();

VE_ENUM(::Veng::ActionPhase, 0x0FE53BE3AFAF21AAULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(Started)
VE_ENUMERATOR(Ongoing)
VE_ENUMERATOR(Completed)
VE_ENUM_END();

VE_REFLECT(::Veng::InputAction, 0xC81225F15105A79FULL)
VE_FIELD(Id, .DisplayName = "Id",
         .Tooltip = "Stable minted action identity bindings and control code reference.")
VE_FIELD(Name, .DisplayName = "Name",
         .Tooltip = "Display label; on-disk identity is Id, not this name.")
VE_FIELD(Kind, .DisplayName = "Kind",
         .Tooltip = "Value shape the action resolves to (button, 1D axis, 2D axis).")
VE_REFLECT_END();

VE_REFLECT(::Veng::InputSource, 0x715BCFCB9DC23625ULL)
VE_FIELD(Device, .DisplayName = "Device",
         .Tooltip = "Raw device the Control code is interpreted against.")
VE_FIELD(Control, .DisplayName = "Control",
         .Tooltip = "Key / mouse-button / mouse-axis / gamepad control code.")
VE_REFLECT_END();

VE_REFLECT(::Veng::Binding, 0x700B5FF73EEE3953ULL)
VE_FIELD(Source, .DisplayName = "Source", .Tooltip = "The raw control this binding reads.",
         .Category = "Source")
VE_FIELD(Action, .DisplayName = "Action", .Tooltip = "The action this binding contributes to.",
         .Category = "Mapping")
VE_FIELD(Axis, .DisplayName = "Axis",
         .Tooltip = "Which action component the source drives (Whole for a native axis).",
         .Category = "Mapping")
VE_FIELD(Scale, .DisplayName = "Scale",
         .Tooltip = "Signed scale applied to the source before accumulation (negative inverts).",
         .Category = "Mapping")
VE_REFLECT_END();

VE_REFLECT(::Veng::ActionSample, 0xCCB2AAE2234FF034ULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Value, .DisplayName = "Value")
VE_FIELD(Phase, .DisplayName = "Phase")
VE_REFLECT_END();

VE_REFLECT(::Veng::ActionState, 0x0F90317083A015CEULL)
VE_ARRAY_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();

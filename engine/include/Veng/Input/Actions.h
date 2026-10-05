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

    /// @brief The engine meaning an action carries, beyond the value a control system reads.
    ///
    /// The engine binds no key: an application declares an action, tags it with a role, and binds
    /// it in its own input map, and the engine resolves every role-tagged action each frame and acts
    /// on its press. The navigation roles drive focus in the interactive Gui documents of the seat
    /// that pressed them, while that seat holds UI focus; ReleaseFocus hands a seat holding gameplay
    /// focus back to the UI. The two never fire under the same focus, so one control may carry both
    /// a Cancel action and a ReleaseFocus action.
    enum class ActionRole : u32
    {
        /// @brief No engine meaning: the action is read only by the application's own code.
        None,
        /// @brief Moves Gui focus to the nearest focusable above the current one.
        NavigateUp,
        /// @brief Moves Gui focus to the nearest focusable below the current one.
        NavigateDown,
        /// @brief Moves Gui focus to the nearest focusable left of the current one.
        NavigateLeft,
        /// @brief Moves Gui focus to the nearest focusable right of the current one.
        NavigateRight,
        /// @brief Moves Gui focus to the next focusable in tree order.
        NavigateNext,
        /// @brief Moves Gui focus to the previous focusable in tree order.
        NavigatePrevious,
        /// @brief Activates the focused Gui element.
        Confirm,
        /// @brief Raises a cancel the focused Gui element or an open popup consumes.
        Cancel,
        /// @brief Releases the gameplay focus of the seat that pressed it, freeing the cursor.
        ///
        /// Fires only while that seat holds gameplay focus as the frame begins. The press stays
        /// held across the release, so it never also cancels in the UI it uncovers.
        ReleaseFocus
    };

    /// @brief Whether a role drives Gui focus navigation, so it fires only under UI focus.
    /// @param role  The role to test.
    /// @return True for every role from NavigateUp through Cancel; false for None and ReleaseFocus.
    [[nodiscard]] constexpr bool IsNavigationRole(const ActionRole role)
    {
        return role >= ActionRole::NavigateUp && role <= ActionRole::Cancel;
    }

    /// @brief One action a context declares: its id, display name, value shape, and engine role.
    struct InputAction
    {
        /// @brief The action's stable identity, referenced by bindings and control code.
        ActionId Id = ActionId::Null;

        /// @brief Display/authoring label; on-disk identity is Id, not this name.
        string Name;

        /// @brief The value shape this action resolves to.
        ActionKind Kind = ActionKind::Button;

        /// @brief The engine meaning of a press of this action; None for an application-only action.
        ///
        /// A role action fires once when it activates, and again on each repeat while held when
        /// RepeatRate is set. A role action is a Button: a stick drives one through a binding's
        /// Threshold on its half-axis.
        ActionRole Role = ActionRole::None;

        /// @brief Seconds a role action is held before its first repeat.
        ///
        /// Read only when RepeatRate is greater than 0; 0 places the first repeat one RepeatRate
        /// after the press.
        f32 RepeatDelay = 0.0f;

        /// @brief Seconds between the repeats of a held role action; 0 (the default) never repeats.
        ///
        /// The engine times the repeat itself, once per action per seat, so a pad repeats exactly as
        /// a key does and the platform's key auto-repeat plays no part. At most one repeat fires per
        /// frame, so a long frame never bursts several.
        f32 RepeatRate = 0.0f;
    };

    /// @brief Which raw device a binding reads.
    enum class InputDeviceType : u32
    {
        /// @brief A keyboard key (Control is a key code).
        Keyboard,
        /// @brief A mouse button (Control is a button index).
        MouseButton,
        /// @brief A mouse motion axis (Control selects the axis).
        MouseAxis,
        /// @brief A gamepad button (Control is a GamepadButton index).
        GamepadButton,
        /// @brief A gamepad analog axis (Control is a GamepadAxis index).
        GamepadAxis,
        /// @brief No device: the unset sentinel, which reads neutral.
        ///
        /// A binding's Modifier defaults to it, meaning "no modifier"; a binding's own Source never
        /// names it.
        None
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

    /// @brief One raw-source → action mapping with its shaping.
    ///
    /// A scalar source contributes to one action component with a signed scale (a negative
    /// Scale inverts, so there is no separate invert flag); a native axis (AxisComponent::Whole)
    /// drives its action directly. This covers WASD → a 2D Move action and a stick → the same
    /// action.
    ///
    /// The source's value v (a pad's already passed through the device deadzones, see
    /// Input::IngestGamepadStates) is scaled first, s = Scale · v, and the binding then shapes s by
    /// the kind of action it drives:
    /// - **A Button action** reads the source as a half-axis: it contributes 1 when s > 0 and
    ///   s ≥ Threshold, else 0. A resting source (s = 0) never presses, whatever the threshold, and
    ///   a stick axis bound with Scale −1 to one button and +1 to another fires each only on its own
    ///   half.
    /// - **An axis action** takes nothing from a source with |s| < Threshold; above it the value
    ///   passes through the response curve sign(s) · |s|^Exponent, with no rescale at the threshold.
    ///
    /// The defaults (Threshold 0, Exponent 1) leave an axis binding's value unchanged.
    ///
    /// A binding with a Modifier is a **chord**: it contributes only while its modifier is down,
    /// the modifier read as a button on its positive half-axis at ModifierThreshold. While a chord
    /// is **live** (its modifier is down and its action resolves from the chord's context, not
    /// shadowed by a higher one), every plain binding on the same Source, in any active context,
    /// contributes nothing, so the chord replaces the plain meaning of its control rather than
    /// adding to it. A chord never suppresses another chord.
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

        /// @brief The scaled value a source must reach to count, 0 or more.
        ///
        /// For a Button action, the pull point that presses it; for an axis action, the magnitude
        /// under which the source contributes nothing.
        f32 Threshold = 0.0f;

        /// @brief The response curve's exponent on an axis action, greater than 0.
        ///
        /// Above 1 it gives fine control near centre (2 maps a half deflection to a quarter);
        /// 1 is linear. A Button action ignores it.
        f32 Exponent = 1.0f;

        /// @brief The control that must be held for this binding to contribute.
        ///
        /// A Device of InputDeviceType::None (the default) means no modifier: a plain binding.
        InputSource Modifier{.Device = InputDeviceType::None};

        /// @brief The value the modifier must reach to count as down, 0 or more.
        ///
        /// The modifier reads as a button does: down when its value v > 0 and v ≥ this, so a key,
        /// mouse or pad button (v = 1) is down whenever held, and a trigger or stick half-axis is
        /// down past this pull. Separate from Threshold, which shapes the binding's own Source.
        f32 ModifierThreshold = 0.5f;
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

        /// @brief Whether the action started (Started) on any Sim tick since this frame began.
        ///
        /// The frame-accumulated companion to Phase == Started: InputMappingSystem ORs this across
        /// every Sim tick of a frame and resets it on the frame's first tick, so a once-per-frame
        /// reader (a View system) sees a Started edge that landed on a non-final tick of a multi-tick
        /// frame — which the single-valued Phase would have overwritten. Not reflected: a transient,
        /// locally-derived view of Phase, never cooked or replicated.
        bool StartedThisFrame = false;

        /// @brief Whether the action released (Completed) on any Sim tick since this frame began.
        ///
        /// The frame-accumulated companion to Phase == Completed, maintained exactly as
        /// StartedThisFrame. Not reflected.
        bool ReleasedThisFrame = false;
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
        ///
        /// The per-tick edge, for a Sim system that runs every tick and observes each pulse once.
        /// A once-per-frame reader (a View system) wants WasTriggeredThisFrame instead — this pulse
        /// is overwritten by a later tick of the same frame.
        /// @param id  The action to look up.
        /// @return True on the tick the action activated.
        [[nodiscard]] bool WasTriggered(ActionId id) const;

        /// @brief Whether an action was released this tick (Completed).
        ///
        /// The per-tick edge; see WasTriggered for the Sim-vs-View distinction.
        /// @param id  The action to look up.
        /// @return True on the tick the action released.
        [[nodiscard]] bool WasReleased(ActionId id) const;

        /// @brief Whether an action started on any Sim tick since this frame began.
        ///
        /// The frame-accumulated edge a once-per-frame reader (a View system, or per-frame
        /// application code) samples: under the fixed timestep a frame runs 0..N Sim ticks and a
        /// Started pulse on a non-final tick is overwritten in Phase before the frame's single View
        /// pass reads it. This survives that, reading the ORed StartedThisFrame the InputMappingSystem
        /// maintains. A per-tick Sim system uses WasTriggered.
        /// @param id  The action to look up.
        /// @return True if the action activated on any tick this frame.
        [[nodiscard]] bool WasTriggeredThisFrame(ActionId id) const;

        /// @brief Whether an action released on any Sim tick since this frame began.
        ///
        /// The frame-accumulated release edge; the companion to WasTriggeredThisFrame, and the query
        /// a once-per-frame click/release consumer must use so a Completed pulse on a non-final tick
        /// of a multi-tick frame is not lost.
        /// @param id  The action to look up.
        /// @return True if the action released on any tick this frame.
        [[nodiscard]] bool WasReleasedThisFrame(ActionId id) const;
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

        /// @brief Whether this context resolves only while its seat holds gameplay focus.
        ///
        /// A gated context (authored `requiresGameplayFocus`) is excluded from a seat's effective
        /// active list whenever the seat is not gameplay-focused — a pure evaluation the
        /// InputMappingSystem performs at list assembly, never a mutation of the authored
        /// InputContextStack. False (the default) leaves a context always active, the status quo.
        bool RequiresGameplayFocus = false;
    };

    /// @brief Whether a resolved context contributes bindings under the current focus.
    ///
    /// The focus gate as a pure predicate: a context requiring gameplay focus is active only while
    /// the seat is gameplay-focused; an ungated context is always active. The InputMappingSystem
    /// filters a seat's stack through this before ResolveActions, so a gate never touches the
    /// authored active list — device-free, testable in isolation.
    /// @param context          The resolved context to test.
    /// @param gameplayFocused  Whether the seat currently holds gameplay focus.
    /// @return True when @p context should contribute to the seat's resolution this tick.
    [[nodiscard]] inline bool IsContextActiveUnderFocus(const ResolvedContext& context,
                                                        const bool gameplayFocused)
    {
        return gameplayFocused || !context.RequiresGameplayFocus;
    }

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
    /// action's component bindings, shapes each binding's source by its scale, threshold and
    /// curve, gates a chord on its modifier and silences the plain bindings a live chord shares a
    /// source with (see Binding), and derives each action's phase by comparing this tick's
    /// activation against previous — the seat's ActionState from last tick — so phase needs no
    /// stateful adapter and works for axis actions.
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
VE_ENUMERATOR(None)
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

VE_ENUM(::Veng::ActionRole, 0x7AED104C373382F9ULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(NavigateUp)
VE_ENUMERATOR(NavigateDown)
VE_ENUMERATOR(NavigateLeft)
VE_ENUMERATOR(NavigateRight)
VE_ENUMERATOR(NavigateNext)
VE_ENUMERATOR(NavigatePrevious)
VE_ENUMERATOR(Confirm)
VE_ENUMERATOR(Cancel)
VE_ENUMERATOR(ReleaseFocus)
VE_ENUM_END();

VE_REFLECT(::Veng::InputAction, 0xC81225F15105A79FULL)
VE_FIELD(Id, .DisplayName = "Id",
         .Tooltip = "Stable minted action identity bindings and control code reference.")
VE_FIELD(Name, .DisplayName = "Name",
         .Tooltip = "Display label; on-disk identity is Id, not this name.")
VE_FIELD(Kind, .DisplayName = "Kind",
         .Tooltip = "Value shape the action resolves to (button, 1D axis, 2D axis).")
VE_FIELD(Role, .DisplayName = "Role",
         .Tooltip = "What the engine does on a press: navigate, confirm or cancel in the seat's "
                    "Gui documents, or release its gameplay focus. None for an action only the "
                    "game reads.",
         .Category = "Role")
VE_FIELD(RepeatDelay, .DisplayName = "Repeat Delay",
         .Tooltip = "Seconds held before a role action first repeats; read only with a Repeat "
                    "Rate. 0 waits one Repeat Rate.",
         .Category = "Role")
VE_FIELD(RepeatRate, .DisplayName = "Repeat Rate",
         .Tooltip = "Seconds between repeats while a role action is held; 0 never repeats.",
         .Category = "Role")
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
VE_FIELD(Threshold, .DisplayName = "Threshold",
         .Tooltip = "Scaled value the source must reach. A button action presses at it, on the "
                    "positive half only; an axis action ignores a source below it.",
         .Category = "Shaping")
VE_FIELD(Exponent, .DisplayName = "Exponent",
         .Tooltip = "Response curve sign(s)*|s|^Exponent on an axis action; above 1 gives fine "
                    "control near centre. Ignored by a button action.",
         .Category = "Shaping")
VE_FIELD(Modifier, .DisplayName = "Modifier",
         .Tooltip = "A control that must be held for this binding to count (a chord); Device "
                    "None means none. While it is held, plain bindings on the same source are "
                    "silent.",
         .Category = "Chord")
VE_FIELD(ModifierThreshold, .DisplayName = "Modifier Threshold",
         .Tooltip = "Value the modifier must reach to count as held, on its positive half; a "
                    "key or button is held at any value up to 1.",
         .Category = "Chord")
VE_REFLECT_END();

VE_REFLECT(::Veng::ActionSample, 0xCCB2AAE2234FF034ULL)
VE_FIELD(Id, .DisplayName = "Id")
VE_FIELD(Value, .DisplayName = "Value")
VE_FIELD(Phase, .DisplayName = "Phase")
VE_REFLECT_END();

VE_REFLECT(::Veng::ActionState, 0x0F90317083A015CEULL)
VE_ARRAY_FIELD(Actions, .DisplayName = "Actions")
VE_REFLECT_END();

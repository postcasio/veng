#pragma once

#include <array>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief Keyboard key codes.
    ///
    /// Engine-owned so consumers need not include GLFW. Values are GLFW-compatible;
    /// the backend mapping in Window.cpp is a direct cast.
    enum class Key : u16
    {
        Space = 32,
        Apostrophe = 39,
        Comma = 44,
        Minus = 45,
        Period = 46,
        Slash = 47,
        Num0 = 48,
        Num1,
        Num2,
        Num3,
        Num4,
        Num5,
        Num6,
        Num7,
        Num8,
        Num9,
        Semicolon = 59,
        Equal = 61,
        A = 65,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,
        LeftBracket = 91,
        Backslash = 92,
        RightBracket = 93,
        GraveAccent = 96,
        Escape = 256,
        Enter = 257,
        Tab = 258,
        Backspace = 259,
        Insert = 260,
        Delete = 261,
        Right = 262,
        Left = 263,
        Down = 264,
        Up = 265,
        PageUp = 266,
        PageDown = 267,
        Home = 268,
        End = 269,
        F1 = 290,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        LeftShift = 340,
        LeftControl = 341,
        LeftAlt = 342,
        LeftSuper = 343,
        RightShift = 344,
        RightControl = 345,
        RightAlt = 346,
        RightSuper = 347,
    };

    /// @brief Mouse button codes; values match GLFW constants.
    enum class MouseButton : u8
    {
        Left = 0,
        Right = 1,
        Middle = 2,
    };

    /// @brief Identity of a connected gamepad: the GLFW joystick slot, stable while it stays connected.
    ///
    /// The raw slot (0..15), not a dense index over the connected set: a slot is reused only after
    /// its pad disconnects, so a persisted assignment never silently re-points at a different pad.
    enum class GamepadId : u32
    {
        /// @brief The empty id, distinct from every real slot.
        None = 0xFFFFFFFFu
    };

    /// @brief Engine gamepad button vocabulary (mapped to GLFW_GAMEPAD_BUTTON_* at the poll boundary).
    enum class GamepadButton : u32
    {
        A,
        B,
        X,
        Y,
        LeftBumper,
        RightBumper,
        Back,
        Start,
        Guide,
        LeftThumb,
        RightThumb,
        DpadUp,
        DpadRight,
        DpadDown,
        DpadLeft,
        /// @brief Count of buttons; sizes the per-pad button array, excluded from the authored set.
        Count
    };

    /// @brief Engine gamepad axis vocabulary (mapped to GLFW_GAMEPAD_AXIS_* at the poll boundary).
    enum class GamepadAxis : u32
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        /// @brief Count of axes; sizes the per-pad axis array, excluded from the authored set.
        Count
    };

    /// @brief One pad's raw state for a frame: the poll target, a GLFW-free struct.
    ///
    /// Sticks report −1..1 and triggers 0..1; Buttons/Axes index by GamepadButton / GamepadAxis so
    /// the arrays size off the enum Count sentinel and cannot drift when a control is added.
    struct GamepadState
    {
        /// @brief Whether this slot holds a connected, gamepad-mapped pad this frame.
        bool Connected = false;
        /// @brief Per-button held state, indexed by GamepadButton.
        std::array<bool, usize(GamepadButton::Count)> Buttons{};
        /// @brief Per-axis value, indexed by GamepadAxis (sticks −1..1, triggers 0..1).
        std::array<f32, usize(GamepadAxis::Count)> Axes{};
    };

    class Window;
    class Event;

    /// @brief Frame-coherent input service, always present, updated once per frame.
    ///
    /// Mirrors Time: a per-frame service the run loop pumps before OnUpdate/OnRender.
    /// It holds current and previous key/button state plus per-frame mouse/scroll deltas,
    /// so callers get per-frame edges (WasKeyPressed/WasKeyReleased) and deltas. State is
    /// **event-fed, not polled**: BeginFrame rolls the snapshot forward and the InputRouter
    /// applies this frame's routed events via ApplyEvent. It is driven from the single
    /// render thread like the rest of veng. The borrowed window is nullable: with no window
    /// (a headless run) no events arrive, so the snapshot stays the neutral all-zeros input
    /// a windowed app produces with nothing pressed.
    ///
    /// Because the router applies only the events routed to it, this snapshot reflects input
    /// **only while its layer owns focus** — a gameplay-focused snapshot sees the game's
    /// input exclusively; a UI-focused one sees input the editor camera reads.
    class Input
    {
    public:
        /// @brief Constructs the input service borrowing the given window.
        /// @param window  Window borrowed for cursor capture; nullptr for a headless run that
        ///                reports the neutral all-zeros state. Must outlive this when non-null.
        explicit Input(Window* window);

        /// @brief Rolls the snapshot forward for a new frame; called once at the top of the loop.
        ///
        /// Copies current key/button/pad state to previous, so the edges the router then applies via
        /// ApplyEvent are this frame's. With no events applied the state stays neutral (nothing
        /// pressed). The per-frame mouse and scroll deltas clear here **unconditionally**, on every
        /// frame: they are a once-per-frame quantity and no gate applies to them. The Sim-tick
        /// deltas are a separate cadence entirely — see BeginSimTick.
        ///
        /// @p rollEdges gates that roll on whether the previous frame consumed the edges. Under a
        /// fixed-timestep drive a frame can run zero Sim ticks (frame rate above the tick rate), and an
        /// input state a Sim system reads by level (a held key/button, the source of every action) must
        /// survive until a frame that runs a tick reads it. The action resolver samples level, not
        /// edges, so a press then release entirely between two ticks would leave the level low at every
        /// tick and the action would never fire: a released key/button whose press has not yet crossed a
        /// roll therefore holds its down level (its release is *deferred*) and is applied on the next
        /// roll, guaranteeing every physical press is observed down for at least one tick and released on
        /// a later one. The caller passes false to hold the latched state while no tick ran, and true
        /// once a tick has consumed it (which then applies any deferred releases). A key/button held
        /// across ticks is unaffected either way.
        /// @param rollEdges  True to roll edges and apply deferred releases this frame (the previous
        ///                   frame ran a Sim tick); false to hold them latched for the next
        ///                   tick-running frame.
        /// @pre Must run before the event drain so ApplyEvent writes into a fresh frame.
        void BeginFrame(bool rollEdges = true);

        /// @brief Latches the pointer motion accumulated since the previous Sim tick as this tick's delta.
        ///
        /// The pointer deltas have **two cadences**, because they are consumed at two rates. A frame
        /// consumer (a UI drag, a debug panel, an editor camera) is sampled once per frame and reads
        /// GetMouseDelta / GetScrollDelta. A fixed-rate Sim consumer runs at the tick rate, which is
        /// unrelated to the frame rate — so reading the per-frame delta from a Sim tick drops the
        /// motion of every frame no tick observed and counts one frame's motion once per tick when a
        /// frame runs several. Motion therefore also accumulates into a separate running total that
        /// only this call consumes: it moves the total into the per-tick delta (GetSimMouseDelta /
        /// GetSimScrollDelta) and zeroes the total, so a tick's delta is exactly the motion since the
        /// previous tick at any frame-rate-to-tick-rate ratio, and stays stable for the whole tick
        /// however many systems read it.
        ///
        /// @pre Called once per Sim step, before the step's systems run — and for **one** stepping
        ///      consumer of the pointer, since the call consumes the shared accumulation. The engine
        ///      calls it for the steps of the single world the pointer routes to; a second world's
        ///      steps must not, or the routed world is left with nothing.
        void BeginSimTick();

        /// @brief Discards the accumulated and latched Sim deltas without a tick consuming them.
        ///
        /// The counterpart of BeginSimTick for a frame no Sim step ran on because nothing was
        /// simulating: motion made while the simulation is stopped or paused must not bank into the
        /// delta the resuming tick reads, which would arrive as one jump of the whole stopped
        /// stretch's travel. A frame that ran no step merely because the accumulator has not yet
        /// filled a tick calls neither, holding the motion for the tick-running frame that follows.
        void DropSimDeltas();

        /// @brief Folds one input event into the current snapshot.
        ///
        /// The general event-injection entry point: the InputRouter calls it for each key/mouse
        /// event routed to this snapshot, and it is equally the seam a synthetic-input driver
        /// (a test harness, an automation tool) feeds fabricated events through, so an injected
        /// event is indistinguishable from a routed one — the same key/button/delta state the
        /// action layer then resolves against. Non-input events are ignored. Out-of-range
        /// key/button codes are guarded, never an out-of-bounds write.
        /// @param event  The event to apply.
        /// @pre Runs on the render thread, between BeginFrame and the frame's action resolution,
        ///      so the event lands in the same frame the game reads.
        void ApplyEvent(const Event& event);

        /// @brief Returns true if the given key is currently held down.
        [[nodiscard]] bool IsKeyDown(Key key) const;

        /// @brief Returns true only on the frame the key transitioned from up to down.
        [[nodiscard]] bool WasKeyPressed(Key key) const;

        /// @brief Returns true only on the frame the key transitioned from down to up.
        [[nodiscard]] bool WasKeyReleased(Key key) const;

        /// @brief Returns true if the given mouse button is currently held down.
        [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const;

        /// @brief Returns true only on the frame the button transitioned from up to down.
        [[nodiscard]] bool WasMouseButtonPressed(MouseButton button) const;

        /// @brief Returns true only on the frame the button transitioned from down to up.
        [[nodiscard]] bool WasMouseButtonReleased(MouseButton button) const;

        /// @brief Returns the current mouse cursor position in window-space pixels.
        [[nodiscard]] vec2 GetMousePosition() const;

        /// @brief Returns the change in mouse position since the previous frame, in pixels.
        ///
        /// Works while the mouse is captured: relative motion accumulates so a fly
        /// camera reads continuous deltas with the OS cursor hidden and locked.
        ///
        /// This is the **per-frame** cadence, for a consumer sampled once per frame. A fixed-rate
        /// Sim consumer reads GetSimMouseDelta instead — see BeginSimTick for why the two differ.
        [[nodiscard]] vec2 GetMouseDelta() const;

        /// @brief Returns the scroll wheel delta accumulated this frame as (x, y).
        ///
        /// The **per-frame** cadence, like GetMouseDelta; GetSimScrollDelta is the per-tick one.
        [[nodiscard]] vec2 GetScrollDelta() const;

        /// @brief Returns the mouse motion latched for the current Sim tick, in pixels.
        ///
        /// The motion accumulated since the previous tick, moved here by BeginSimTick and constant
        /// for the whole tick, so every Sim system reading it within one tick agrees and the sum
        /// across ticks is the whole injected motion whatever the frame-to-tick ratio.
        [[nodiscard]] vec2 GetSimMouseDelta() const;

        /// @brief Returns the scroll wheel delta latched for the current Sim tick as (x, y).
        ///
        /// The per-tick counterpart of GetScrollDelta. A wheel notch is a count, so summing the
        /// notches since the previous tick is what a tick wants: read per-frame instead, a single
        /// notch is applied once per tick of a multi-tick frame.
        [[nodiscard]] vec2 GetSimScrollDelta() const;

        /// @brief Captures or releases the mouse cursor.
        ///
        /// Captured hides and locks the OS cursor and accumulates relative motion,
        /// the mode a fly camera drives in. Delegates to the window; no-ops with no
        /// window.
        /// @param captured  True to capture, false to release.
        void SetMouseCaptured(bool captured);

        /// @brief Returns true if the mouse cursor is currently captured.
        [[nodiscard]] bool IsMouseCaptured() const;

        /// @brief Shows or hides the free (uncaptured) OS cursor.
        ///
        /// Hidden, the cursor still moves and reports positions normally — it just is
        /// not drawn, so an app can render its own software cursor. Independent of
        /// capture (a captured cursor is always hidden); set while captured, it takes
        /// effect on release. Delegates to the window; no-ops with no window.
        /// @param visible  True to draw the OS cursor when free, false to hide it.
        void SetCursorVisible(bool visible);

        /// @brief Returns whether the free (uncaptured) OS cursor is drawn.
        [[nodiscard]] bool IsCursorVisible() const;

        /// @brief Replaces this frame's polled gamepad state for every slot.
        ///
        /// The window layer polls each present joystick once per frame and hands the full
        /// slot-indexed set here; a slot with no connected pad carries a default (unconnected)
        /// GamepadState. Called after BeginFrame's roll so the previous-frame button bits the
        /// pressed-edge query reads are already captured. Headless never calls it, so the pad
        /// surface stays the neutral no-pads state.
        /// @param states  One GamepadState per slot; must span all slots the queries can name.
        void IngestGamepadStates(std::span<const GamepadState> states);

        /// @brief Returns true if the given slot currently holds a connected pad.
        [[nodiscard]] bool IsGamepadConnected(GamepadId id) const;

        /// @brief Returns true if the given pad's button is currently held down.
        ///
        /// Reports neutral for a stale or disconnected slot, so a query by an id whose pad
        /// left never reads a reused slot's new occupant.
        [[nodiscard]] bool IsGamepadButtonDown(GamepadId id, GamepadButton button) const;

        /// @brief Returns true only on the frame the pad's button transitioned from up to down.
        [[nodiscard]] bool WasGamepadButtonPressed(GamepadId id, GamepadButton button) const;

        /// @brief Returns the given pad's axis value (sticks −1..1, triggers 0..1), zero if absent.
        [[nodiscard]] f32 GetGamepadAxis(GamepadId id, GamepadAxis axis) const;

        /// @brief Returns the slots currently holding a connected pad, in ascending slot order.
        [[nodiscard]] std::span<const GamepadId> ConnectedGamepads() const;

    private:
        /// @brief Highest GLFW key code, sizing the key state bitsets.
        static constexpr usize MaxKeys = 512;
        /// @brief Number of tracked mouse buttons.
        static constexpr usize MaxMouseButtons = 8;
        /// @brief Number of GLFW joystick slots, sizing the slot-indexed pad state.
        static constexpr usize MaxGamepads = 16;

        /// @brief Returns the per-slot state for a valid slot id, or nullptr for None/out-of-range.
        [[nodiscard]] const GamepadState* PadFor(GamepadId id) const;

        /// @brief Borrowed window polled for input state; nullptr in a headless run.
        Window* m_Window;

        /// @brief Per-key held state this frame, indexed by key code.
        std::array<bool, MaxKeys> m_Keys{};
        /// @brief Per-key held state last frame, indexed by key code.
        std::array<bool, MaxKeys> m_PreviousKeys{};
        /// @brief Whether a key was pressed since the last roll, gating its release deferral.
        std::array<bool, MaxKeys> m_KeyPressedSinceRoll{};
        /// @brief A key whose release is held (its press has not yet crossed a roll), applied next roll.
        std::array<bool, MaxKeys> m_KeyReleaseDeferred{};

        /// @brief Per-button held state this frame, indexed by button code.
        std::array<bool, MaxMouseButtons> m_MouseButtons{};
        /// @brief Per-button held state last frame, indexed by button code.
        std::array<bool, MaxMouseButtons> m_PreviousMouseButtons{};
        /// @brief Whether a button was pressed since the last roll, gating its release deferral.
        std::array<bool, MaxMouseButtons> m_MousePressedSinceRoll{};
        /// @brief A button whose release is held (its press has not yet crossed a roll), applied next roll.
        std::array<bool, MaxMouseButtons> m_MouseReleaseDeferred{};

        /// @brief Mouse position this frame, in window-space pixels.
        vec2 m_MousePosition = {0, 0};
        /// @brief Accumulated mouse motion this frame, summed across this frame's move events.
        vec2 m_MouseDelta = {0, 0};

        /// @brief Scroll delta accumulated this frame.
        vec2 m_ScrollDelta = {0, 0};

        /// @brief Mouse motion summed since the last BeginSimTick, spanning however many frames that is.
        vec2 m_SimMouseAccumulator = {0, 0};
        /// @brief Scroll delta summed since the last BeginSimTick, spanning however many frames that is.
        vec2 m_SimScrollAccumulator = {0, 0};
        /// @brief The mouse motion BeginSimTick latched for the current Sim tick.
        vec2 m_SimMouseDelta = {0, 0};
        /// @brief The scroll delta BeginSimTick latched for the current Sim tick.
        vec2 m_SimScrollDelta = {0, 0};

        /// @brief False until the first move event seeds m_MousePosition, so the opening move reports no delta.
        bool m_HavePosition = false;

        /// @brief Per-slot pad state this frame, filled by IngestGamepadStates.
        std::array<GamepadState, MaxGamepads> m_Gamepads{};
        /// @brief Per-slot button bits last frame, for the pad pressed-edge query.
        std::array<std::array<bool, usize(GamepadButton::Count)>, MaxGamepads>
            m_PreviousGamepadButtons{};
        /// @brief Slots marked Connected this frame, rebuilt in IngestGamepadStates for ConnectedGamepads.
        vector<GamepadId> m_ConnectedGamepads;
    };
}

VE_ENUM(::Veng::GamepadId, 0xA37F694012413532ULL)
VE_ENUMERATOR(None)
VE_ENUM_END();

VE_ENUM(::Veng::GamepadButton, 0xA40829295182994CULL)
VE_ENUMERATOR(A)
VE_ENUMERATOR(B)
VE_ENUMERATOR(X)
VE_ENUMERATOR(Y)
VE_ENUMERATOR(LeftBumper)
VE_ENUMERATOR(RightBumper)
VE_ENUMERATOR(Back)
VE_ENUMERATOR(Start)
VE_ENUMERATOR(Guide)
VE_ENUMERATOR(LeftThumb)
VE_ENUMERATOR(RightThumb)
VE_ENUMERATOR(DpadUp)
VE_ENUMERATOR(DpadRight)
VE_ENUMERATOR(DpadDown)
VE_ENUMERATOR(DpadLeft)
VE_ENUM_END();

VE_ENUM(::Veng::GamepadAxis, 0x512AC8C48915CA9EULL)
VE_ENUMERATOR(LeftX)
VE_ENUMERATOR(LeftY)
VE_ENUMERATOR(RightX)
VE_ENUMERATOR(RightY)
VE_ENUMERATOR(LeftTrigger)
VE_ENUMERATOR(RightTrigger)
VE_ENUM_END();

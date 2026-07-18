#pragma once

#include <Veng/Veng.h>

namespace Veng::Gui
{
    struct Element;

    /// @brief The pointer button a pointer event carries.
    ///
    /// The document routing is button-aware so a control can distinguish a primary click from a
    /// secondary (context) one; Primary is the button a synthesized activation raises.
    enum class PointerButton : u8
    {
        /// @brief The primary (left) button — the one a click and a synthesized activation raise.
        Primary,
        /// @brief The secondary (right) button — a context action.
        Secondary,
        /// @brief The middle button.
        Middle,
    };

    /// @brief The modifier keys held while a pointer or navigation action was raised.
    ///
    /// The bits are a bitmask (combine with the bitwise operators). They carry the chord a
    /// selection model reads — Control (or Meta on a platform whose convention is the command key)
    /// toggles one item, Shift extends a range from the anchor — so the input seam maps the
    /// physical keys once and the document applies the meaning.
    enum class InputModifiers : u8
    {
        /// @brief No modifier key was held.
        None = 0,
        /// @brief The shift key was held.
        Shift = 1u << 0,
        /// @brief The control key was held.
        Control = 1u << 1,
        /// @brief The alt/option key was held.
        Alt = 1u << 2,
        /// @brief The meta (command/super/windows) key was held.
        Meta = 1u << 3,
    };

    /// @brief Returns the union of two modifier masks.
    /// @param a  The first mask.
    /// @param b  The second mask.
    /// @return The bitwise-or of the two masks.
    constexpr InputModifiers operator|(InputModifiers a, InputModifiers b)
    {
        return static_cast<InputModifiers>(static_cast<u8>(a) | static_cast<u8>(b));
    }

    /// @brief Returns the intersection of two modifier masks.
    /// @param a  The first mask.
    /// @param b  The second mask.
    /// @return The bitwise-and of the two masks.
    constexpr InputModifiers operator&(InputModifiers a, InputModifiers b)
    {
        return static_cast<InputModifiers>(static_cast<u8>(a) & static_cast<u8>(b));
    }

    /// @brief Returns whether every bit of a modifier is set in a mask.
    /// @param mask      The mask to test.
    /// @param modifier  The modifier bit (or bits) to look for.
    /// @return True when the mask carries the modifier.
    constexpr bool HasModifier(InputModifiers mask, InputModifiers modifier)
    {
        return (mask & modifier) == modifier;
    }

    /// @brief The kind of pointer transition a pointer event reports.
    ///
    /// Move retargets hover; Down/Up drive the pressed state and, when a Down and its Up land on
    /// the same element, synthesize a Click. Enter/Leave are produced by the router as hover moves
    /// between elements — an author routes only Move/Down/Up and reads Enter/Leave as the hover
    /// transitions they cause.
    enum class PointerEventKind : u8
    {
        /// @brief The pointer moved to a new document-space position.
        Move,
        /// @brief A pointer button transitioned to pressed.
        Down,
        /// @brief A pointer button transitioned to released.
        Up,
        /// @brief The pointer entered an element's box (hover began).
        Enter,
        /// @brief The pointer left an element's box (hover ended).
        Leave,
        /// @brief A Down and Up landed on the same element (a completed click).
        Click,
    };

    /// @brief One pointer transition routed through a document, in document-space pixels.
    ///
    /// Position is the pointer in the document's own coordinate space (the same space
    /// Element::Layout rects live in). The routing walks the element path capture (root→target)
    /// then bubble (target→root), setting Handled on the first handler that consumes it to stop
    /// propagation.
    struct PointerEvent
    {
        /// @brief The transition kind.
        PointerEventKind Kind = PointerEventKind::Move;
        /// @brief The button, meaningful for Down/Up/Click; ignored for Move/Enter/Leave.
        PointerButton Button = PointerButton::Primary;
        /// @brief The pointer position, in document-space pixels.
        vec2 Position{0.0f};
        /// @brief The modifier keys held when the transition was raised.
        InputModifiers Modifiers = InputModifiers::None;
        /// @brief The element the routing reached; the hit-test target, or the entered/left element.
        Element* Target = nullptr;
        /// @brief Set by a handler to consume the event and stop further propagation.
        bool Handled = false;
    };

    /// @brief A logical navigation action a key or gamepad control raises against a document.
    ///
    /// The focus model is expressed in these abstract actions so keyboard and gamepad drive one
    /// path: an arrow key or a d-pad direction both raise a Move*; Enter or the gamepad confirm
    /// button both raise Confirm; Escape or the cancel button both raise Cancel. Tab/Shift-Tab map
    /// to Next/Previous.
    enum class NavAction : u8
    {
        /// @brief Move focus to the nearest focusable above the current one.
        MoveUp,
        /// @brief Move focus to the nearest focusable below the current one.
        MoveDown,
        /// @brief Move focus to the nearest focusable left of the current one.
        MoveLeft,
        /// @brief Move focus to the nearest focusable right of the current one.
        MoveRight,
        /// @brief Move focus to the next focusable in tree order (Tab).
        Next,
        /// @brief Move focus to the previous focusable in tree order (Shift-Tab).
        Previous,
        /// @brief Activate the focused element (synthesizes a primary click).
        Confirm,
        /// @brief Raise a cancel a menu can consume (routes as an event, no default).
        Cancel,
    };

    /// @brief A logical text-editing action a key raises against the focused text field.
    ///
    /// Editing is expressed in these abstract actions for the same reason the focus model is: the
    /// key-to-action mapping lives at the input seam and the document applies the action. Every
    /// action operates on the caret, which indexes **codepoints** of the field's UTF-8 value, so a
    /// move steps one whole glyph and a delete removes one whole glyph. There is no selection, so
    /// each action addresses exactly the caret or the codepoint adjacent to it.
    enum class TextEditAction : u8
    {
        /// @brief Move the caret one codepoint toward the start, clamping at the start.
        CaretLeft,
        /// @brief Move the caret one codepoint toward the end, clamping at the end.
        CaretRight,
        /// @brief Move the caret to the start of the value.
        CaretHome,
        /// @brief Move the caret to the end of the value.
        CaretEnd,
        /// @brief Delete the codepoint before the caret (Backspace).
        DeleteBackward,
        /// @brief Delete the codepoint after the caret (forward Delete).
        DeleteForward,
    };
}

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
}

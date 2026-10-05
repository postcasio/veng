#pragma once

#include <Veng/Veng.h>
#include <Veng/Input/SeatRef.h>

namespace Veng
{
    class Event;
    enum class ActionRole : u32;

    /// @brief The modifier keys held on a seat's keyboard when a role fired.
    ///
    /// Either side's key counts. A seat that does not hold the keyboard reads none held.
    struct ModifierKeys
    {
        /// @brief A shift key was held.
        bool Shift = false;
        /// @brief A control key was held.
        bool Control = false;
        /// @brief An alt/option key was held.
        bool Alt = false;
        /// @brief A super (command/windows) key was held.
        bool Super = false;
    };

    /// @brief One press of a role-tagged action, offered to the consumers on behalf of a seat.
    ///
    /// Raised by the engine's per-frame role resolution, never by a window event: the application's
    /// input map decides which control presses a role, and this carries only the meaning.
    struct RoleEvent
    {
        /// @brief The seat whose devices pressed the role; a null Viewer is the implicit seat.
        SeatRef Seat;
        /// @brief The role that fired.
        ActionRole Role{};
        /// @brief The modifiers held on the seat's keyboard as it fired.
        ModifierKeys Modifiers;
        /// @brief Whether this is a held action's repeat rather than its press.
        bool Repeat = false;
    };

    /// @brief A cooperative sink the InputRouter offers each UI-owned event, in priority order.
    ///
    /// The router holds an ordered list of consumers and, for an event a seat's focus routes to
    /// the UI, offers it to each consumer in turn until one accepts it. A consumer that returns
    /// true from ForwardEvent stops the fall-through; one that returns false lets the next
    /// consumer see the event. The dev/editor overlay registers first and sits above the seat
    /// model — it is offered every UI-owned event regardless of which seat holds focus. The
    /// snapshot fold the router performs alongside is independent of this list: a consumer's
    /// acceptance gates only later consumers, never the frame-coherent Input snapshot.
    ///
    /// A consumer that accepts a key press **claims** the key until it is released: the engine's
    /// role resolution reads a claimed key as up, so a key typed into a text box never also
    /// navigates. Role presses arrive separately, through ForwardRole, in the same order.
    class InputConsumer
    {
    public:
        /// @brief Virtual destructor for a borrowed-through-base consumer.
        virtual ~InputConsumer() = default;

        /// @brief Offers one UI-owned event to this consumer.
        /// @param event  The event to consume; window/system events are offered too.
        /// @return True to consume the event and stop the fall-through, false to pass it on.
        virtual bool ForwardEvent(const Event& event) = 0;

        /// @brief Offers one role press to this consumer.
        ///
        /// The engine offers a navigation role only while the pressing seat holds UI focus, so a
        /// consumer need not test focus again. The default declines.
        /// @param event  The role press and the seat it belongs to.
        /// @return True to consume the press and stop the fall-through, false to pass it on.
        virtual bool ForwardRole(const RoleEvent& event)
        {
            (void)event;
            return false;
        }

        /// @brief Notifies the consumer that the OS cursor capture state changed.
        ///
        /// The router derives capture from the keyboard/mouse seat's focus top and calls this on
        /// every registered consumer when it flips, so a consumer that polls the cursor (rather
        /// than reading forwarded events) can suspend that poll while the cursor is captured. The
        /// default does nothing; a consumer overrides it only when it needs the signal.
        /// @param captured  True when the OS cursor is captured (hidden and locked).
        virtual void OnCursorCaptured(bool captured) { (void)captured; }
    };
}

#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Event;

    /// @brief A cooperative sink the InputRouter offers each UI-owned event, in priority order.
    ///
    /// The router holds an ordered list of consumers and, for an event a seat's focus routes to
    /// the UI, offers it to each consumer in turn until one accepts it. A consumer that returns
    /// true from ForwardEvent stops the fall-through; one that returns false lets the next
    /// consumer see the event. The dev/editor overlay registers first and sits above the seat
    /// model — it is offered every UI-owned event regardless of which seat holds focus. The
    /// snapshot fold the router performs alongside is independent of this list: a consumer's
    /// acceptance gates only later consumers, never the frame-coherent Input snapshot.
    class InputConsumer
    {
    public:
        /// @brief Virtual destructor for a borrowed-through-base consumer.
        virtual ~InputConsumer() = default;

        /// @brief Offers one UI-owned event to this consumer.
        /// @param event  The event to consume; window/system events are offered too.
        /// @return True to consume the event and stop the fall-through, false to pass it on.
        virtual bool ForwardEvent(const Event& event) = 0;

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

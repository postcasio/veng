#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief A monotonically increasing u64 counter both the host and the GPU queue can signal and wait on.
    ///
    /// The asset loader signals a value as an upload completes; the render thread waits the
    /// value a resource depends on. Distinct from the binary Semaphore used for present/acquire
    /// synchronization. Single-owner (Unique) — nothing else holds a reference.
    class TimelineSemaphore
    {
    public:
        /// @brief Creates a timeline semaphore with an initial counter value.
        /// @param context      The owning context.
        /// @param initialValue Starting value of the monotonic counter.
        /// @return A unique owner of the new semaphore.
        static Unique<TimelineSemaphore> Create(Context& context, u64 initialValue = 0)
        {
            return Unique<TimelineSemaphore>(new TimelineSemaphore(context, initialValue));
        }

        /// @brief Destroys the underlying Vulkan semaphore immediately.
        ///
        /// The timeline semaphore is destroyed only after the transfer queue has been
        /// drained (via fence wait or WaitIdle), so no deferred destruction is needed.
        ~TimelineSemaphore();

        TimelineSemaphore(const TimelineSemaphore&) = delete;
        TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

        /// @brief Host-side signal: advances the counter to value.
        /// @param value  New counter value; must be greater than the current value.
        void Signal(u64 value);

        /// @brief Host-side wait: blocks until the counter reaches value.
        /// @param value  Counter value to wait for.
        void Wait(u64 value) const;

        /// @brief Returns the current counter value.
        [[nodiscard]] u64 GetValue() const;

        /// @brief Backend handle accessor. Returns a mutable ref from a const method by design —
        /// see the Native idiom in Native.h.
        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        TimelineSemaphore(Context& context, u64 initialValue);

        /// @brief Context this semaphore was created with; must outlive it.
        Context& m_Context;
        /// @brief Backend Vulkan timeline semaphore.
        Unique<Native> m_Native;
    };
}

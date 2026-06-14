#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    // A timeline semaphore: a monotonically increasing u64 counter both the host
    // and the queue can signal and wait on. The loader signals a value as an
    // upload completes; the render thread waits the value a resource depends on.
    // This is distinct from the binary Semaphore used for present/acquire sync.
    class TimelineSemaphore
    {
    public:
        // Unique, not Ref: a single-owner synchronization primitive (see the
        // Create return-type rule in Veng.h) — nothing else holds a reference.
        static Unique<TimelineSemaphore> Create(Context& context, u64 initialValue = 0)
        {
            return Unique<TimelineSemaphore>(new TimelineSemaphore(context, initialValue));
        }

        ~TimelineSemaphore();

        TimelineSemaphore(const TimelineSemaphore&) = delete;
        TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

        // Host-side signal: raise the counter to value.
        void Signal(u64 value);

        // Host-side wait: block until the counter is >= value.
        void Wait(u64 value) const;

        // The current counter value.
        [[nodiscard]] u64 GetValue() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        TimelineSemaphore(Context& context, u64 initialValue);

        Context& m_Context;
        Unique<Native> m_Native;
    };
}

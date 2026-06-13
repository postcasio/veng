#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    class Semaphore
    {
    public:
        // Unique, not Ref: a single-owner synchronization primitive (see the
        // Create return-type rule in Veng.h) — nothing else holds a reference.
        static Unique<Semaphore> Create(Context& context, const string& name)
        {
            return Unique<Semaphore>(new Semaphore(context, name));
        }

        ~Semaphore();

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Semaphore(Context& context, const string& name);

        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;
    };
}

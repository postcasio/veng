#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Semaphore
    {
    public:
        // Unique, not Ref: a single-owner synchronization primitive (see the
        // Create return-type rule in Veng.h) — nothing else holds a reference.
        static Unique<Semaphore> Create(const string& name)
        {
            return CreateUnique<Semaphore>(name);
        }

        explicit Semaphore(const string& name);
        ~Semaphore();

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
    };
}

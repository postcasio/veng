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
            return Unique<Semaphore>(new Semaphore(name));
        }

        ~Semaphore();

        Semaphore(const Semaphore&) = delete;
        Semaphore& operator=(const Semaphore&) = delete;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit Semaphore(const string& name);

        string m_Name;
        Unique<Native> m_Native;
    };
}

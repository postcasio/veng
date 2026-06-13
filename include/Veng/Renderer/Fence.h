#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Fence
    {
    public:
        // Unique, not Ref: a single-owner synchronization primitive (see the
        // Create return-type rule in Veng.h) — nothing else holds a reference.
        static Unique<Fence> Create(const string& name, bool signaled = false)
        {
            return Unique<Fence>(new Fence(name, signaled));
        }

        ~Fence();

        Fence(const Fence&) = delete;
        Fence& operator=(const Fence&) = delete;

        void Wait() const;
        void Reset() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        explicit Fence(const string& name, bool signaled);

        string m_Name;
        Unique<Native> m_Native;
    };
}

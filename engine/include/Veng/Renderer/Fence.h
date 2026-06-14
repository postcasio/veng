#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    class Fence
    {
    public:
        // Unique, not Ref: a single-owner synchronization primitive (see the
        // Create return-type rule in Veng.h) — nothing else holds a reference.
        static Unique<Fence> Create(Context& context, const string& name, bool signaled = false)
        {
            return Unique<Fence>(new Fence(context, name, signaled));
        }

        ~Fence();

        Fence(const Fence&) = delete;
        Fence& operator=(const Fence&) = delete;

        void Wait() const;
        void Reset() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        Fence(Context& context, const string& name, bool signaled);

        Context& m_Context;
        string m_Name;
        Unique<Native> m_Native;
    };
}

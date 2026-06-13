#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Fence
    {
    public:
        static Unique<Fence> Create(const string& name, bool signaled = false)
        {
            return CreateUnique<Fence>(name, signaled);
        }

        explicit Fence(const string& name, bool signaled);
        ~Fence();

        void Wait() const;
        void Reset() const;

        struct Native;
        [[nodiscard]] Native& GetNative() const;

    private:
        string m_Name;
        Unique<Native> m_Native;
    };
}

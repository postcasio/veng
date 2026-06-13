#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Semaphore
    {
    public:
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

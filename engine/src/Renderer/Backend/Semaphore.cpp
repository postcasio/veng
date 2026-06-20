#include <Veng/Renderer/Semaphore.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    Semaphore::Native& Semaphore::GetNative() const
    {
        return *m_Native;
    }

    Semaphore::Semaphore(Context& context, const string& name)
        : m_Context(context), m_Name(name), m_Native(CreateUnique<Native>())
    {
        constexpr vk::SemaphoreCreateInfo semaphoreCreateInfo{};

        m_Native->Semaphore = GetVkDevice(m_Context).createSemaphore(semaphoreCreateInfo).value;

        DebugMarkers::MarkSemaphore(GetVkDevice(m_Context), m_Native->Semaphore, m_Name);
    }

    Semaphore::~Semaphore()
    {
        GetVkDevice(m_Context).destroySemaphore(m_Native->Semaphore);
    }
}

#include <Veng/Renderer/Semaphore.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>


namespace Veng::Renderer
{
    Semaphore::Native& Semaphore::GetNative() const { return *m_Native; }

    Semaphore::Semaphore(const string& name) : m_Name(name), m_Native(CreateUnique<Native>())
    {
        constexpr vk::SemaphoreCreateInfo semaphoreCreateInfo{};

        m_Native->Semaphore = GetVkDevice(Context::Instance()).createSemaphore(semaphoreCreateInfo).value;

        DebugMarkers::MarkSemaphore(m_Native->Semaphore, m_Name);
    }

    Semaphore::~Semaphore()
    {
        GetVkDevice(Context::Instance()).destroySemaphore(m_Native->Semaphore);
    }
}

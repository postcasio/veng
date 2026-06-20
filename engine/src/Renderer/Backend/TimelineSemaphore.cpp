#include <Veng/Renderer/TimelineSemaphore.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    TimelineSemaphore::Native& TimelineSemaphore::GetNative() const { return *m_Native; }

    TimelineSemaphore::TimelineSemaphore(Context& context, const u64 initialValue)
        : m_Context(context), m_Native(CreateUnique<Native>())
    {
        const vk::SemaphoreTypeCreateInfo typeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = initialValue
        };

        const vk::SemaphoreCreateInfo semaphoreCreateInfo{
            .pNext = &typeCreateInfo
        };

        m_Native->Semaphore = GetVkDevice(m_Context).createSemaphore(semaphoreCreateInfo).value;

        DebugMarkers::MarkSemaphore(GetVkDevice(m_Context), m_Native->Semaphore, "Timeline Semaphore");
    }

    TimelineSemaphore::~TimelineSemaphore()
    {
        GetVkDevice(m_Context).destroySemaphore(m_Native->Semaphore);
    }

    void TimelineSemaphore::Signal(const u64 value)
    {
        const vk::SemaphoreSignalInfo signalInfo{
            .semaphore = m_Native->Semaphore,
            .value = value
        };

        VK_ASSERT(GetVkDevice(m_Context).signalSemaphore(signalInfo), "Failed to signal timeline semaphore!");
    }

    void TimelineSemaphore::Wait(const u64 value) const
    {
        const vk::SemaphoreWaitInfo waitInfo{
            .semaphoreCount = 1,
            .pSemaphores = &m_Native->Semaphore,
            .pValues = &value
        };

        VK_ASSERT(GetVkDevice(m_Context).waitSemaphores(waitInfo, std::numeric_limits<u64>::max()),
                  "Failed to wait on timeline semaphore!");
    }

    u64 TimelineSemaphore::GetValue() const
    {
        return GetVkDevice(m_Context).getSemaphoreCounterValue(m_Native->Semaphore).value;
    }
}

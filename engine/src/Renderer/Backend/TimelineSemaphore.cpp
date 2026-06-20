#include <Veng/Renderer/TimelineSemaphore.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>
#include <Veng/Renderer/Backend/Natives.h>

namespace Veng::Renderer
{
    /// @brief Returns the backend-native semaphore handle.
    TimelineSemaphore::Native& TimelineSemaphore::GetNative() const { return *m_Native; }

    /// @brief Creates a Vulkan timeline semaphore with the given initial counter value.
    /// @param context       The owning render context.
    /// @param initialValue  Starting counter value; signal calls must use strictly greater values.
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

    /// @brief Destroys the timeline semaphore immediately.
    ///
    /// Timeline semaphores are destroyed after the transfer queue is drained (WaitIdle or fence wait),
    /// so no deferred destruction is needed.
    TimelineSemaphore::~TimelineSemaphore()
    {
        GetVkDevice(m_Context).destroySemaphore(m_Native->Semaphore);
    }

    /// @brief Signals the timeline semaphore to @p value from the CPU.
    /// @param value  The value to signal; must be greater than the current semaphore value.
    void TimelineSemaphore::Signal(const u64 value)
    {
        const vk::SemaphoreSignalInfo signalInfo{
            .semaphore = m_Native->Semaphore,
            .value = value
        };

        VK_ASSERT(GetVkDevice(m_Context).signalSemaphore(signalInfo), "Failed to signal timeline semaphore!");
    }

    /// @brief Blocks until the timeline semaphore reaches or exceeds @p value.
    /// @param value  The counter value to wait for.
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

    /// @brief Returns the current counter value of the timeline semaphore.
    u64 TimelineSemaphore::GetValue() const
    {
        return GetVkDevice(m_Context).getSemaphoreCounterValue(m_Native->Semaphore).value;
    }
}

#include <Veng/Renderer/Backend/Semaphore.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>


namespace Veng::Renderer
{
    Semaphore::Semaphore(const string& name, const vk::SemaphoreCreateFlags flags) : m_Name(name)
    {
        vk::SemaphoreCreateInfo semaphoreCreateInfo{
            .flags = flags
        };

        m_VkSemaphore = Context::Instance().GetVkDevice().createSemaphore(semaphoreCreateInfo);

        DebugMarkers::MarkSemaphore(m_VkSemaphore, m_Name);
    }

    Semaphore::~Semaphore()
    {
        Context::Instance().GetVkDevice().destroySemaphore(m_VkSemaphore);
    }
}

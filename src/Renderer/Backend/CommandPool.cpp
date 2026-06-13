#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>

namespace Veng::Renderer
{
    CommandPool::CommandPool(Context& context) : m_Context(context)
    {
        m_VkCommandPool = GetVkDevice(m_Context).createCommandPool(
            vk::CommandPoolCreateInfo{
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = m_Context.GetQueueFamilies().GraphicsFamily.value()
            }
        ).value;
    }

    CommandPool::~CommandPool()
    {
        GetVkDevice(m_Context).destroyCommandPool(m_VkCommandPool);
    }
}

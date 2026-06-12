#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    CommandPool::CommandPool()
    {
        m_VkCommandPool = Context::Instance().GetVkDevice().createCommandPool(
            vk::CommandPoolCreateInfo{
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = Context::Instance().GetQueueFamilies().GraphicsFamily.value()
            }
        ).value;
    }

    CommandPool::~CommandPool()
    {
        Context::Instance().GetVkDevice().destroyCommandPool(m_VkCommandPool);
    }
}

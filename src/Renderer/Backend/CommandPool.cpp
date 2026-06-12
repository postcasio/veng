#include <Veng/Renderer/Backend/Context.h>

namespace Veng::Renderer
{
    CommandPool::CommandPool()
    {
        m_VkCommandPool = Context::Instance().GetVkDevice().createCommandPool(
            vk::CommandPoolCreateInfo{
                .queueFamilyIndex = Context::Instance().GetQueueFamilies().GraphicsFamily.value(),
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer
            }
        );
    }

    CommandPool::~CommandPool()
    {
        Context::Instance().GetVkDevice().destroyCommandPool(m_VkCommandPool);
    }
}

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>

namespace Veng::Renderer
{
    CommandPool::CommandPool()
    {
        m_VkCommandPool = GetVkDevice(Context::Instance()).createCommandPool(
            vk::CommandPoolCreateInfo{
                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                .queueFamilyIndex = Context::Instance().GetQueueFamilies().GraphicsFamily.value()
            }
        ).value;
    }

    CommandPool::~CommandPool()
    {
        GetVkDevice(Context::Instance()).destroyCommandPool(m_VkCommandPool);
    }
}

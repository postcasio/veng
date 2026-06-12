#include <Veng/Renderer/Backend/Fence.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    Fence::Fence(const string& name, const vk::FenceCreateFlags flags) : m_Name(name)
    {
        vk::FenceCreateInfo fenceCreateInfo{
            .flags = flags
        };

        m_VkFence = Context::Instance().GetVkDevice().createFence(fenceCreateInfo);

        DebugMarkers::MarkFence(m_VkFence, m_Name);
    }

    Fence::~Fence()
    {
        Context::Instance().GetVkDevice().destroyFence(m_VkFence);
    }

    void Fence::Wait() const
    {
        auto result = Context::Instance().GetVkDevice().waitForFences(m_VkFence, VK_TRUE,
                                                                      std::numeric_limits<u64>::max());

        VK_ASSERT(result, "Failed to wait for fence!");
    }

    void Fence::Reset() const
    {
        Context::Instance().GetVkDevice().resetFences(m_VkFence);
    }
}

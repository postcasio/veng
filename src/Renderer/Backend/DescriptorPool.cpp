#include <Veng/Renderer/Backend/DescriptorPool.h>

#include <Veng/Renderer/Backend/Context.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    DescriptorPool::DescriptorPool(const DescriptorPoolInfo& info)
    {
        const vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo{
            .flags = info.Flags,
            .maxSets = info.MaxSets,
            .poolSizeCount = static_cast<u32>(info.PoolSizes.size()),
            .pPoolSizes = info.PoolSizes.data()
        };

        m_VkDescriptorPool = Context::Instance().GetVkDevice().createDescriptorPool(descriptorPoolCreateInfo).value;

        DebugMarkers::MarkDescriptorPool(m_VkDescriptorPool, info.Name);
    }

    DescriptorPool::~DescriptorPool()
    {
        Context::Instance().GetVkDevice().destroyDescriptorPool(m_VkDescriptorPool);
    }
}

#include <Veng/Renderer/Backend/DescriptorPool.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Backend/DebugMarkers.h>

namespace Veng::Renderer
{
    DescriptorPool::DescriptorPool(Context& context, const DescriptorPoolInfo& info) : m_Context(context)
    {
        const vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo{
            .flags = info.Flags,
            .maxSets = info.MaxSets,
            .poolSizeCount = static_cast<u32>(info.PoolSizes.size()),
            .pPoolSizes = info.PoolSizes.data()
        };

        m_VkDescriptorPool = GetVkDevice(m_Context).createDescriptorPool(descriptorPoolCreateInfo).value;

        DebugMarkers::MarkDescriptorPool(GetVkDevice(m_Context), m_VkDescriptorPool, info.Name);
    }

    DescriptorPool::~DescriptorPool()
    {
        GetVkDevice(m_Context).destroyDescriptorPool(m_VkDescriptorPool);
    }
}

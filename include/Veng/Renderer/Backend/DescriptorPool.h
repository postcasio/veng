#pragma once

#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    struct DescriptorPoolInfo
    {
        string Name;
        u32 MaxSets;
        vector<vk::DescriptorPoolSize> PoolSizes;
        vk::DescriptorPoolCreateFlags Flags{};
    };

    class DescriptorPool
    {
    public:
        static Unique<DescriptorPool> Create(Context& context, const DescriptorPoolInfo& info)
        {
            return CreateUnique<DescriptorPool>(context, info);
        }

        DescriptorPool(Context& context, const DescriptorPoolInfo& info);
        ~DescriptorPool();

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] vk::DescriptorPool GetVkDescriptorPool() const { return m_VkDescriptorPool; }

    private:
        Context& m_Context;
        string m_Name;
        vk::DescriptorPool m_VkDescriptorPool;
    };
}

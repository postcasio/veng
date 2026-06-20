#pragma once

#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Construction parameters for DescriptorPool.
    struct DescriptorPoolInfo
    {
        /// @brief Debug name attached to the pool.
        string Name;
        /// @brief Maximum number of descriptor sets allocatable from this pool.
        u32 MaxSets;
        /// @brief Per-type descriptor budgets.
        vector<vk::DescriptorPoolSize> PoolSizes;
        /// @brief Pool creation flags (e.g. FreeDescriptorSet).
        vk::DescriptorPoolCreateFlags Flags;
    };

    /// @brief RAII wrapper around a vk::DescriptorPool.
    class DescriptorPool
    {
    public:
        /// @brief Creates and returns a DescriptorPool from @p info.
        static Unique<DescriptorPool> Create(Context& context, const DescriptorPoolInfo& info)
        {
            return CreateUnique<DescriptorPool>(context, info);
        }

        DescriptorPool(Context& context, const DescriptorPoolInfo& info);
        ~DescriptorPool();

        /// @brief Returns the debug name given at construction.
        [[nodiscard]] const string& GetName() const { return m_Name; }
        /// @brief Returns the underlying Vulkan descriptor pool handle.
        [[nodiscard]] vk::DescriptorPool GetVkDescriptorPool() const { return m_VkDescriptorPool; }

    private:
        Context& m_Context;
        string m_Name;
        vk::DescriptorPool m_VkDescriptorPool;
    };
}

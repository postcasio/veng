#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief RAII wrapper around a single vk::CommandPool.
    class CommandPool
    {
    public:
        /// @brief Creates and returns a CommandPool for the given context.
        static Unique<CommandPool> Create(Context& context)
        {
            return CreateUnique<CommandPool>(context);
        }

        /// @brief Returns the underlying Vulkan command pool handle.
        [[nodiscard]] vk::CommandPool GetVkCommandPool() const { return m_VkCommandPool; }

        explicit CommandPool(Context& context);
        ~CommandPool();

    private:
        Context& m_Context;
        vk::CommandPool m_VkCommandPool;
    };
}

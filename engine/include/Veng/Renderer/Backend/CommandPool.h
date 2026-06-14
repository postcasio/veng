#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class Context;

    class CommandPool
    {
    public:
        static Unique<CommandPool> Create(Context& context)
        {
            return CreateUnique<CommandPool>(context);
        }

        [[nodiscard]] vk::CommandPool GetVkCommandPool() const { return m_VkCommandPool; }

        explicit CommandPool(Context& context);
        ~CommandPool();

    private:
        Context& m_Context;
        vk::CommandPool m_VkCommandPool;
    };
}

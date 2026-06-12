#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class CommandPool
    {
    public:
        static Unique<CommandPool> Create()
        {
            return CreateUnique<CommandPool>();
        }

        [[nodiscard]] vk::CommandPool GetVkCommandPool() const { return m_VkCommandPool; }

        CommandPool();
        ~CommandPool();

    protected:


    private:
        vk::CommandPool m_VkCommandPool;
    };
}

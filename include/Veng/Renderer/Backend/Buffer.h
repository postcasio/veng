#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    class CommandBuffer;

    struct BufferInfo
    {
        string Name;
        u64 Size;
        vk::BufferUsageFlags Usage;
    };

    class Buffer
    {
    public:
        static Ref<Buffer> Create(const BufferInfo& info)
        {
            return CreateRef<Buffer>(info);
        }

        explicit Buffer(const BufferInfo& info);
        ~Buffer();

        void Upload(std::span<u8> data) const;
        [[nodiscard]] std::span<u8> Download() const;

        [[nodiscard]] vk::Buffer GetVkBuffer() const { return m_VkBuffer; }
        [[nodiscard]] VmaAllocation GetVmaAllocation() const { return m_VmaAllocation; }
        [[nodiscard]] string GetName() const { return m_Name; }

    private:
        string m_Name;

        vk::Buffer m_VkBuffer;
        VmaAllocation m_VmaAllocation{};
        u64 m_Size;
    };
}

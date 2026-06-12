#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/Vulkan.h>

namespace Veng::Renderer
{
    enum class VertexElementDataType
    {
        None = 0,
        Float,
        Float2,
        Float3
    };

    struct VertexBufferElementDefinition
    {
        VertexElementDataType Type;
        const char* Name;
    };

    struct VertexBufferElement
    {
        VertexElementDataType Type;
        std::string Name;
        u32 Size;
        u32 Offset;

        VertexBufferElement(VertexElementDataType type, const std::string& name);
    };

    vk::Format VertexElementDataTypeToVulkanFormat(VertexElementDataType type);

    class VertexBufferLayout
    {
    public:
        VertexBufferLayout(const std::initializer_list<VertexBufferElement>& elements);
        VertexBufferLayout(const std::vector<VertexBufferElement>& elements);

        [[nodiscard]] const std::vector<VertexBufferElement>& GetElements() const
        {
            return m_Elements;
        }

        [[nodiscard]] u32 GetStride() const
        {
            return m_Stride;
        }
        
        [[nodiscard]] u32 GetFloatCount() const { return m_FloatCount; }

        static Unique<VertexBufferLayout> Create(const std::initializer_list<VertexBufferElement>& elements)
        {
            return CreateUnique<VertexBufferLayout>(elements);
        }

    private:
        std::vector<VertexBufferElement> m_Elements = {};
        u32 m_Stride = 0; // total size in bytes
        u32 m_FloatCount = 0; // total number of 32-bit components comprising one vertex
    };
}

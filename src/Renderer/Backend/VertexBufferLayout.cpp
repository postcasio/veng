#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#include <utility>

namespace Veng::Renderer
{
    // Declared here (not in the public header) — only used internally by the
    // pipeline .cpp files via the forward declaration below.
    vk::Format VertexElementDataTypeToVulkanFormat(const VertexElementDataType type)
    {
        switch (type)
        {
        case VertexElementDataType::Float:
            return vk::Format::eR32Sfloat;
        case VertexElementDataType::Float2:
            return vk::Format::eR32G32Sfloat;
        case VertexElementDataType::Float3:
            return vk::Format::eR32G32B32Sfloat;
        default:
            VE_ASSERT(false, "Unknown vertex element data type");
        }
    }

    static u32 GetVertexElementDataTypeSize(const VertexElementDataType type)
    {
        switch (type)
        {
        case VertexElementDataType::Float:
            return 4;
        case VertexElementDataType::Float2:
            return 4 * 2;
        case VertexElementDataType::Float3:
            return 4 * 3;
        default:
            VE_ASSERT(false, "Unknown VertexElementDataType");
        }
    }

    static u32 GetVertexElementDataTypeComponentCount(const VertexElementDataType type)
    {
        switch (type)
        {
        case VertexElementDataType::Float: return 1;
        case VertexElementDataType::Float2: return 2;
        case VertexElementDataType::Float3: return 3;
        default: VE_ASSERT(false, "Unknown VertexElementDataType");
        }
    }

    VertexBufferElement::VertexBufferElement(const VertexElementDataType type, const std::string& name)
        : Type(type), Name(name), Size(GetVertexElementDataTypeSize(type)), Offset(0)
    {
    }

    VertexBufferLayout::VertexBufferLayout(const std::initializer_list<VertexBufferElement>& elements) : m_Elements(
        elements)
    {
        u32 offset = 0;
        u32 floatCount = 0;
        for (auto& element : m_Elements)
        {
            element.Offset = offset;
            offset += element.Size;
            floatCount += GetVertexElementDataTypeComponentCount(element.Type);
        }

        m_Stride = offset;
        m_FloatCount = floatCount;
    }

    VertexBufferLayout::VertexBufferLayout(const std::vector<VertexBufferElement>& elements) : m_Elements(elements)
    {
        u32 offset = 0;
        u32 floatCount = 0;
        for (auto& element : m_Elements)
        {
            element.Offset = offset;
            offset += element.Size;
            floatCount += GetVertexElementDataTypeComponentCount(element.Type);
        }
        m_Stride = offset;
        m_FloatCount = floatCount;
    }
}

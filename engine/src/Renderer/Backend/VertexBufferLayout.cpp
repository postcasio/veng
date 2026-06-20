#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#include <utility>

namespace Veng::Renderer
{
    /// @brief Returns the byte size of a vertex element format.
    ///
    /// Vertex element formats are the float/vecN subset of Format that vertex shader
    /// inputs accept. Vulkan format mapping is in TypeMapping.h; only byte size (for
    /// buffer layout) is local here.
    static u32 GetFormatSize(const Format format)
    {
        switch (format)
        {
        case Format::R32Sfloat:
            return 4;
        case Format::RG32Sfloat:
            return 4 * 2;
        case Format::RGB32Sfloat:
            return 4 * 3;
        case Format::RGBA32Sfloat:
            return 4 * 4;
        default:
            VE_ASSERT(false, "Unknown vertex element Format");
        }
    }

    /// @brief Returns the number of scalar float components in a vertex element format.
    static u32 GetFormatComponentCount(const Format format)
    {
        switch (format)
        {
        case Format::R32Sfloat: return 1;
        case Format::RG32Sfloat: return 2;
        case Format::RGB32Sfloat: return 3;
        case Format::RGBA32Sfloat: return 4;
        default: VE_ASSERT(false, "Unknown vertex element Format");
        }
    }

    VertexBufferElement::VertexBufferElement(const Format type, const string& name)
        : Type(type), Name(name), Size(GetFormatSize(type)), Offset(0)
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
            floatCount += GetFormatComponentCount(element.Type);
        }

        m_Stride = offset;
        m_FloatCount = floatCount;
    }

    VertexBufferLayout::VertexBufferLayout(const vector<VertexBufferElement>& elements) : m_Elements(elements)
    {
        u32 offset = 0;
        u32 floatCount = 0;
        for (auto& element : m_Elements)
        {
            element.Offset = offset;
            offset += element.Size;
            floatCount += GetFormatComponentCount(element.Type);
        }
        m_Stride = offset;
        m_FloatCount = floatCount;
    }
}

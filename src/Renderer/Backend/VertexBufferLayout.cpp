#include <Veng/Renderer/VertexBufferLayout.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Backend/Vulkan.h>

#include <utility>

namespace Veng::Renderer
{
    // Vertex element formats are a small subset of Format — the plain
    // float/vecN attributes a vertex shader input can be. Vulkan format
    // mapping comes from the engine-wide ToVk(Format) in TypeMapping.h; only
    // size/component-count (needed to lay out the buffer) are local to vertex
    // elements.
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

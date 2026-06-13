#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    struct VertexBufferElement
    {
        Format Type;
        string Name;
        u32 Size;
        u32 Offset;

        VertexBufferElement(Format type, const string& name);
    };

    class VertexBufferLayout
    {
    public:
        VertexBufferLayout(const std::initializer_list<VertexBufferElement>& elements);
        VertexBufferLayout(const vector<VertexBufferElement>& elements);

        [[nodiscard]] const vector<VertexBufferElement>& GetElements() const
        {
            return m_Elements;
        }

        [[nodiscard]] u32 GetStride() const
        {
            return m_Stride;
        }

        [[nodiscard]] u32 GetFloatCount() const { return m_FloatCount; }

    private:
        vector<VertexBufferElement> m_Elements = {};
        u32 m_Stride = 0; // total size in bytes
        u32 m_FloatCount = 0; // total number of 32-bit components comprising one vertex
    };
}

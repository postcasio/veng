#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    /// @brief One typed attribute element in a vertex buffer layout.
    struct VertexBufferElement
    {
        /// @brief Attribute format (determines byte width and interpretation).
        Format Type;
        /// @brief Attribute name, matched against the shader's reflected input name.
        string Name;
        /// @brief Byte size of this attribute derived from Type.
        u32 Size;
        /// @brief Byte offset from the start of one vertex record.
        u32 Offset;

        /// @brief Constructs an element from a format and name; computes Size and Offset automatically.
        VertexBufferElement(Format type, const string& name);
    };

    /// @brief Describes the per-vertex attribute layout of a vertex buffer.
    ///
    /// Used to build the Vulkan vertex-input state for a graphics pipeline. Compute
    /// Stride (total vertex size in bytes) and the attribute offset table from a list
    /// of VertexBufferElement descriptors.
    class VertexBufferLayout
    {
    public:
        /// @brief Constructs a layout from an initializer list of attribute elements.
        VertexBufferLayout(const std::initializer_list<VertexBufferElement>& elements);
        /// @brief Constructs a layout from a vector of attribute elements.
        VertexBufferLayout(const vector<VertexBufferElement>& elements);

        /// @brief Returns the ordered list of attribute elements.
        [[nodiscard]] const vector<VertexBufferElement>& GetElements() const { return m_Elements; }

        /// @brief Returns the total byte size of one vertex record.
        [[nodiscard]] u32 GetStride() const { return m_Stride; }

        /// @brief Returns the total number of 32-bit scalar components comprising one vertex.
        [[nodiscard]] u32 GetFloatCount() const { return m_FloatCount; }

    private:
        /// @brief Ordered attribute elements.
        vector<VertexBufferElement> m_Elements;
        /// @brief Total vertex byte size.
        u32 m_Stride = 0;
        /// @brief Total 32-bit scalar components per vertex.
        u32 m_FloatCount = 0;
    };
}

#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

// Mesh: a cooked mesh's GPU buffers + draw ranges. A vertex
// buffer + u32 index buffer in veng's fixed canonical vertex layout
// (position/normal/tangent/uv, v1), plus a submesh table — each submesh a
// (index range, material AssetId) draw range. The mesh does not load its
// materials; submesh ids are forward references the caller resolves explicitly.
namespace Veng::Renderer
{
    // One draw range within a mesh's index buffer, with the material it was
    // authored against. Material is a forward AssetId reference (0 = unassigned)
    // — the mesh does not eagerly load it.
    struct SubMesh
    {
        u32 IndexOffset = 0;
        u32 IndexCount = 0;
        AssetId Material;
    };

    struct MeshInfo
    {
        string Name;
        Ref<Buffer> VertexBuffer;
        Ref<Buffer> IndexBuffer;
        VertexBufferLayout Layout;
        IndexType IndexType = IndexType::U32;
        u32 IndexCount = 0;
        vector<SubMesh> SubMeshes;
    };

    class Mesh
    {
    public:
        // Well-known AssetId of the canonical layout in the embedded core pack
        // (canonical = position/normal/tangent/uv; tangent is a vec4 whose w is
        // the bitangent handedness sign). Must match core.vengpack.json.
        static constexpr AssetId CanonicalLayoutId{5603155022528551788ULL};

        static Ref<Mesh> Create(const MeshInfo& info)
        {
            return Ref<Mesh>(new Mesh(info));
        }

        // veng's fixed canonical vertex layout v1 (position/normal/tangent/uv,
        // all 32-bit float, 48-byte stride). Tangent is a vec4: xyz is the
        // tangent, w is the bitangent handedness sign (±1), so shaders
        // reconstruct the bitangent as cross(N, T.xyz) * T.w. The cooker writes
        // meshes in this layout and the loader validates each cooked mesh
        // against it; pipelines that draw cooked meshes declare it as their
        // VertexBufferLayout.
        [[nodiscard]] static VertexBufferLayout CanonicalLayout()
        {
            return VertexBufferLayout({
                {Format::RGB32Sfloat, "a_Position"},
                {Format::RGB32Sfloat, "a_Normal"},
                {Format::RGBA32Sfloat, "a_Tangent"},
                {Format::RG32Sfloat, "a_UV"},
            });
        }

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const Ref<Buffer>& GetVertexBuffer() const { return m_VertexBuffer; }
        [[nodiscard]] const Ref<Buffer>& GetIndexBuffer() const { return m_IndexBuffer; }
        [[nodiscard]] const VertexBufferLayout& GetLayout() const { return m_Layout; }
        [[nodiscard]] IndexType GetIndexType() const { return m_IndexType; }
        [[nodiscard]] u32 GetIndexCount() const { return m_IndexCount; }
        [[nodiscard]] std::span<const SubMesh> GetSubMeshes() const { return m_SubMeshes; }

    private:
        explicit Mesh(const MeshInfo& info) :
            m_Name(info.Name),
            m_VertexBuffer(info.VertexBuffer),
            m_IndexBuffer(info.IndexBuffer),
            m_Layout(info.Layout),
            m_IndexType(info.IndexType),
            m_IndexCount(info.IndexCount),
            m_SubMeshes(info.SubMeshes)
        {
        }

        string m_Name;
        Ref<Buffer> m_VertexBuffer;
        Ref<Buffer> m_IndexBuffer;
        VertexBufferLayout m_Layout;
        IndexType m_IndexType;
        u32 m_IndexCount;
        vector<SubMesh> m_SubMeshes;
    };
}

namespace Veng
{
    template <>
    struct AssetTypeTrait<Renderer::Mesh>
    {
        static constexpr AssetType Type = AssetType::Mesh;
    };
}

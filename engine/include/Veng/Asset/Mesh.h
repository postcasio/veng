#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Material.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

// Mesh: a cooked mesh's GPU buffers + draw ranges. A vertex
// buffer + u32 index buffer in veng's fixed canonical vertex layout
// (position/normal/tangent/uv, v1), plus a submesh table — each submesh a
// (index range, material index) draw range. The mesh owns a list of resident
// material instances and each submesh indexes into it.
namespace Veng
{
    // One draw range within a mesh's index buffer. MaterialIndex selects an entry
    // in the owning Mesh's material list; NoMaterial leaves the submesh
    // unassigned, in which case the caller binds its own material.
    struct SubMesh
    {
        u32 IndexOffset = 0;
        u32 IndexCount = 0;
        u32 MaterialIndex = NoMaterial;
        static constexpr u32 NoMaterial = ~0u;
    };

    struct MeshInfo
    {
        string Name;
        Ref<Renderer::Buffer> VertexBuffer;
        Ref<Renderer::Buffer> IndexBuffer;
        Renderer::VertexBufferLayout Layout;
        Renderer::IndexType IndexType = Renderer::IndexType::U32;
        u32 IndexCount = 0;
        vector<SubMesh> SubMeshes;
        vector<AssetHandle<Material>> Materials;
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
        [[nodiscard]] static Renderer::VertexBufferLayout CanonicalLayout()
        {
            return Renderer::VertexBufferLayout({
                {Renderer::Format::RGB32Sfloat, "a_Position"},
                {Renderer::Format::RGB32Sfloat, "a_Normal"},
                {Renderer::Format::RGBA32Sfloat, "a_Tangent"},
                {Renderer::Format::RG32Sfloat, "a_UV"},
            });
        }

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const Ref<Renderer::Buffer>& GetVertexBuffer() const { return m_VertexBuffer; }
        [[nodiscard]] const Ref<Renderer::Buffer>& GetIndexBuffer() const { return m_IndexBuffer; }
        [[nodiscard]] const Renderer::VertexBufferLayout& GetLayout() const { return m_Layout; }
        [[nodiscard]] Renderer::IndexType GetIndexType() const { return m_IndexType; }
        [[nodiscard]] u32 GetIndexCount() const { return m_IndexCount; }
        [[nodiscard]] std::span<const SubMesh> GetSubMeshes() const { return m_SubMeshes; }
        [[nodiscard]] std::span<const AssetHandle<Material>> GetMaterials() const { return m_Materials; }

    private:
        explicit Mesh(const MeshInfo& info) :
            m_Name(info.Name),
            m_VertexBuffer(info.VertexBuffer),
            m_IndexBuffer(info.IndexBuffer),
            m_Layout(info.Layout),
            m_IndexType(info.IndexType),
            m_IndexCount(info.IndexCount),
            m_SubMeshes(info.SubMeshes),
            m_Materials(info.Materials)
        {
        }

        string m_Name;
        Ref<Renderer::Buffer> m_VertexBuffer;
        Ref<Renderer::Buffer> m_IndexBuffer;
        Renderer::VertexBufferLayout m_Layout;
        Renderer::IndexType m_IndexType;
        u32 m_IndexCount;
        vector<SubMesh> m_SubMeshes;
        vector<AssetHandle<Material>> m_Materials;
    };

    template <>
    struct AssetTypeTrait<Mesh>
    {
        static constexpr AssetType Type = AssetType::Mesh;
    };
}

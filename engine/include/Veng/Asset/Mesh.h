#pragma once

#include <cstddef>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Material.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

namespace Veng::Renderer
{
    class Context;
}

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

    // One interleaved vertex in the canonical layout (48 bytes). Field order,
    // sizeof, and offsets are statically asserted to match CanonicalLayout():
    // position @0, normal @12, tangent @24, uv @40. Tangent is a vec4 — xyz the
    // tangent, w the bitangent handedness sign (±1), reconstructed in-shader as
    // cross(N, T.xyz) * T.w.
    struct CanonicalVertex
    {
        vec3 Position;
        vec3 Normal;
        vec4 Tangent;
        vec2 UV;
    };

    static_assert(sizeof(CanonicalVertex) == 48);
    static_assert(offsetof(CanonicalVertex, Position) == 0);
    static_assert(offsetof(CanonicalVertex, Normal) == 12);
    static_assert(offsetof(CanonicalVertex, Tangent) == 24);
    static_assert(offsetof(CanonicalVertex, UV) == 40);

    // CPU-side mesh geometry in the canonical layout. Plain data — primitive
    // generators and tests build it with no GPU. Upload it into a GPU Mesh with
    // Mesh::Create(context, data, name).
    struct MeshData
    {
        vector<CanonicalVertex> Vertices;
        vector<u32> Indices;
        // Resident materials the produced Mesh will own; submeshes index this
        // list. Empty = the mesh has no materials.
        vector<AssetHandle<Material>> Materials;
        // Each submesh is a draw range + a MaterialIndex into Materials
        // (SubMesh::NoMaterial = unassigned). Empty → the factory synthesizes
        // one unassigned submesh over [0, Indices.size()).
        vector<SubMesh> SubMeshes;
    };

    struct MeshInfo
    {
        string Name;
        Ref<Renderer::Buffer> VertexBuffer;
        Renderer::IndexBuffer IndexBuffer;
        Renderer::VertexBufferLayout Layout;
        vector<SubMesh> SubMeshes;
        vector<AssetHandle<Material>> Materials;
    };

    class Mesh
    {
    public:
        // Well-known AssetId of the canonical layout in the embedded core pack
        // (canonical = position/normal/tangent/uv; tangent is a vec4 whose w is
        // the bitangent handedness sign). Must match core.vengpack.json.
        static constexpr AssetId CanonicalLayoutId{0x4DC267CE63429B6CULL};

        static Ref<Mesh> Create(const MeshInfo& info)
        {
            return Ref<Mesh>(new Mesh(info));
        }

        // Uploads CPU-side geometry into a resident GPU Mesh in the canonical
        // layout, carrying data.Materials onto the mesh. An empty SubMeshes list
        // synthesizes one unassigned submesh over the whole index range. Uploads
        // with the blocking UploadSync, so the returned Mesh is ready to draw.
        [[nodiscard]] static Ref<Mesh> Create(
            Renderer::Context& context, const MeshData& data, const string& name);

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
        [[nodiscard]] const Renderer::IndexBuffer& GetIndexBuffer() const { return m_IndexBuffer; }
        [[nodiscard]] const Renderer::VertexBufferLayout& GetLayout() const { return m_Layout; }
        [[nodiscard]] Renderer::IndexType GetIndexType() const { return m_IndexBuffer.GetIndexType(); }
        [[nodiscard]] u32 GetIndexCount() const { return static_cast<u32>(m_IndexBuffer.GetIndexCount()); }
        [[nodiscard]] std::span<const SubMesh> GetSubMeshes() const { return m_SubMeshes; }
        [[nodiscard]] std::span<const AssetHandle<Material>> GetMaterials() const { return m_Materials; }

    private:
        explicit Mesh(const MeshInfo& info) :
            m_Name(info.Name),
            m_VertexBuffer(info.VertexBuffer),
            m_IndexBuffer(info.IndexBuffer),
            m_Layout(info.Layout),
            m_SubMeshes(info.SubMeshes),
            m_Materials(info.Materials)
        {
        }

        string m_Name;
        Ref<Renderer::Buffer> m_VertexBuffer;
        Renderer::IndexBuffer m_IndexBuffer;
        Renderer::VertexBufferLayout m_Layout;
        vector<SubMesh> m_SubMeshes;
        vector<AssetHandle<Material>> m_Materials;
    };

    template <>
    struct AssetTypeTrait<Mesh>
    {
        static constexpr AssetType Type = AssetType::Mesh;
    };
}

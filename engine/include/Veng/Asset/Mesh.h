#pragma once

#include <cstddef>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Material.h>
#include <Veng/Math/AABB.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

namespace Veng::Renderer
{
    class Context;
}

namespace Veng
{
    /// @brief One draw range within a mesh's index buffer.
    ///
    /// MaterialIndex selects an entry in the owning Mesh's material list;
    /// NoMaterial leaves the submesh unassigned and the caller binds its own material.
    struct SubMesh
    {
        /// @brief First index in the mesh's index buffer.
        u32 IndexOffset = 0;
        /// @brief Number of indices in this range.
        u32 IndexCount = 0;
        /// @brief Index into the owning Mesh's material list, or NoMaterial.
        u32 MaterialIndex = NoMaterial;
        /// @brief Local-space bound of this range's referenced vertices.
        ///
        /// Folded at load over [IndexOffset, IndexOffset + IndexCount) through the mesh's
        /// vertex positions; never serialized. Lift to world space per instance via
        /// AABB::Transformed(worldMatrix). It is the broadphase's per-submesh leaf granularity.
        AABB Bounds = AABB::Empty();
        /// @brief Sentinel: submesh has no assigned material.
        static constexpr u32 NoMaterial = ~0u;
    };

    /// @brief One interleaved vertex in the canonical layout (48 bytes).
    ///
    /// Field order, sizeof, and offsets are statically asserted to match CanonicalLayout():
    /// position @0, normal @12, tangent @24, uv @40. Tangent is a vec4 — xyz the tangent,
    /// w the bitangent handedness sign (±1), reconstructed in-shader as cross(N, T.xyz) * T.w.
    struct CanonicalVertex
    {
        /// @brief Local-space vertex position.
        vec3 Position;
        /// @brief Local-space vertex normal.
        vec3 Normal;
        /// @brief xyz = tangent, w = bitangent handedness sign (±1).
        vec4 Tangent;
        /// @brief Texture coordinates.
        vec2 UV;
    };

    static_assert(sizeof(CanonicalVertex) == 48);
    static_assert(offsetof(CanonicalVertex, Position) == 0);
    static_assert(offsetof(CanonicalVertex, Normal) == 12);
    static_assert(offsetof(CanonicalVertex, Tangent) == 24);
    static_assert(offsetof(CanonicalVertex, UV) == 40);

    /// @brief CPU-side mesh geometry in the canonical layout.
    ///
    /// Plain data — primitive generators and tests build it with no GPU context.
    /// Upload into a GPU Mesh with Mesh::BuildSync(context, data, name).
    struct MeshData
    {
        /// @brief Vertices in the canonical layout.
        vector<CanonicalVertex> Vertices;
        /// @brief Triangle index list.
        vector<u32> Indices;
        /// @brief Resident materials the produced Mesh will own; submeshes index this list.
        vector<AssetHandle<Material>> Materials;
        /// @brief Draw ranges; empty → the factory synthesizes one unassigned submesh over [0, Indices.size()).
        vector<SubMesh> SubMeshes;
    };

    /// @brief Construction parameters for a GPU Mesh.
    struct MeshInfo
    {
        /// @brief Debug name for the mesh.
        string Name;
        /// @brief Uploaded vertex data.
        Ref<Renderer::Buffer> VertexBuffer;
        /// @brief Uploaded index data.
        Renderer::IndexBuffer IndexBuffer;
        /// @brief Vertex attribute layout.
        Renderer::VertexBufferLayout Layout;
        /// @brief Draw ranges and material indices.
        vector<SubMesh> SubMeshes;
        /// @brief Material instances owned by the mesh.
        vector<AssetHandle<Material>> Materials;
        /// @brief Local/object-space bound of the mesh's vertices, folded from canonical positions by Mesh::ComputeBounds.
        AABB Bounds = AABB::Empty();
    };

    /// @brief Cooked mesh's GPU buffers and draw ranges.
    ///
    /// A vertex buffer and u32 index buffer in the fixed canonical vertex layout
    /// (position/normal/tangent/uv, all 32-bit float, 48-byte stride), plus a submesh table.
    /// Each submesh is a (index range, material index) draw range. The mesh owns a list of
    /// resident material instances and each submesh indexes into it.
    class Mesh
    {
    public:
        /// @brief Well-known AssetId of the canonical VertexLayout asset in the embedded core pack.
        ///
        /// Must match the id assigned in core.vengpack.json.
        static constexpr AssetId CanonicalLayoutId{0x4DC267CE63429B6CULL};

        /// @brief Creates a Mesh directly from a MeshInfo (GPU buffers already uploaded).
        ///
        /// The low-level GPU-object construction step from already-uploaded handles, distinct
        /// from building the asset from CPU geometry (see Build / BuildSync).
        static Ref<Mesh> Create(const MeshInfo& info) { return Ref<Mesh>(new Mesh(info)); }

        /// @brief Uploads CPU-side geometry into a resident GPU Mesh in the canonical layout, blocking.
        ///
        /// Carries data.Materials onto the mesh. An empty SubMeshes list synthesizes one
        /// unassigned submesh over the whole index range. Uses the blocking UploadSync path,
        /// so the returned Mesh is immediately ready to draw.
        /// @see Build  The async sibling that streams the geometry in off the render thread.
        [[nodiscard]] static Ref<Mesh> BuildSync(Renderer::Context& context, const MeshData& data,
                                                 const string& name);

        /// @brief Builds a resident Mesh from CPU geometry off the render thread.
        ///
        /// Submits one worker job that creates the canonical vertex + index buffers, memcpys the
        /// geometry into them (the buffers are HOST_VISIBLE | HOST_COHERENT, so the copy is a
        /// plain memcpy — no staging, no transfer-queue command, no device wait), folds the
        /// bounds, and assembles the Ref<Mesh>. The returned Task yields that Ref; a caller
        /// publishes it to the render thread through the continuation pump (see
        /// AssetManager::Adopt(Task<Ref<T>>)).
        /// @param context The owning render context; the buffers are created on it and must not outlive it.
        /// @param tasks   The task system the worker job runs on.
        /// @param data    CPU geometry, moved into the worker job so it outlives the caller's frame.
        /// @param name    Debug name for the mesh and its buffers.
        /// @return A Task yielding the resident Ref<Mesh>.
        /// @see BuildSync  The blocking sibling that uploads inline and returns a ready Mesh.
        [[nodiscard]] static Task<Ref<Mesh>> Build(Renderer::Context& context, TaskSystem& tasks,
                                                   MeshData data, string name);

        /// @brief Returns the fixed canonical vertex layout (position/normal/tangent/uv, 48-byte stride).
        ///
        /// Tangent is a vec4: xyz is the tangent, w is the bitangent handedness sign (±1).
        /// The cooker writes every mesh in this layout; the loader validates against it;
        /// pipelines that draw cooked meshes declare it as their VertexBufferLayout.
        [[nodiscard]] static Renderer::VertexBufferLayout CanonicalLayout()
        {
            return Renderer::VertexBufferLayout({
                {Renderer::Format::RGB32Sfloat, "a_Position"},
                {Renderer::Format::RGB32Sfloat, "a_Normal"},
                {Renderer::Format::RGBA32Sfloat, "a_Tangent"},
                {Renderer::Format::RG32Sfloat, "a_UV"},
            });
        }

        /// @brief Folds the local-space AABB of typed canonical vertices.
        ///
        /// Zero vertices yields AABB::Empty().
        [[nodiscard]] static AABB ComputeBounds(std::span<const CanonicalVertex> vertices);

        /// @brief Folds the local-space AABB from raw interleaved bytes, reading the leading vec3 Position at offset 0.
        ///
        /// Used by MeshLoader, which operates on raw bytes. Zero vertices yields AABB::Empty().
        [[nodiscard]] static AABB ComputeBounds(std::span<const u8> interleaved, usize stride);

        /// @brief Folds the local-space AABB of a submesh's referenced canonical vertices.
        ///
        /// Walks the index range [indexOffset, indexOffset + indexCount) into vertices.
        /// Zero indices yields AABB::Empty().
        /// @param vertices    The mesh's canonical vertices.
        /// @param indices     The mesh's index list.
        /// @param indexOffset First index in the submesh's range.
        /// @param indexCount  Number of indices in the submesh's range.
        /// @return The local-space bound of the referenced vertices.
        [[nodiscard]] static AABB ComputeSubMeshBounds(std::span<const CanonicalVertex> vertices,
                                                       std::span<const u32> indices,
                                                       u32 indexOffset, u32 indexCount);

        /// @brief Folds a submesh's local-space AABB from raw interleaved vertex bytes via its index range.
        ///
        /// Reads each index in [indexOffset, indexOffset + indexCount) and dereferences the
        /// leading vec3 Position at that vertex's offset (index * stride). Used by MeshLoader,
        /// which operates on raw bytes. Zero indices yields AABB::Empty().
        /// @param interleaved The mesh's interleaved vertex bytes.
        /// @param stride      Per-vertex stride in bytes.
        /// @param indices     The mesh's index list.
        /// @param indexOffset First index in the submesh's range.
        /// @param indexCount  Number of indices in the submesh's range.
        /// @return The local-space bound of the referenced vertices.
        [[nodiscard]] static AABB ComputeSubMeshBounds(std::span<const u8> interleaved,
                                                       usize stride, std::span<const u32> indices,
                                                       u32 indexOffset, u32 indexCount);

        /// @brief Returns the mesh's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the GPU vertex buffer.
        [[nodiscard]] const Ref<Renderer::Buffer>& GetVertexBuffer() const
        {
            return m_VertexBuffer;
        }

        /// @brief Returns the GPU index buffer.
        [[nodiscard]] const Renderer::IndexBuffer& GetIndexBuffer() const { return m_IndexBuffer; }

        /// @brief Returns the vertex attribute layout.
        [[nodiscard]] const Renderer::VertexBufferLayout& GetLayout() const { return m_Layout; }

        /// @brief Returns the index type (always u32 for cooked meshes).
        [[nodiscard]] Renderer::IndexType GetIndexType() const
        {
            return m_IndexBuffer.GetIndexType();
        }

        /// @brief Returns the total number of indices across all submeshes.
        [[nodiscard]] u32 GetIndexCount() const
        {
            return static_cast<u32>(m_IndexBuffer.GetIndexCount());
        }

        /// @brief Returns the submesh draw ranges.
        [[nodiscard]] std::span<const SubMesh> GetSubMeshes() const { return m_SubMeshes; }

        /// @brief Returns the mesh's resident material instances.
        [[nodiscard]] std::span<const AssetHandle<Material>> GetMaterials() const
        {
            return m_Materials;
        }

        /// @brief Returns the mesh's local/object-space bound.
        ///
        /// Lift to world space per instance via AABB::Transformed(worldMatrix).
        [[nodiscard]] const AABB& GetBounds() const { return m_Bounds; }

    private:
        explicit Mesh(const MeshInfo& info)
            : m_Name(info.Name), m_VertexBuffer(info.VertexBuffer), m_IndexBuffer(info.IndexBuffer),
              m_Layout(info.Layout), m_SubMeshes(info.SubMeshes), m_Materials(info.Materials),
              m_Bounds(info.Bounds)
        {
        }

        string m_Name;
        Ref<Renderer::Buffer> m_VertexBuffer;
        Renderer::IndexBuffer m_IndexBuffer;
        Renderer::VertexBufferLayout m_Layout;
        vector<SubMesh> m_SubMeshes;
        vector<AssetHandle<Material>> m_Materials;
        AABB m_Bounds = AABB::Empty();
    };

    /// @brief AssetTypeTrait specialization mapping Mesh to AssetType::Mesh.
    template <>
    struct AssetTypeTrait<Mesh>
    {
        /// @brief The asset type tag for Mesh.
        static constexpr AssetType Type = AssetType::Mesh;
    };
}

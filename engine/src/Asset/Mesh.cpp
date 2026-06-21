#include <Veng/Asset/Mesh.h>

#include <cstring>
#include <span>
#include <utility>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    using namespace Renderer;

    namespace
    {
        // Validates a MeshData against the canonical-layout contract the factories assert on:
        // non-empty geometry, in-range indices, and valid submesh material indices. Misuse is a
        // fatal, run on the calling thread so the abort is eager — clearer than inside a worker.
        void ValidateMeshData(const MeshData& data, const string& name)
        {
            VE_ASSERT(!data.Vertices.empty(), "Mesh::Create: '{}' has no vertices", name);
            VE_ASSERT(!data.Indices.empty(), "Mesh::Create: '{}' has no indices", name);

            const u32 vertexCount = static_cast<u32>(data.Vertices.size());
            for (const u32 index : data.Indices)
            {
                VE_ASSERT(index < vertexCount,
                          "Mesh::Create: '{}' index {} out of range ({} vertices)", name, index,
                          vertexCount);
            }

            for (const SubMesh& subMesh : data.SubMeshes)
            {
                VE_ASSERT(subMesh.MaterialIndex == SubMesh::NoMaterial ||
                              subMesh.MaterialIndex < data.Materials.size(),
                          "Mesh::Create: '{}' submesh MaterialIndex {} out of range ({} materials)",
                          name, subMesh.MaterialIndex, data.Materials.size());
            }
        }

        // Resolves the submesh table — synthesizing one unassigned whole-range submesh when the
        // source has none — and folds each range's local-space bound over the canonical vertices.
        // The bounds are derived at load, never serialized.
        vector<SubMesh> BuildSubMeshes(const MeshData& data)
        {
            vector<SubMesh> subMeshes;
            if (data.SubMeshes.empty())
            {
                subMeshes.push_back(SubMesh{
                    .IndexOffset = 0,
                    .IndexCount = static_cast<u32>(data.Indices.size()),
                    .MaterialIndex = SubMesh::NoMaterial,
                });
            }
            else
            {
                subMeshes = data.SubMeshes;
            }

            for (SubMesh& subMesh : subMeshes)
            {
                subMesh.Bounds = Mesh::ComputeSubMeshBounds(
                    std::span<const CanonicalVertex>(data.Vertices),
                    std::span<const u32>(data.Indices), subMesh.IndexOffset, subMesh.IndexCount);
            }
            return subMeshes;
        }

        // Creates the canonical vertex + index buffers and memcpys the geometry into them through
        // the blocking host-visible path. A Buffer is HOST_VISIBLE | HOST_COHERENT, so this is a
        // plain memcpy with no staging or device wait — legal to run on a worker thread.
        MeshInfo UploadMesh(Context& context, const MeshData& data, const string& name,
                            vector<SubMesh> subMeshes)
        {
            const Ref<Buffer> vertexBuffer =
                Buffer::Create(context, {
                                            .Name = name + " Vertices",
                                            .Size = data.Vertices.size() * sizeof(CanonicalVertex),
                                            .Usage = BufferUsage::Vertex | BufferUsage::TransferDst,
                                        });
            vertexBuffer->UploadSync({reinterpret_cast<const u8*>(data.Vertices.data()),
                                      data.Vertices.size() * sizeof(CanonicalVertex)});

            IndexBuffer indexBuffer =
                IndexBuffer::Create(context, name + " Indices", data.Indices.size());
            indexBuffer.UploadSync(std::span<const u32>(data.Indices));

            return MeshInfo{
                .Name = name,
                .VertexBuffer = vertexBuffer,
                .IndexBuffer = std::move(indexBuffer),
                .Layout = Mesh::CanonicalLayout(),
                .SubMeshes = std::move(subMeshes),
                .Materials = data.Materials,
                .Bounds = Mesh::ComputeBounds(std::span<const CanonicalVertex>(data.Vertices)),
            };
        }
    }

    AABB Mesh::ComputeBounds(std::span<const CanonicalVertex> vertices)
    {
        AABB bounds = AABB::Empty();
        for (const CanonicalVertex& vertex : vertices)
        {
            bounds.Expand(vertex.Position);
        }
        return bounds;
    }

    AABB Mesh::ComputeBounds(std::span<const u8> interleaved, usize stride)
    {
        VE_ASSERT(stride >= sizeof(vec3),
                  "Mesh::ComputeBounds: stride {} smaller than a vec3 position", stride);

        AABB bounds = AABB::Empty();
        for (usize offset = 0; offset + sizeof(vec3) <= interleaved.size(); offset += stride)
        {
            vec3 position;
            std::memcpy(&position, interleaved.data() + offset, sizeof(position));
            bounds.Expand(position);
        }
        return bounds;
    }

    AABB Mesh::ComputeSubMeshBounds(std::span<const CanonicalVertex> vertices,
                                    std::span<const u32> indices, u32 indexOffset, u32 indexCount)
    {
        AABB bounds = AABB::Empty();
        for (u32 i = 0; i < indexCount; ++i)
        {
            const u32 vertexIndex = indices[indexOffset + i];
            bounds.Expand(vertices[vertexIndex].Position);
        }
        return bounds;
    }

    AABB Mesh::ComputeSubMeshBounds(std::span<const u8> interleaved, usize stride,
                                    std::span<const u32> indices, u32 indexOffset, u32 indexCount)
    {
        VE_ASSERT(stride >= sizeof(vec3),
                  "Mesh::ComputeSubMeshBounds: stride {} smaller than a vec3 position", stride);

        AABB bounds = AABB::Empty();
        for (u32 i = 0; i < indexCount; ++i)
        {
            const u32 vertexIndex = indices[indexOffset + i];
            const usize offset = static_cast<usize>(vertexIndex) * stride;
            vec3 position;
            std::memcpy(&position, interleaved.data() + offset, sizeof(position));
            bounds.Expand(position);
        }
        return bounds;
    }

    Ref<Mesh> Mesh::Create(Context& context, const MeshData& data, const string& name)
    {
        ValidateMeshData(data, name);
        return Mesh::Create(UploadMesh(context, data, name, BuildSubMeshes(data)));
    }

    Task<Ref<Mesh>> Mesh::CreateAsync(Context& context, TaskSystem& tasks, MeshData data,
                                      string name)
    {
        // Validate eagerly on the calling thread: a misuse fatal is clearer here than buried
        // inside a worker job.
        ValidateMeshData(data, name);

        return tasks.Submit(
            [&context, data = std::move(data), name = std::move(name)]
            { return Mesh::Create(UploadMesh(context, data, name, BuildSubMeshes(data))); });
    }
}

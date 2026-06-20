#include <Veng/Asset/Mesh.h>

#include <cstring>
#include <span>
#include <utility>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/TypedBuffers.h>

namespace Veng
{
    using namespace Renderer;

    AABB Mesh::ComputeBounds(std::span<const CanonicalVertex> vertices)
    {
        AABB bounds = AABB::Empty();
        for (const CanonicalVertex& vertex : vertices)
            bounds.Expand(vertex.Position);
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

    Ref<Mesh> Mesh::Create(Context& context, const MeshData& data, const string& name)
    {
        VE_ASSERT(!data.Vertices.empty(), "Mesh::Create: '{}' has no vertices", name);
        VE_ASSERT(!data.Indices.empty(), "Mesh::Create: '{}' has no indices", name);

        const u32 vertexCount = static_cast<u32>(data.Vertices.size());
        for (const u32 index : data.Indices)
        {
            VE_ASSERT(index < vertexCount, "Mesh::Create: '{}' index {} out of range ({} vertices)",
                      name, index, vertexCount);
        }

        for (const SubMesh& subMesh : data.SubMeshes)
        {
            VE_ASSERT(subMesh.MaterialIndex == SubMesh::NoMaterial ||
                          subMesh.MaterialIndex < data.Materials.size(),
                      "Mesh::Create: '{}' submesh MaterialIndex {} out of range ({} materials)",
                      name, subMesh.MaterialIndex, data.Materials.size());
        }

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

        return Mesh::Create(MeshInfo{
            .Name = name,
            .VertexBuffer = vertexBuffer,
            .IndexBuffer = std::move(indexBuffer),
            .Layout = Mesh::CanonicalLayout(),
            .SubMeshes = std::move(subMeshes),
            .Materials = data.Materials,
            .Bounds = Mesh::ComputeBounds(std::span<const CanonicalVertex>(data.Vertices)),
        });
    }
}

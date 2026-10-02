#include "MeshLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Skeleton.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    namespace
    {
        // Bridges a cooked attribute's underlying-integer Format to its
        // Veng::Renderer enum — the engine side of the cycle-avoidance rule
        // (CookedBlobs.h). Only the canonical vertex layout's formats are
        // recognized; anything else is a stale/corrupt archive.
        optional<Renderer::Format> BridgeVertexFormat(u32 value)
        {
            switch (value)
            {
            case 8:
                return Renderer::Format::RG32Sfloat;
            case 9:
                return Renderer::Format::RGB32Sfloat;
            case 10:
                return Renderer::Format::RGBA32Sfloat;
            case 20:
                return Renderer::Format::RGBA16Uint;
            default:
                return std::nullopt;
            }
        }

        optional<Renderer::IndexType> BridgeIndexType(u32 value)
        {
            switch (value)
            {
            case 0:
                return Renderer::IndexType::U16;
            case 1:
                return Renderer::IndexType::U32;
            default:
                return std::nullopt;
            }
        }

        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    namespace
    {
        /// @brief What a mesh's completion builds it from, beyond its buffers.
        struct MeshParts
        {
            /// @brief The submesh table, bounds folded.
            vector<Veng::SubMesh> SubMeshes;
            /// @brief The decoded socket table.
            vector<Veng::MeshSocket> Sockets;
            /// @brief The mesh's local-space bound.
            AABB Bounds;
        };
    }

    AssetResult<Detail::ParsedAsset> MeshLoader::Parse(const AssetParseContext& context,
                                                       const AssetId id,
                                                       const std::span<const u8> cooked) const
    {
        if (cooked.size() < sizeof(CookedMeshHeader))
        {
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than CookedMeshHeader"));
        }

        CookedMeshHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedMeshVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("mesh: cooked version {} does not match expected {}",
                                        header.Version, CookedMeshVersion)));
        }

        const optional<Renderer::IndexType> indexType = BridgeIndexType(header.IndexType);
        if (!indexType)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("mesh: unrecognized IndexType {}", header.IndexType)));
        }

        if (*indexType != Renderer::IndexType::U32)
        {
            return std::unexpected(Corrupt(id, "mesh: only u32 indices are supported"));
        }

        // Validate the cooked attribute descriptor against the engine's canonical (static) or
        // skinned layout, selected by SkeletonId: count, per-attribute format + offset, and
        // stride must all match, or it's a stale/corrupt archive (loud, not silent UB).
        const bool skinned = header.SkeletonId != 0;
        const Renderer::VertexBufferLayout canonical =
            skinned ? Veng::Mesh::SkinnedLayout() : Veng::Mesh::CanonicalLayout();
        const vector<Renderer::VertexBufferElement>& elements = canonical.GetElements();

        if (header.AttributeCount != elements.size())
        {
            return std::unexpected(Corrupt(
                id, fmt::format("mesh: attribute count {} does not match canonical layout's {}",
                                header.AttributeCount, elements.size())));
        }

        if (header.VertexStride != canonical.GetStride())
        {
            return std::unexpected(Corrupt(
                id, fmt::format("mesh: vertex stride {} does not match canonical layout's {}",
                                header.VertexStride, canonical.GetStride())));
        }

        // Blob cursor walks header -> attributes -> submeshes -> vertices ->
        // indices; each step bounds-checks before reading.
        usize cursor = sizeof(CookedMeshHeader);

        const usize attributeBytes =
            static_cast<usize>(header.AttributeCount) * sizeof(CookedVertexAttribute);
        if (cooked.size() < cursor + attributeBytes)
        {
            return std::unexpected(
                Corrupt(id, "mesh: cooked blob smaller than attribute descriptor"));
        }

        for (u32 i = 0; i < header.AttributeCount; ++i)
        {
            CookedVertexAttribute attribute;
            std::memcpy(&attribute, cooked.data() + cursor + i * sizeof(CookedVertexAttribute),
                        sizeof(attribute));

            const optional<Renderer::Format> format = BridgeVertexFormat(attribute.Format);
            if (!format || *format != elements[i].Type || attribute.Offset != elements[i].Offset)
            {
                return std::unexpected(Corrupt(
                    id,
                    fmt::format(
                        "mesh: attribute {} (format {}, offset {}) does not match canonical layout",
                        i, attribute.Format, attribute.Offset)));
            }
        }
        cursor += attributeBytes;

        const usize subMeshBytes = static_cast<usize>(header.SubMeshCount) * sizeof(CookedSubMesh);
        if (cooked.size() < cursor + subMeshBytes)
        {
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than submesh table"));
        }

        // Each distinct non-zero cooked submesh material id becomes one entry in the material list,
        // and a dependency; each submesh stores an index into the list (or NoMaterial for id 0). A
        // cooked submesh material id names a MaterialInstance — a material reference is rewritten
        // to a parent's default-instance id at cook time.
        vector<u64> materialIds;

        auto resolveMaterial = [&](u64 materialId) -> AssetResult<u32>
        {
            for (u32 i = 0; i < materialIds.size(); ++i)
            {
                if (materialIds[i] == materialId)
                {
                    return i;
                }
            }
            materialIds.push_back(materialId);
            return static_cast<u32>(materialIds.size() - 1);
        };

        vector<Veng::SubMesh> subMeshes(header.SubMeshCount);
        for (u32 i = 0; i < header.SubMeshCount; ++i)
        {
            CookedSubMesh cookedSubMesh;
            std::memcpy(&cookedSubMesh, cooked.data() + cursor + i * sizeof(CookedSubMesh),
                        sizeof(cookedSubMesh));

            u32 materialIndex = Veng::SubMesh::NoMaterial;
            if (cookedSubMesh.MaterialId != 0)
            {
                const AssetResult<u32> resolved = resolveMaterial(cookedSubMesh.MaterialId);
                if (!resolved)
                {
                    return std::unexpected(resolved.error());
                }
                materialIndex = *resolved;
            }

            subMeshes[i] = Veng::SubMesh{
                .IndexOffset = cookedSubMesh.IndexOffset,
                .IndexCount = cookedSubMesh.IndexCount,
                .MaterialIndex = materialIndex,
            };
        }
        cursor += subMeshBytes;

        // The socket table goes through the one decoder a CPU-only socket read uses too, so a
        // resident mesh and a read of the same blob cannot disagree.
        Result<vector<Veng::MeshSocket>> sockets = ParseCookedMeshSockets(cooked);
        if (!sockets)
        {
            return std::unexpected(Corrupt(id, std::move(sockets.error())));
        }
        cursor += static_cast<usize>(header.SocketCount) * sizeof(CookedMeshSocket);

        const usize vertexBytes = static_cast<usize>(header.VertexCount) * header.VertexStride;
        if (cooked.size() < cursor + vertexBytes)
        {
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than vertex buffer"));
        }

        const std::span<const u8> vertexData = cooked.subspan(cursor, vertexBytes);
        cursor += vertexBytes;

        const usize indexBytes = static_cast<usize>(header.IndexCount) * sizeof(u32);
        if (cooked.size() < cursor + indexBytes)
        {
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than index buffer"));
        }

        const std::span<const u8> indexData = cooked.subspan(cursor, indexBytes);
        const std::span<const u32> indices(reinterpret_cast<const u32*>(indexData.data()),
                                           header.IndexCount);

        // Fold each submesh's local-space bound over its index range through the raw
        // vertex bytes — derived at load, never serialized.
        for (Veng::SubMesh& subMesh : subMeshes)
        {
            subMesh.Bounds = Veng::Mesh::ComputeSubMeshBounds(
                vertexData, header.VertexStride, indices, subMesh.IndexOffset, subMesh.IndexCount);
        }

        // The buffers are host-visible, so each upload is a copy into the mapping — worker-legal,
        // and on an asynchronous load already on a worker.
        const Ref<Renderer::Buffer> vertexBuffer = Renderer::Buffer::Create(
            context.Context,
            {
                .Name = fmt::format("Mesh {} Vertices", id.Value),
                .Size = vertexBytes,
                .Usage = Renderer::BufferUsage::Vertex | Renderer::BufferUsage::TransferDst,
            });
        auto indexBuffer = CreateRef<Renderer::IndexBuffer>(Renderer::IndexBuffer::Create(
            context.Context, fmt::format("Mesh {} Indices", id.Value), header.IndexCount));
        vertexBuffer->UploadSync(vertexData);
        indexBuffer->UploadSync(indices);

        // The materials, then the skeleton of a skinned mesh: resolved as dependencies so a mesh is
        // resident only once it is ready to draw and, skinned, to pose.
        Detail::ParsedAsset parsed;
        for (const u64 materialId : materialIds)
        {
            parsed.Dependencies.push_back(
                {.Type = AssetTypes::MaterialInstance, .Id = AssetId{materialId}});
        }
        if (skinned)
        {
            parsed.Dependencies.push_back(
                {.Type = AssetTypes::Skeleton, .Id = AssetId{header.SkeletonId}});
        }

        auto parts = CreateRef<MeshParts>(MeshParts{
            .SubMeshes = std::move(subMeshes),
            .Sockets = std::move(*sockets),
            .Bounds = Veng::Mesh::ComputeBounds(vertexData, header.VertexStride),
        });
        parsed.Complete = [vertexBuffer, indexBuffer, parts, canonical, skinned, id](
                              AssetManager&, std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            const usize materialCount = skinned ? resolved.size() - 1 : resolved.size();
            vector<AssetHandle<Veng::MaterialInstance>> materials;
            materials.reserve(materialCount);
            for (usize i = 0; i < materialCount; ++i)
            {
                materials.push_back(AssetManager::HandleOf<Veng::MaterialInstance>(resolved[i]));
            }
            const AssetHandle<Veng::Skeleton> skeleton =
                skinned ? AssetManager::HandleOf<Veng::Skeleton>(resolved.back())
                        : AssetHandle<Veng::Skeleton>();

            const Ref<Veng::Mesh> mesh = Veng::Mesh::Create({
                .Name = fmt::format("Mesh {}", id.Value),
                .VertexBuffer = vertexBuffer,
                .IndexBuffer = std::move(*indexBuffer),
                .Layout = canonical,
                .SubMeshes = std::move(parts->SubMeshes),
                .Materials = std::move(materials),
                .Bounds = parts->Bounds,
                .Skeleton = skeleton,
                .Sockets = std::move(parts->Sockets),
            });

            // The empty finalize is what holds the mesh pending until its materials (and skeleton)
            // are resident.
            return Detail::LoadJob{
                .Resource = Detail::RefAny(mesh),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
                .Finalize = []() -> VoidResult { return {}; },
            };
        };
        return parsed;
    }
}

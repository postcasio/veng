#include "MeshLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Renderer/Buffer.h>
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
                case 8: return Renderer::Format::RG32Sfloat;
                case 9: return Renderer::Format::RGB32Sfloat;
                case 10: return Renderer::Format::RGBA32Sfloat;
                default: return std::nullopt;
            }
        }

        optional<Renderer::IndexType> BridgeIndexType(u32 value)
        {
            switch (value)
            {
                case 0: return Renderer::IndexType::U16;
                case 1: return Renderer::IndexType::U32;
                default: return std::nullopt;
            }
        }

        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{.Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob> MeshLoader::Load(
        AssetManager& manager, Renderer::Context& context, TaskSystem& tasks,
        AssetId id, std::span<const u8> cooked, bool async) const
    {
        if (cooked.size() < sizeof(CookedMeshHeader))
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than CookedMeshHeader"));

        CookedMeshHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        const optional<Renderer::IndexType> indexType = BridgeIndexType(header.IndexType);
        if (!indexType)
            return std::unexpected(Corrupt(id, fmt::format("mesh: unrecognized IndexType {}", header.IndexType)));

        if (*indexType != Renderer::IndexType::U32)
            return std::unexpected(Corrupt(id, "mesh: only u32 indices are supported (v1)"));

        // Validate the cooked attribute descriptor against the engine's single
        // canonical layout: count, per-attribute format + offset, and stride
        // must all match, or it's a stale/corrupt archive (loud, not silent UB).
        const Renderer::VertexBufferLayout canonical = Veng::Mesh::CanonicalLayout();
        const vector<Renderer::VertexBufferElement>& elements = canonical.GetElements();

        if (header.AttributeCount != elements.size())
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "mesh: attribute count {} does not match canonical layout's {}",
                header.AttributeCount, elements.size())));
        }

        if (header.VertexStride != canonical.GetStride())
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "mesh: vertex stride {} does not match canonical layout's {}",
                header.VertexStride, canonical.GetStride())));
        }

        // Blob cursor walks header -> attributes -> submeshes -> vertices ->
        // indices; each step bounds-checks before reading.
        usize cursor = sizeof(CookedMeshHeader);

        const usize attributeBytes = static_cast<usize>(header.AttributeCount) * sizeof(CookedVertexAttribute);
        if (cooked.size() < cursor + attributeBytes)
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than attribute descriptor"));

        for (u32 i = 0; i < header.AttributeCount; ++i)
        {
            CookedVertexAttribute attribute;
            std::memcpy(&attribute, cooked.data() + cursor + i * sizeof(CookedVertexAttribute), sizeof(attribute));

            const optional<Renderer::Format> format = BridgeVertexFormat(attribute.Format);
            if (!format || *format != elements[i].Type || attribute.Offset != elements[i].Offset)
            {
                return std::unexpected(Corrupt(id, fmt::format(
                    "mesh: attribute {} (format {}, offset {}) does not match canonical layout",
                    i, attribute.Format, attribute.Offset)));
            }
        }
        cursor += attributeBytes;

        const usize subMeshBytes = static_cast<usize>(header.SubMeshCount) * sizeof(CookedSubMesh);
        if (cooked.size() < cursor + subMeshBytes)
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than submesh table"));

        // Resolve cooked submesh material ids into resident material instances
        // eagerly — mirroring how Material resolves its texture/shader deps. A
        // distinct non-zero id becomes one entry in the mesh's material list;
        // each submesh stores the index of its material (or NoMaterial for id 0).
        // A failed material load fails the mesh load, same as a missing texture
        // fails a material load.
        vector<AssetHandle<Veng::Material>> materials;
        vector<u64> materialIds;

        auto resolveMaterial = [&](u64 materialId) -> AssetResult<u32>
        {
            for (u32 i = 0; i < materialIds.size(); ++i)
            {
                if (materialIds[i] == materialId)
                    return i;
            }

            const AssetResult<AssetHandle<Veng::Material>> result =
                manager.LoadSync<Veng::Material>(AssetId{materialId});
            if (!result)
                return std::unexpected(result.error());

            const u32 index = static_cast<u32>(materials.size());
            materialIds.push_back(materialId);
            materials.push_back(*result);
            return index;
        };

        vector<Veng::SubMesh> subMeshes(header.SubMeshCount);
        for (u32 i = 0; i < header.SubMeshCount; ++i)
        {
            CookedSubMesh cookedSubMesh;
            std::memcpy(&cookedSubMesh, cooked.data() + cursor + i * sizeof(CookedSubMesh), sizeof(cookedSubMesh));

            u32 materialIndex = Veng::SubMesh::NoMaterial;
            if (cookedSubMesh.MaterialId != 0)
            {
                const AssetResult<u32> resolved = resolveMaterial(cookedSubMesh.MaterialId);
                if (!resolved)
                    return std::unexpected(resolved.error());
                materialIndex = *resolved;
            }

            subMeshes[i] = Veng::SubMesh{
                .IndexOffset = cookedSubMesh.IndexOffset,
                .IndexCount = cookedSubMesh.IndexCount,
                .MaterialIndex = materialIndex,
            };
        }
        cursor += subMeshBytes;

        const usize vertexBytes = static_cast<usize>(header.VertexCount) * header.VertexStride;
        if (cooked.size() < cursor + vertexBytes)
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than vertex buffer"));

        const std::span<const u8> vertexData = cooked.subspan(cursor, vertexBytes);
        cursor += vertexBytes;

        const usize indexBytes = static_cast<usize>(header.IndexCount) * sizeof(u32);
        if (cooked.size() < cursor + indexBytes)
            return std::unexpected(Corrupt(id, "mesh: cooked blob smaller than index buffer"));

        const std::span<const u8> indexData = cooked.subspan(cursor, indexBytes);

        const Ref<Renderer::Buffer> vertexBuffer = Renderer::Buffer::Create(context, {
            .Name = fmt::format("Mesh {} Vertices", id.Value),
            .Size = vertexBytes,
            .Usage = Renderer::BufferUsage::Vertex | Renderer::BufferUsage::TransferDst,
        });

        const Ref<Renderer::Buffer> indexBuffer = Renderer::Buffer::Create(context, {
            .Name = fmt::format("Mesh {} Indices", id.Value),
            .Size = indexBytes,
            .Usage = Renderer::BufferUsage::Index | Renderer::BufferUsage::TransferDst,
        });

        // A Buffer is host-visible+coherent, so an upload is a plain memcpy with
        // no GPU command and no device wait. Async runs it off the main thread;
        // sync runs it inline. Either way the data is ready before any draw binds
        // these buffers (a mesh has no bindless registration to defer).
        if (async)
        {
            Task<void> vertexUpload = vertexBuffer->Upload(tasks, vertexData);
            Task<void> indexUpload = indexBuffer->Upload(tasks, indexData);
        }
        else
        {
            vertexBuffer->UploadSync(vertexData);
            indexBuffer->UploadSync(indexData);
        }

        const Ref<Veng::Mesh> mesh = Veng::Mesh::Create({
            .Name = fmt::format("Mesh {}", id.Value),
            .VertexBuffer = vertexBuffer,
            .IndexBuffer = indexBuffer,
            .Layout = canonical,
            .IndexType = *indexType,
            .IndexCount = header.IndexCount,
            .SubMeshes = std::move(subMeshes),
            .Materials = std::move(materials),
        });

        return Detail::LoadJob{.Resource = Detail::RefAny(mesh)};
    }
}

#include "CollisionShapeLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CollisionShape.h>
#include <Veng/Asset/CookedBlobs.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob>
    CollisionShapeLoader::Load(AssetManager& /*manager*/, Renderer::Context& /*context*/,
                               TaskSystem& /*tasks*/, TypeRegistry& /*types*/, const AssetId id,
                               const std::span<const u8> cooked, bool /*async*/) const
    {
        if (cooked.size() < sizeof(CookedCollisionShapeHeader))
        {
            return std::unexpected(Corrupt(
                id, "collision shape: cooked blob smaller than CookedCollisionShapeHeader"));
        }

        CookedCollisionShapeHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedCollisionShapeVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("collision shape: version {} != expected {}",
                                        header.Version, CookedCollisionShapeVersion)));
        }
        if (header.Mode > static_cast<u32>(CookedCollisionGeometry::Mesh))
        {
            return std::unexpected(
                Corrupt(id, fmt::format("collision shape: unknown geometry mode {}", header.Mode)));
        }
        if (header.IndexCount % 3 != 0)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("collision shape: {} indices is not a whole number of triangles",
                                header.IndexCount)));
        }

        const usize pointBytes = static_cast<usize>(header.PointCount) * 3 * sizeof(f32);
        const usize indexBytes = static_cast<usize>(header.IndexCount) * sizeof(u32);
        if (cooked.size() < sizeof(CookedCollisionShapeHeader) + pointBytes + indexBytes)
        {
            return std::unexpected(
                Corrupt(id, "collision shape: cooked blob smaller than its declared geometry"));
        }

        const Ref<CollisionShape> shape = CreateRef<CollisionShape>();
        shape->Geometry = static_cast<CollisionGeometry>(header.Mode);

        shape->Points.resize(header.PointCount);
        const usize pointCursor = sizeof(CookedCollisionShapeHeader);
        for (u32 i = 0; i < header.PointCount; ++i)
        {
            f32 xyz[3] = {};
            std::memcpy(xyz, cooked.data() + pointCursor + i * sizeof(xyz), sizeof(xyz));
            shape->Points[i] = vec3(xyz[0], xyz[1], xyz[2]);
        }

        shape->Indices.resize(header.IndexCount);
        if (header.IndexCount > 0)
        {
            std::memcpy(shape->Indices.data(), cooked.data() + pointCursor + pointBytes,
                        indexBytes);
        }

        // An index out of range would be read as a vertex address by the solver's shape builder.
        for (const u32 index : shape->Indices)
        {
            if (index >= header.PointCount)
            {
                return std::unexpected(
                    Corrupt(id, fmt::format("collision shape: index {} addresses past {} points",
                                            index, header.PointCount)));
            }
        }

        return Detail::LoadJob{.Resource = Detail::RefAny(shape)};
    }
}

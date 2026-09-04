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
        if (header.Mode > static_cast<u32>(CookedCollisionGeometry::Compound))
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

        const bool compound = header.Mode == static_cast<u32>(CookedCollisionGeometry::Compound);
        if (!compound && header.ChildCount != 0)
        {
            return std::unexpected(
                Corrupt(id, "collision shape: a non-compound shape carries collision children"));
        }

        const usize childBytes =
            static_cast<usize>(header.ChildCount) * sizeof(CookedCollisionChild);
        const usize pointBytes = static_cast<usize>(header.PointCount) * 3 * sizeof(f32);
        const usize indexBytes = static_cast<usize>(header.IndexCount) * sizeof(u32);
        if (cooked.size() <
            sizeof(CookedCollisionShapeHeader) + childBytes + pointBytes + indexBytes)
        {
            return std::unexpected(
                Corrupt(id, "collision shape: cooked blob smaller than its declared geometry"));
        }

        // The point/index region sits after the child table (empty under Convex/Mesh), so read it
        // once into shared buffers and let both the top-level shape and each child slice it.
        const usize pointCursor = sizeof(CookedCollisionShapeHeader) + childBytes;
        vector<vec3> points(header.PointCount);
        for (u32 i = 0; i < header.PointCount; ++i)
        {
            f32 xyz[3] = {};
            std::memcpy(xyz, cooked.data() + pointCursor + i * sizeof(xyz), sizeof(xyz));
            points[i] = vec3(xyz[0], xyz[1], xyz[2]);
        }
        vector<u32> indices(header.IndexCount);
        if (header.IndexCount > 0)
        {
            std::memcpy(indices.data(), cooked.data() + pointCursor + pointBytes, indexBytes);
        }

        const Ref<CollisionShape> shape = CreateRef<CollisionShape>();
        shape->Geometry = static_cast<CollisionGeometry>(header.Mode);

        if (!compound)
        {
            // An index out of range would be read as a vertex address by the solver's shape builder.
            for (const u32 index : indices)
            {
                if (index >= header.PointCount)
                {
                    return std::unexpected(Corrupt(
                        id, fmt::format("collision shape: index {} addresses past {} points", index,
                                        header.PointCount)));
                }
            }
            shape->Points = std::move(points);
            shape->Indices = std::move(indices);
            return Detail::LoadJob{.Resource = Detail::RefAny(shape)};
        }

        shape->Children.resize(header.ChildCount);
        for (u32 c = 0; c < header.ChildCount; ++c)
        {
            CookedCollisionChild raw;
            std::memcpy(&raw,
                        cooked.data() + sizeof(CookedCollisionShapeHeader) +
                            static_cast<usize>(c) * sizeof(CookedCollisionChild),
                        sizeof(raw));

            if (raw.Kind > static_cast<u32>(CookedCollisionChildKind::Mesh))
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("collision shape: child {} has unknown kind {}", c, raw.Kind)));
            }
            if (raw.IndexCount % 3 != 0)
            {
                return std::unexpected(Corrupt(
                    id, fmt::format("collision shape: child {} has {} indices, not whole triangles",
                                    c, raw.IndexCount)));
            }
            if (static_cast<usize>(raw.PointOffset) + raw.PointCount > header.PointCount ||
                static_cast<usize>(raw.IndexOffset) + raw.IndexCount > header.IndexCount)
            {
                return std::unexpected(Corrupt(
                    id,
                    fmt::format("collision shape: child {} geometry slice is out of range", c)));
            }

            CollisionChild& child = shape->Children[c];
            child.Kind = static_cast<CollisionChildKind>(raw.Kind);
            child.Extents = vec3(raw.Extents[0], raw.Extents[1], raw.Extents[2]);
            child.Offset = vec3(raw.Offset[0], raw.Offset[1], raw.Offset[2]);
            child.Rotation =
                quat(raw.Rotation[3], raw.Rotation[0], raw.Rotation[1], raw.Rotation[2]);

            child.Points.assign(points.begin() + raw.PointOffset,
                                points.begin() + raw.PointOffset + raw.PointCount);
            child.Indices.assign(indices.begin() + raw.IndexOffset,
                                 indices.begin() + raw.IndexOffset + raw.IndexCount);
            // A child's indices are 0-based within its own point slice.
            for (const u32 index : child.Indices)
            {
                if (index >= raw.PointCount)
                {
                    return std::unexpected(Corrupt(
                        id, fmt::format("collision shape: child {} index {} addresses past its {} "
                                        "points",
                                        c, index, raw.PointCount)));
                }
            }
        }

        return Detail::LoadJob{.Resource = Detail::RefAny(shape)};
    }
}
